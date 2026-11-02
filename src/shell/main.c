/* NotaTV shell - main program: video, screen stack, main loop */
#include "tv.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static SDL_Window *win;
static SDL_Renderer *ren;
/* Pi in 4K: the UI is drawn at 1080p into this texture and stretched to the display in one go - drawing the whole
   UI directly in 4K only gives 20-30 fps on the Pi 4's GPU */
static SDL_Texture *ui_rt;
static bool running = true;
static bool target_mode; /* running on the TV box (KMS/DRM + evdev) */
static Screen *stack[16];
static int depth;
static char toast_msg[256];
static double toast_t = -10;
static double t0;
static float boot_time = -1;
static int ext_cfg_gen; /* changed from outside (phone page) */
static char ip_str[64] = "";
static double ip_checked = -100;
static bool screensaver;
static double ss_t;

double app_time(void) { return SDL_GetTicks() / 1000.0 - t0; }
void app_quit(void) { running = false; }
float app_boot_time(void) { return boot_time; }
int app_cfg_gen(void) { return ext_cfg_gen; }
void app_cfg_changed(void) { ext_cfg_gen++; }
void app_request_redraw(void) {}

void app_toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_msg, sizeof toast_msg, fmt, ap);
    va_end(ap);
    toast_t = app_time();
    SDL_Log("toast: %s", toast_msg);
}

static bool show_fps(void) {
    static int seen = -1;
    static bool on;
    if (seen != cfg_gen()) { seen = cfg_gen(); on = cfg_int("show_fps", 0) != 0; }
    return on;
}

/* the address is fetched in the background (net ip forks a shell); callers get the last known one */
static bool ip_busy;
static void ip_work(void *arg) {
    char **o = arg;
    char *cmd = str_printf("%s/net ip 2>/dev/null", helper_path()); /* the connection in use (cable before Wi-Fi) */
    *o = run_capture(cmd);
    free(cmd);
}
static void ip_done(void *arg) {
    char **o = arg;
    ip_busy = false;
    if (*o) { str_trim(*o); snprintf(ip_str, sizeof ip_str, "%s", *o); }
    free(*o);
    free(o);
}
const char *app_ip(void) {
    if (!ip_busy && app_time() - ip_checked > 5) {
        char **o = calloc(1, sizeof *o);
        if (o) {
            ip_checked = app_time();
            ip_busy = true;
            job_submit(ip_work, ip_done, o);
        }
    }
    return ip_str;
}

/* ---------- transitions ----------
   Opening: the new screen is drawn into a texture and fades in with a slight zoom on top of the old one.
   Closing: a snapshot of the screen is taken before it is torn down and fades out (zooming in slightly). */
static SDL_Texture *tx_open, *tx_close;
static SDL_Renderer *tx_ren;
static float close_t;       /* 1 -> 0 while the closed snapshot fades out */
static bool close_overlay;
static float ui_dim;        /* black over the whole UI: 1 right after playback, fades to 0 */

static SDL_Texture *main_target(void) { return kms_present_active() ? kms_present_target() : ui_rt; }

static bool tx_ensure(void) {
    if (!ren) return false;
    if (tx_ren != ren) { tx_open = tx_close = NULL; tx_ren = ren; } /* old renderer: the textures are gone */
    for (int i = 0; i < 2; i++) {
        SDL_Texture **t = i ? &tx_close : &tx_open;
        if (*t) continue;
        *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, UI_W, UI_H);
        if (!*t) return false;
        SDL_SetTextureBlendMode(*t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(*t, SDL_SCALEMODE_LINEAR);
    }
    return true;
}

/* the key bar (draw_keybar): the color keys' actions as buttons. ↓ from the bottom row moves focus
   down into the bar (the screen's input returns false for A_DOWN), ← → choose, OK runs it, ↑/Back goes back up.
   The color keys still work as shortcuts. */
static struct {
    const char *label[4];
    Action act[4];
    int n, sel;
    bool focus;
    Screen *owner;    /* the screen that drew the bar */
    unsigned long frame;
} kbar;
static unsigned long frame_no;
static Screen *drawing; /* the screen being drawn right now */

static void draw_screens(int from, int to) {
    for (int i = from; i <= to; i++) if (stack[i]->draw) { drawing = stack[i]; stack[i]->draw(stack[i]); }
    drawing = NULL;
}

static int base_of(int top) {
    int first = top;
    while (first > 0 && stack[first]->overlay) first--;
    return first;
}

static void blit_scaled(SDL_Texture *t, float alpha, float scale) {
    SDL_SetTextureAlphaModFloat(t, alpha);
    float w = UI_W * scale, h = UI_H * scale;
    SDL_FRect d = {(UI_W - w) / 2, (UI_H - h) / 2, w, h};
    SDL_RenderTexture(ren, t, NULL, &d);
}

/* snapshot of what is shown now - called before the top screen is removed */
static void snapshot_close(void) {
    if (!depth || !tx_ensure()) return;
    SDL_Texture *mt = main_target();
    SDL_SetRenderTarget(ren, tx_close);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    SDL_RenderClear(ren);
    int top = depth - 1;
    draw_screens(base_of(top), top);
    SDL_SetRenderTarget(ren, mt);
    close_overlay = stack[top]->overlay;
    close_t = 1;
}

/* ---------- screen stack ---------- */
Screen *app_top(void) { return depth ? stack[depth - 1] : NULL; }

void app_push(Screen *s) {
    if (!s) return;
    if (depth >= 16) { /* full: the oldest screen above home goes, so the new one is really opened */
        SDL_Log("skærmstakken er fuld - lukker %s", stack[1]->name ? stack[1]->name : "?");
        app_close(stack[1]);
    }
    stack[depth++] = s;
    s->anim = 0;
    if (s->enter) s->enter(s);
}

static void destroy_screen(Screen *s) {
    if (s->leave) s->leave(s);
    if (s->destroy) s->destroy(s);
    else { free(s->st); free(s); }
}

void app_pop(void) {
    if (depth <= 1) return;
    snapshot_close();
    Screen *s = stack[--depth];
    destroy_screen(s);
    Screen *t = app_top();
    if (t && t->resume) t->resume(t);
}

/* close one screen wherever it is in the stack (a screen closing itself from update() may not be on top) */
void app_close(Screen *s) {
    if (!s) return;
    if (depth > 1 && stack[depth - 1] == s) { app_pop(); return; }
    for (int i = 1; i < depth - 1; i++)
        if (stack[i] == s) {
            memmove(&stack[i], &stack[i + 1], sizeof stack[0] * (size_t)(depth - 1 - i));
            depth--;
            destroy_screen(s);
            return;
        }
}

/* put another screen in place of s (e.g. the login screen is replaced by the app when signed in) */
void app_replace(Screen *s, Screen *with) {
    if (!with) return;
    for (int i = 1; i < depth; i++)
        if (stack[i] == s) {
            if (i == depth - 1) { app_pop(); app_push(with); return; }
            destroy_screen(s);
            stack[i] = with;
            with->anim = 1;
            if (with->enter) with->enter(with);
            return;
        }
    destroy_screen(with); /* s is not open any more */
}

void app_home(void) {
    if (depth > 1) snapshot_close();
    while (depth > 1) {
        Screen *s = stack[--depth];
        destroy_screen(s);
    }
    Screen *t = app_top();
    if (t && t->resume) t->resume(t);
}

/* ---------- video ---------- */
static bool kms_failed; /* own presentation failed: use SDL's KMSDRM driver */

static bool video_init(void) {
    /* "always HDR": the TV is put in HDR10 before SDL takes the display, and the UI is drawn in PQ */
    bool hdr = hdr_cfg("hdr_always", 0);
    bool kmsdrm = getenv("SDL_VIDEO_DRIVER") && !strcmp(getenv("SDL_VIDEO_DRIVER"), "kmsdrm");
    if (kmsdrm) drm_set_ui_hdr(hdr);
    gfx_set_hdr(hdr);
    /* SDL draws without a display, and we show the frame ourselves with scaling in the display hardware (see
       kms_present.c) - SDL's own KMS path only manages 20 fps in 4K on the Pi 4 */
    bool use_kms = kmsdrm && !kms_failed;
    if (kmsdrm) SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, use_kms ? "offscreen" : "kmsdrm", SDL_HINT_OVERRIDE);
    if (use_kms) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            SDL_Log("offscreen-video fejlede: %s - bruger SDL's KMSDRM", SDL_GetError());
            kms_failed = true;
            return video_init();
        }
        target_mode = true;
        win = SDL_CreateWindow("NotaTV", UI_W, UI_H, 0);
        ren = win ? SDL_CreateRenderer(win, "opengles2") : NULL;
        if (ren && kms_present_init(ren, UI_W, UI_H)) {
            SDL_Log("renderer: %s (egen KMS-præsentation)", SDL_GetRendererName(ren));
            return gfx_init(ren, UI_W, UI_H);
        }
        SDL_Log("egen KMS-præsentation fejlede (%s) - bruger SDL's KMSDRM", SDL_GetError());
        kms_failed = true;
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        ren = NULL; win = NULL;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return video_init();
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_Log("SDL video init fejlede: %s", SDL_GetError());
        return false;
    }
    const char *drv = SDL_GetCurrentVideoDriver();
    SDL_Log("video-driver: %s", drv ? drv : "?");
    target_mode = drv && strcmp(drv, "kmsdrm") == 0;
    int w = 1280, h = 720;
    SDL_WindowFlags fl = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (target_mode) {
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (m) { w = m->w; h = m->h; }
        fl = SDL_WINDOW_FULLSCREEN;
    } else if (getenv("TVSHELL_SIZE")) {
        sscanf(getenv("TVSHELL_SIZE"), "%dx%d", &w, &h);
    }
    win = SDL_CreateWindow("NotaTV", w, h, fl);
    if (!win) { SDL_Log("vindue fejlede: %s", SDL_GetError()); return false; }
    ren = SDL_CreateRenderer(win, NULL);
    if (!ren) { SDL_Log("renderer fejlede: %s", SDL_GetError()); return false; }
    SDL_Log("renderer: %s", SDL_GetRendererName(ren));
    SDL_SetRenderVSync(ren, 1);
    SDL_SetRenderLogicalPresentation(ren, UI_W, UI_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_HideCursor();
    int ow, oh;
    SDL_GetRenderOutputSize(ren, &ow, &oh);
    if (target_mode && ow > UI_W) {
        ui_rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, UI_W, UI_H);
        if (ui_rt) {
            SDL_SetTextureScaleMode(ui_rt, SDL_SCALEMODE_LINEAR);
            SDL_Log("UI tegnes i %dx%d og skaleres til %dx%d", UI_W, UI_H, ow, oh);
            return gfx_init(ren, UI_W, UI_H);
        }
    }
    return gfx_init(ren, ow, oh);
}

static void video_quit(void) {
    gfx_lost();
    kms_present_quit();
    if (ui_rt) SDL_DestroyTexture(ui_rt);
    ui_rt = NULL;
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    ren = NULL;
    win = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void draw_frame(void);

static void present(void) {
    if (kms_present_active()) {
        kms_present_frame(ren);
        return;
    }
    if (ui_rt) {
        SDL_SetRenderTarget(ren, NULL);
        SDL_RenderTexture(ren, ui_rt, NULL, NULL);
    }
    if (!SDL_RenderPresent(ren)) {
        static int present_errors;
        if (present_errors++ < 5) SDL_Log("RenderPresent fejlede: %s", SDL_GetError());
    }
}

/* Put the UI's last frame in the console framebuffer (/dev/fb0). The kernel shows it when nobody has
   the display (between the UI and mpv) - otherwise the screen flashes black when playback starts and stops. */
static void snapshot_to_fbdev(void) {
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi) ||
        (vi.bits_per_pixel != 16 && vi.bits_per_pixel != 32)) { close(fd); return; }
    draw_frame();
    SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
    if (shot) {
        SDL_PixelFormat fmt = vi.bits_per_pixel == 16 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_XRGB8888;
        SDL_Surface *conv = SDL_ConvertSurface(shot, fmt);
        SDL_Surface *scaled = conv && (conv->w != (int)vi.xres || conv->h != (int)vi.yres)
                                  ? SDL_ScaleSurface(conv, (int)vi.xres, (int)vi.yres, SDL_SCALEMODE_LINEAR) : NULL;
        SDL_Surface *src = scaled ? scaled : conv;
        size_t len = (size_t)fi.line_length * vi.yres;
        uint8_t *fb = src ? mmap(NULL, len, PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (fb != MAP_FAILED) {
            int row = src->w * (vi.bits_per_pixel / 8);
            for (int y = 0; y < src->h && y < (int)vi.yres; y++)
                memcpy(fb + (size_t)y * fi.line_length, (uint8_t *)src->pixels + (size_t)y * src->pitch, row);
            munmap(fb, len);
        }
        SDL_DestroySurface(scaled);
        SDL_DestroySurface(conv);
        SDL_DestroySurface(shot);
    }
    close(fd);
}

/* release the display so mpv can take DRM master */
bool app_video_suspend(void) {
    if (!target_mode) return false;
    /* the UI fades to black before playback; the framebuffer is black too, so the transition
       mpv -> UI afterwards goes through black and the UI can fade in again */
    if (ren) {
        double t0 = app_time();
        for (float k = 0; k < 1; k = (float)((app_time() - t0) / 0.22)) {
            ui_dim = k;
            draw_frame();
            present();
        }
    }
    ui_dim = 1;
    snapshot_to_fbdev();
    video_quit();
    return true;
}

void app_video_resume(void) {
    ui_dim = 1; /* fades in from black */
    close_t = 0;
    for (int i = 0; i < 20; i++) {
        if (video_init()) return;
        SDL_Log("video gen-init fejlede, prøver igen");
        video_quit();
        SDL_Delay(250);
    }
    SDL_Log("kan ikke genstarte video - afslutter (init genstarter os)");
    exit(1);
}

/* ---------- shared UI helpers ---------- */
float anim_to(float cur, float target, float dt, float speed) {
    float k = 1.0f - expf(-dt * speed);
    float r = cur + (target - cur) * k;
    if (fabsf(r - target) < 0.001f) r = target;
    return r;
}

void draw_background(void) {
    fill_vgrad_dither(0, 0, UI_W, UI_H, RGB(16, 19, 27), RGB(7, 8, 12));
}

void draw_topbar(const char *title) {
    if (title) text_draw(F_BOLD, 44, 96, 56, C_TEXT, AL_LEFT, title);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof clk, "%H:%M", &tm);
    text_draw(F_SEMI, 34, UI_W - 96, 62, C_TEXT, AL_RIGHT, clk);
}

void draw_hint(float x, float y, const char *keys, const char *label) {
    static const struct { const char *name; Color c; } dots[] = { /* color keys: a dot instead of text */
        {N_("Red"), {226, 62, 58, 255}}, {N_("Green"), {64, 186, 92, 255}}, {N_("Yellow"), {238, 196, 48, 255}}, {N_("Blue"), {58, 124, 226, 255}}};
    /* the English name, or its translation (callers may pass either) */
    for (size_t i = 0; i < sizeof dots / sizeof dots[0]; i++)
        if (!strcmp(keys, dots[i].name) || !strcmp(keys, T(dots[i].name))) {
            fill_circle(x + 15, y + 17, 12, dots[i].c);
            text_draw(F_REG, 22, x + 36, y + 4, C_TEXT2, AL_LEFT, label);
            return;
        }
    float w = text_width(F_SEMI, 20, keys) + 20;
    fill_rrect(x, y, w, 34, 8, color_alpha(C_SURFACE2, 0.9f));
    text_draw(F_SEMI, 20, x + w / 2, y + 5, C_TEXT, AL_CENTER, keys);
    text_draw(F_REG, 22, x + w + 10, y + 4, C_TEXT2, AL_LEFT, label);
}

/* shared hint bar: always the same order OK · Green · Yellow · Blue · Red (NULL = not shown) */
float draw_keybar(float x, float y, const char *ok, const char *green, const char *yellow, const char *blue, const char *red) {
    static const Color dots[4] = {{64, 186, 92, 255}, {238, 196, 48, 255}, {58, 124, 226, 255}, {226, 62, 58, 255}};
    static const Action acts[4] = {A_INFO, A_YELLOW, A_SEARCH, A_MENU};
    const char *l[4] = {green, yellow, blue, red};
    if (ok) { /* OK acts on the selected item - only an explanation */
        draw_hint(x, y, "OK", ok);
        x += text_width(F_SEMI, 20, "OK") + 30 + text_width(F_REG, 22, ok) + 44;
    }
    /* register the bar for the screen that draws it (only the top one can get focus) */
    if (drawing && (kbar.owner != drawing || kbar.frame != frame_no)) { kbar.owner = drawing; kbar.n = 0; }
    kbar.frame = frame_no;
    bool mine = drawing && kbar.owner == drawing;
    int k = 0;
    for (int i = 0; i < 4; i++) {
        if (!l[i]) continue;
        if (mine && k < 4 && kbar.n <= k) { kbar.label[k] = l[i]; kbar.act[k] = acts[i]; kbar.n = k + 1; }
        bool f = mine && kbar.focus && kbar.sel == k && drawing == app_top();
        float w = text_width(F_SEMI, 22, l[i]) + 70, h = 44, by = y - 5;
        fill_rrect(x, by, w, h, h / 2, f ? C_FOCUS : RGBA(255, 255, 255, 28));
        fill_circle(x + 24, by + h / 2, 9, dots[i]);
        text_draw(F_SEMI, 22, x + 42, by + 9, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, l[i]);
        x += w + 16;
        k++;
    }
    return x;
}

bool keybar_focused(void) {
    Screen *t = app_top();
    return kbar.focus && kbar.n && kbar.owner == t && kbar.frame + 2 >= frame_no;
}

void draw_spinner(float cx, float cy, float r, Color c) {
    float t = (float)app_time();
    for (int i = 0; i < 10; i++) {
        float a = t * 5 + i * 0.6283f;
        float al = (float)i / 10;
        fill_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.16f, color_alpha(c, al));
    }
}

void draw_empty(const char *title, const char *msg) {
    text_draw(F_BOLD, 42, UI_W / 2, 430, C_TEXT, AL_CENTER, title);
    text_draw_wrap(F_REG, 28, UI_W / 2 - 520, 500, 1040, 6, 40, C_TEXT2, msg);
}

void draw_buttons(float cx, float y, const char *const *labels, int n, int sel) {
    float w[8], total = 0, gap = 30, h = 72;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) { w[i] = text_width(F_SEMI, 28, labels[i]) + 90; if (w[i] < 260) w[i] = 260; total += w[i]; }
    total += gap * (n - 1);
    float x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        bool f = i == sel;
        if (f) draw_shadow(x, y, w[i], h, h / 2, 20, 1);
        fill_rrect(x, y, w[i], h, h / 2, f ? C_FOCUS : RGB(44, 50, 64));
        text_draw(F_SEMI, 28, x + w[i] / 2, y + (h - 34) / 2, f ? RGB(10, 10, 10) : C_TEXT, AL_CENTER, labels[i]);
        x += w[i] + gap;
    }
}

bool buttons_nav(int *sel, int n, const InputEvent *ev) {
    if (*sel >= n) *sel = n - 1;
    if (*sel < 0) *sel = 0;
    if (ev->a == A_LEFT) { if (*sel == 0) return false; (*sel)--; return true; }
    if (ev->a == A_RIGHT) { if (*sel < n - 1) (*sel)++; return true; }
    return false;
}

/* ---------- inbox (phone web page / scripts) ---------- */
/* phone page: settings and accounts are only saved once they are approved here on the TV */
typedef struct { char *kv; char from[64]; char name[64]; } Pending;
static void pending_cb(void *ud, bool yes) {
    Pending *p = ud;
    if (yes) {
        for (char *line = strtok(p->kv, "\n"); line; line = strtok(NULL, "\n")) {
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '_') continue;
            *eq = 0;
            cfg_set(line, eq + 1);
        }
        ext_cfg_gen++;
        app_toast(T("Saved from phone"));
    } else app_toast(T("Declined"));
    free(p->kv);
    free(p);
}
static void pending_show(char *val) {
    static const struct { const char *k, *label; bool show; } lbl[] = {
        {"iptv_url", N_("Live TV playlist"), true}, {"iptv_epg", N_("Program guide"), true}, {"xtream_server", N_("Xtream server"), true},
        {"xtream_user", N_("Xtream username"), true}, {"xtream_pass", N_("Xtream password"), false}, {"media_share", N_("Network drive"), false},
        {"device_name", N_("Name of the box"), true}};
    Pending *p = calloc(1, sizeof *p);
    p->kv = str_dup(val);
    char msg[512] = "";
    for (char *line = strtok(val, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(line, "_from")) { snprintf(p->from, sizeof p->from, "%s", eq + 1); continue; }
        for (size_t i = 0; i < sizeof lbl / sizeof lbl[0]; i++)
            if (!strcmp(line, lbl[i].k)) {
                char one[160];
                if (lbl[i].show && eq[1]) snprintf(one, sizeof one, "%s: %.70s\n", T(lbl[i].label), eq + 1);
                else snprintf(one, sizeof one, "%s%s\n", T(lbl[i].label), eq[1] ? "" : T(" (cleared)"));
                strncat(msg, one, sizeof msg - strlen(msg) - 1);
            }
    }
    char title[128];
    snprintf(title, sizeof title, T("Save from phone %s?"), p->from);
    confirm_open(title, msg, T("Save"), T("Decline"), pending_cb, p);
}
static char inbox_shot[256]; /* screenshot requested via the inbox (_shot) - for debugging */

static void inbox_check(void) {
    static double last;
    if (app_time() - last < 0.5) return;
    last = app_time();
    DIR *d = opendir("/tmp/notatv-inbox");
    if (!d) return;
    struct dirent *e;
    bool changed = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "/tmp/notatv-inbox/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); /* no symlinks out of the inbox */
        FILE *f = fd >= 0 ? fdopen(fd, "r") : NULL;
        if (!f) { if (fd >= 0) close(fd); unlink(path); continue; }
        char *val = calloc(1, 8192);
        size_t n = fread(val, 1, 8191, f);
        val[n] = 0;
        fclose(f);
        unlink(path);
        size_t l = strlen(val);
        while (l && (val[l - 1] == '\n' || val[l - 1] == '\r')) val[--l] = 0;
        if (!strcmp(e->d_name, "_text")) keyboard_inject(val);
        else if (!strcmp(e->d_name, "_toast")) app_toast("%s", tr_msg(val));
        else if (!strcmp(e->d_name, "_pending")) pending_show(val);
        else if (!strcmp(e->d_name, "_shot")) { /* developer mode only: writes a file to any path */
            if (cfg_int("ssh_enabled", 0)) snprintf(inbox_shot, sizeof inbox_shot, "%s", *val ? val : "/tmp/shot.bmp");
        }
        else if (!strcmp(e->d_name, "_moon")) { /* developer mode only: start tvmoon (test) */
            if (cfg_int("ssh_enabled", 0)) moon_inbox(val);
        }
        else if (!strcmp(e->d_name, "_key")) {
            static const struct { const char *n; Action a; } km[] = {
                {"up", A_UP}, {"down", A_DOWN}, {"left", A_LEFT}, {"right", A_RIGHT}, {"ok", A_OK},
                {"back", A_BACK}, {"home", A_HOME}, {"menu", A_MENU}, {"playpause", A_PLAYPAUSE}};
            for (size_t i = 0; i < sizeof km / sizeof km[0]; i++)
                if (!strcmp(val, km[i].n)) input_push(km[i].a, 0, false);
        } else if (e->d_name[0] == '_') {
            apps_inbox(e->d_name, val); /* e.g. _ytpending: an account for the YouTube app */
        } else {
            cfg_set(e->d_name, val);
            changed = true;
        }
        free(val);
    }
    closedir(d);
    if (changed) {
        ext_cfg_gen++;
        app_toast(T("Settings received"));
    }
}

/* what runs every frame besides the screens - also from the playback loop (playing = true), which has its own loop.
   During playback the apps' services (cast etc.), DLNA, AirPlay and the update reminder wait: they may start
   playback or open screens, and the playing app controls its own cast (rq->tick). */
void app_background_tick(bool playing) {
    jobs_pump();
    inbox_check();
    bt_tick(); /* the Bluetooth agent answers pairing requests */
    appcat_tick(); /* finishes App Store installs; moves boxes from 1.0.x to App Store apps */
    if (playing) return;
    dlna_tick();
    mirror_tick();
    apps_tick(); /* the installed apps' background services (Plex cast, YouTube cast, Jellyfin "Play on") */
    update_tick();
}

/* one frame while the main thread waits (e.g. for ffprobe before playback): the screen keeps drawing with a spinner,
   and Back/Home/Stop cancel. Other keys are ignored. */
bool app_wait_frame(const char *msg) {
    bool cancel = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) { running = false; cancel = true; }
        else input_handle_sdl(&e);
    }
    input_poll();
    jobs_pump();
    InputEvent ev;
    while (input_next(&ev))
        if (ev.a == A_BACK || ev.a == A_HOME || ev.a == A_STOP) cancel = true;
    if (ren) {
        draw_frame();
        if (msg) {
            fill_rrect(0, 0, UI_W, UI_H, 0, RGBA(0, 0, 0, 150));
            draw_spinner(UI_W / 2, UI_H / 2 - 30, 30, C_TEXT);
            text_draw(F_REG, 28, UI_W / 2, UI_H / 2 + 30, C_TEXT2, AL_CENTER, msg);
            text_draw(F_REG, 22, UI_W / 2, UI_H - 70, C_TEXT3, AL_CENTER, T("Back: cancel"));
        }
        present();
    }
    if (!target_mode || !ren) SDL_Delay(16);
    return cancel;
}

/* ---------- test script (automatic navigation + screenshots) ---------- */
static char *script, *script_pos;
static int script_wait;

static void script_step(void) {
    if (!script_pos || !*script_pos) return;
    if (script_wait > 0) { script_wait--; return; }
    char cmd[256];
    char *end = strchr(script_pos, ';');
    size_t n = end ? (size_t)(end - script_pos) : strlen(script_pos);
    if (n >= sizeof cmd) n = sizeof cmd - 1;
    memcpy(cmd, script_pos, n);
    cmd[n] = 0;
    script_pos = end ? end + 1 : script_pos + n;
    if (cmd[0] == 'w' && cmd[1] == ':') script_wait = atoi(cmd + 2);
    else if (cmd[0] == 'q') running = false;
    else if (cmd[0] == 't' && cmd[1] == ':') {
        for (char *p = cmd + 2; *p; p++) input_push(A_CHAR, (unsigned char)*p, false);
    } else if (cmd[0] == 'k' && cmd[1] == ':') {
        static const struct { const char *n; Action a; } km[] = {
            {"up", A_UP}, {"down", A_DOWN}, {"left", A_LEFT}, {"right", A_RIGHT}, {"ok", A_OK},
            {"back", A_BACK}, {"home", A_HOME}, {"menu", A_MENU}, {"info", A_INFO}, {"yellow", A_YELLOW},
            /* color keys from the phone page: the same meaning as the remote (input.c) */
            {"red", A_MENU}, {"green", A_INFO}, {"blue", A_SEARCH}, {"power", A_POWER}, {"bs", A_BACKSPACE}, {"pp", A_PLAYPAUSE}};
        for (size_t i = 0; i < sizeof km / sizeof km[0]; i++)
            if (!strcmp(cmd + 2, km[i].n)) input_push(km[i].a, 0, false);
    }
    /* s:file is taken in the render loop */
}

static const char *script_shot(void) {
    static char path[256];
    if (inbox_shot[0]) { snprintf(path, sizeof path, "%s", inbox_shot); inbox_shot[0] = 0; return path; }
    if (!script_pos || script_pos[0] != 's' || script_pos[1] != ':' || script_wait > 0) return NULL;
    char *end = strchr(script_pos, ';');
    size_t n = end ? (size_t)(end - script_pos - 2) : strlen(script_pos + 2);
    if (n >= sizeof path) n = sizeof path - 1;
    memcpy(path, script_pos + 2, n);
    path[n] = 0;
    script_pos = end ? end + 1 : script_pos + 2 + n;
    return path;
}

/* ---------- toast + pause screen ---------- */
static void draw_toast(void) {
    double age = app_time() - toast_t;
    if (age > 3.5) return;
    float a = age < 0.2 ? (float)(age / 0.2) : (age > 3.0 ? (float)((3.5 - age) / 0.5) : 1.0f);
    float w = text_width(F_SEMI, 28, toast_msg) + 64;
    float x = UI_W / 2 - w / 2, y = UI_H - 150 + (1 - a) * 20;
    draw_shadow(x, y, w, 72, 36, 20, a);
    fill_rrect(x, y, w, 72, 36, color_alpha(RGB(42, 48, 62), a));
    text_draw(F_SEMI, 28, UI_W / 2, y + 19, color_alpha(C_TEXT, a), AL_CENTER, toast_msg);
}

static void draw_screensaver(void) {
    fill_rect(0, 0, UI_W, UI_H, RGB(0, 0, 0));
    double t = app_time() - ss_t;
    float x = UI_W / 2 + sinf((float)t * 0.05f) * 600, y = UI_H / 2 + sinf((float)t * 0.037f + 1) * 300;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16], date[64];
    strftime(clk, sizeof clk, "%H:%M", &tm);
    static const char *days[] = {N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"), N_("Thursday"), N_("Friday"), N_("Saturday")};
    static const char *mon[] = {N_("January"), N_("February"), N_("March"), N_("April"), N_("May"), N_("June"), N_("July"),
                                N_("August"), N_("September"), N_("October"), N_("November"), N_("December")};
    snprintf(date, sizeof date, T("%1$s, %3$s %2$d"), T(days[tm.tm_wday]), tm.tm_mday, T(mon[tm.tm_mon]));
    text_draw(F_SEMI, 120, x, y - 90, RGBA(255, 255, 255, 150), AL_CENTER, clk);
    text_draw(F_REG, 36, x, y + 50, RGBA(255, 255, 255, 110), AL_CENTER, date);
}

/* ---------- FPS display (Settings → System → Show FPS) ---------- */
static float fps_now, fps_worst_ms;
static double draw_ms_sum; /* CPU time in draw_frame (debugging) */

static void fps_tick(void) {
    static double win_t0, prev, worst;
    static int frames;
    double now = app_time();
    if (prev > 0 && now - prev > worst) worst = now - prev;
    prev = now;
    frames++;
    if (now - win_t0 >= 1) {
        if (win_t0 > 0) {
            fps_now = (float)(frames / (now - win_t0));
            fps_worst_ms = (float)(worst * 1000);
            int geom, tex_new, imgs;
            gfx_stats(&geom, &tex_new, &imgs);
            if (show_fps())
                SDL_Log("fps: %.1f, langsomste %.0f ms, cpu/billede %.1f ms, tegnekald/billede %d, billeder/billede %d, nye teksturer %d",
                        fps_now, fps_worst_ms, draw_ms_sum / (frames ? frames : 1), geom / (frames ? frames : 1),
                        imgs / (frames ? frames : 1), tex_new);
            draw_ms_sum = 0;
        }
        win_t0 = now; frames = 0; worst = 0;
    }
}

static void draw_fps(void) {
    if (!show_fps() || fps_now <= 0) return;
    char buf[48];
    snprintf(buf, sizeof buf, "%.0f fps · %.0f ms", fps_now, fps_worst_ms);
    Color c = fps_now >= 55 ? C_OK : fps_now >= 40 ? RGB(240, 190, 60) : C_DANGER;
    fill_rrect(UI_W - 300, 20, 280, 52, 14, RGBA(0, 0, 0, 170));
    text_draw(F_SEMI, 26, UI_W - 160, 32, c, AL_CENTER, buf);
}

static void draw_frame(void) {
    if (kms_present_active()) SDL_SetRenderTarget(ren, kms_present_target());
    else if (ui_rt) SDL_SetRenderTarget(ren, ui_rt);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (screensaver) { draw_screensaver(); return; }
    int top = depth - 1;
    Screen *ts = stack[top];
    if (ts->anim < 0.995f && top > 0 && tx_ensure()) {
        /* opening: what is underneath is drawn as is, the new screen fades in from a texture */
        draw_screens(base_of(top - 1), top - 1);
        SDL_Texture *mt = main_target();
        SDL_SetRenderTarget(ren, tx_open);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);
        drawing = ts;
        if (ts->draw) ts->draw(ts);
        drawing = NULL;
        SDL_SetRenderTarget(ren, mt);
        float e = ts->anim;
        blit_scaled(tx_open, e, ts->overlay ? 0.97f + 0.03f * e : 0.94f + 0.06f * e);
    } else {
        draw_screens(base_of(top), top); /* overlays are drawn on top of the screen underneath */
    }
    if (close_t > 0.01f && tx_close) blit_scaled(tx_close, close_t, close_overlay ? 1.0f : 1.0f + 0.05f * (1 - close_t));
    draw_toast();
    draw_fps();
    if (ui_dim > 0.003f) fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(ui_dim * 255)));
}

static void set_kd_graphics(void) {
    int fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    ioctl(fd, KDSETMODE, KD_GRAPHICS);
    close(fd);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* TZ and NOTATV_LANG exist before any thread starts: changing them later (time zone in Settings) then only
       swaps the string, and environ is never moved while another thread reads it (localtime, getenv) */
    if (!getenv("TZ")) setenv("TZ", "UTC0", 1);
    if (!getenv("NOTATV_LANG")) setenv("NOTATV_LANG", "en", 1);
    SDL_SetHint(SDL_HINT_VIDEO_DOUBLE_BUFFER, "1");
    SDL_SetHint(SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER, "1");
    if (!SDL_Init(SDL_INIT_EVENTS)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    t0 = SDL_GetTicks() / 1000.0;
    cfg_load();
    debug_log_apply(); /* troubleshooting log on: logs go to <data>/logs */
    lang_init(); /* language (before anything is shown) */
    tz_init();  /* time zone from the settings (before anything shows the clock) */
    http_init();
    apps_init(); /* the app register: built-in apps and app packages (after the language: their translations) */
    jobs_init(4);
    bt_init();    /* Bluetooth: agent + afspilningsinfo via D-Bus */
    umask(077); /* logs, configuration and cookies (also from mpv/yt-dlp) for root only */
    if (!video_init()) return 1;
    if (target_mode) set_kd_graphics();
    input_init(target_mode || getenv("TVSHELL_EVDEV"));
    mkdir("/tmp/notatv-inbox", 0700); /* root only (tvshell and the phone page's httpd/CGI) */
    chmod("/tmp/notatv-inbox", 0700);

    script = getenv("TVSHELL_SCRIPT") ? str_dup(getenv("TVSHELL_SCRIPT")) : NULL;
    script_pos = script;

    app_push(home_screen());
    /* first boot: the setup guide (not in automatic UI tests, unless TVSHELL_SETUP=1) */
    if ((!cfg_int("setup_done", 0) || cfg_int("setup_resume", 0)) && (!script || getenv("TVSHELL_SETUP"))) app_push(setup_screen());
    double last = app_time();
    bool first_frame = true;
    int ss_minutes = cfg_int("screensaver_min", 10);
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && ren) {
                int ow, oh;
                SDL_GetRenderOutputSize(ren, &ow, &oh);
                gfx_lost();
                if (ui_rt) gfx_init(ren, UI_W, UI_H);
                else gfx_init(ren, ow, oh);
            } else input_handle_sdl(&e);
        }
        input_poll();
        app_background_tick(false);
        script_step();

        double now = app_time();
        /* another display plugged in (e.g. an SDR monitor instead of the HDR TV): "Always HDR" draws the menus in
           HDR only if the display can show it, so redo the video setup when that changes. Only with a known
           display - a TV in standby can hide its EDID, which must not flip the menus back and forth. */
        static double hdr_check_t;
        if (now - hdr_check_t > 3) {
            hdr_check_t = now;
            if (display_known() && hdr_cfg("hdr_always", 0) != gfx_get_hdr() && app_video_suspend()) app_video_resume();
        }
        float dt = (float)(now - last);
        if (dt > 0.1f) dt = 0.1f;
        last = now;

        InputEvent ev;
        while (input_next(&ev)) {
            if (screensaver) { screensaver = false; continue; }
            if (ev.a == A_POWER) { power_menu_open(); continue; }
            Screen *t = app_top();
            bool bar = t && kbar.n && kbar.owner == t && kbar.frame + 2 >= frame_no;
            if (!bar) kbar.focus = false;
            if (bar && kbar.focus) { /* fokus i knaplinjen */
                if (kbar.sel >= kbar.n) kbar.sel = kbar.n - 1;
                if (ev.a == A_LEFT) { if (kbar.sel > 0) kbar.sel--; continue; }
                if (ev.a == A_RIGHT) { if (kbar.sel < kbar.n - 1) kbar.sel++; continue; }
                if (ev.a == A_DOWN) continue;
                if (ev.a == A_UP || ev.a == A_BACK) { kbar.focus = false; continue; }
                if (ev.a == A_OK) {
                    if (!ev.repeat && t->input) {
                        InputEvent b = {kbar.act[kbar.sel], 0, false};
                        kbar.focus = false; /* the action usually opens something new */
                        t->input(t, &b);
                    }
                    continue;
                }
                kbar.focus = false; /* other keys (e.g. color keys) go to the screen as usual */
            }
            bool used = t && t->input && t->input(t, &ev);
            if (!used && bar && ev.a == A_DOWN) { kbar.focus = true; kbar.sel = 0; continue; }
            if (!used && ev.a == A_HOME) app_home();
            else if (!used && ev.a == A_BACK && depth > 1) app_pop();
            else if (!used && (ev.a == A_CHUP || ev.a == A_CHDOWN) && t && t->input) { /* CH+/CH-: one page up/down */
                InputEvent step = {ev.a == A_CHUP ? A_UP : A_DOWN, 0, false};
                for (int k = 0; k < 4 && app_top() == t; k++) t->input(t, &step); /* stop if the screen closed */
            }
        }
        { /* read again only when a setting changed (cfg_int locks and searches every key) */
            static int seen = -1;
            if (seen != cfg_gen()) { seen = cfg_gen(); ss_minutes = cfg_int("screensaver_min", 10); }
        }
        if (!screensaver && ss_minutes > 0 && now - fmax(input_last_activity(), 0) > ss_minutes * 60.0 && depth >= 1) {
            screensaver = true;
            ss_t = now;
        }

        for (int i = 0; i < depth; i++) {
            Screen *s = stack[i];
            if (i == depth - 1) s->anim = anim_to(s->anim, 1, dt, 11);
            if (s->update && (i == depth - 1 || i == depth - 2)) s->update(s, dt);
        }
        close_t = anim_to(close_t, 0, dt, 10);
        ui_dim = anim_to(ui_dim, 0, dt, 7);
        if (!ren) continue;
        double t_draw = app_time();
        frame_no++;
        draw_frame();
        draw_ms_sum += (app_time() - t_draw) * 1000;
        const char *shot = script_shot();
        if (shot) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
            if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); SDL_Log("screenshot: %s", shot); }
        }
        present();
        fps_tick();
        if (first_frame) {
            first_frame = false;
            FILE *f = fopen("/proc/uptime", "r");
            if (f) {
                if (fscanf(f, "%f", &boot_time) != 1) boot_time = -1;
                fclose(f);
            }
            SDL_Log("første billede efter %.2f s (siden kernel-start)", boot_time);
            FILE *b = fopen("/tmp/notatv-boottime", "w");
            if (b) { fprintf(b, "%.2f\n", boot_time); fclose(b); }
        }
        if (!target_mode) SDL_Delay(screensaver ? 50 : 1);
        else if (screensaver) SDL_Delay(100);
        img_cache_trim();
    }
    cfg_flush();
    while (depth) destroy_screen(stack[--depth]);
    video_quit();
    jobs_shutdown();
    SDL_Quit();
    return 0;
}
