/* NotaTV shell - main program: video, screen stack, main loop */
#include "tv.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <math.h>
#include <signal.h>
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
static float ss_fade; /* 0..1: the screensaver's alpha, glides in and out */

double app_time(void) { return SDL_GetTicks() / 1000.0 - t0; }
/* End the process without exit(): detached threads (jobs, image fetches, app threads) can still be
 * inside an HTTPS request, and exit() runs OpenSSL's atexit cleanup under them -> SIGSEGV in
 * OPENSSL_cleanup. Settings are already saved (cfg_flush), so only stdio needs flushing. */
static _Noreturn void quit_now(int code) {
    fflush(NULL);
    _exit(code);
}
void app_quit(void) { running = false; }
float app_boot_time(void) { return boot_time; }
int app_cfg_gen(void) { return ext_cfg_gen; }
void app_cfg_changed(void) { ext_cfg_gen++; }
void app_request_redraw(void) {}

static void toast_set(const char *fmt, va_list ap) {
    vsnprintf(toast_msg, sizeof toast_msg, fmt, ap);
    toast_t = app_time();
}

void app_toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    toast_set(fmt, ap);
    va_end(ap);
    SDL_Log("toast: %s", toast_msg);
}

void app_toast_secret(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    toast_set(fmt, ap);
    va_end(ap);
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
static double scr_draw_ms; /* time in the screens' own draw functions this frame (slow-frame log) */
static SDL_Texture *tx_open, *tx_close, *tx_under;
static Screen *under_for; /* tx_under holds what was under this screen when it was opened */
static SDL_Renderer *tx_ren;
static float close_t;       /* 1 -> 0 while the closed snapshot fades out */
static bool close_overlay;
static float ui_dim;        /* black over the whole UI: 1 right after playback, fades to 0 */
/* a screen opened from a tile/card grows out of it and shrinks back into it (app_set_origin) */
static struct { float x, y, w, h, r; } org_rect, close_rect;
static bool org_pending, close_to_rect;
static Screen *org_screen;
void app_set_origin(float x, float y, float w, float h, float r) {
    org_rect.x = x; org_rect.y = y; org_rect.w = w; org_rect.h = h; org_rect.r = r;
    org_pending = true;
}
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
/* the full screen at t = 1, the origin rectangle at t = 0 */
static void blit_from_rect(SDL_Texture *t, float alpha, float k, float x, float y, float w, float h, float r) {
    gfx_tex_rrect(t, lerpf(x, 0, k), lerpf(y, 0, k), lerpf(w, UI_W, k), lerpf(h, UI_H, k), lerpf(r, 0, k), alpha);
}

static SDL_Texture *main_target(void) { return kms_present_active() ? kms_present_target() : ui_rt; }

static bool tx_make(SDL_Texture **t) {
    *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, UI_W, UI_H);
    if (!*t) return false;
    SDL_SetTextureBlendMode(*t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(*t, SDL_SCALEMODE_LINEAR);
    return true;
}
static SDL_Texture **tx_missing(void) {
    if (tx_ren != ren) { tx_open = tx_close = tx_under = NULL; under_for = NULL; tx_ren = ren; } /* old renderer: the textures are gone */
    return !tx_open ? &tx_open : !tx_close ? &tx_close : !tx_under ? &tx_under : NULL;
}
static bool tx_ensure(void) {
    if (!ren) return false;
    for (SDL_Texture **t; (t = tx_missing());) if (!tx_make(t)) return false;
    return true;
}
/* the transition textures (3 x 8 MB, 15-20 ms each on a Pi 4) are made one per frame while nothing happens - after start
   and after playback (the renderer is new) - instead of in the frame where the first screen opens or closes */
static void tx_prewarm(void) {
    if (!ren || app_time() < 2 || app_time() - fmax(input_last_activity(), 0) < 0.7) return;
    if (close_t > 0.01f || ui_dim > 0.003f || (depth && stack[depth - 1]->anim < 0.995f)) return;
    SDL_Texture **t = tx_missing();
    if (!t || !tx_make(t)) return;
    SDL_Texture *was = SDL_GetRenderTarget(ren); /* drawn into once, so the memory is really there */
    SDL_SetRenderTarget(ren, *t);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    SDL_RenderClear(ren);
    SDL_SetRenderTarget(ren, was);
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
    for (int i = from; i <= to; i++) if (stack[i]->draw) { drawing = stack[i]; double t0 = app_time(); stack[i]->draw(stack[i]); scr_draw_ms += (app_time() - t0) * 1000; }
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
    close_to_rect = stack[top] == org_screen;
    if (close_to_rect) { close_rect = org_rect; org_screen = NULL; }
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
    /* what is under the new screen is photographed once: while the new screen opens, the old one is not drawn
       again every frame (on the Pi's GPU that is what made the animation stutter) */
    under_for = NULL;
    double pt0 = app_time();
    if (depth > 0 && tx_ensure()) {
        SDL_Texture *mt = main_target();
        SDL_SetRenderTarget(ren, tx_under);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        static unsigned long pushed_in; /* frame_no of the last push: a second push in the same frame draws instead */
        if (mt && pushed_in != frame_no + 1 && kms_present_copy_shown(ren)) {
            /* direct mode: copied from the display buffer on screen */
        } else if (mt && pushed_in != frame_no + 1) {
            /* the frame on screen already is that picture: one copy instead of drawing every screen again
               (opening Live TV over the home screen: 27 ms -> ~1 ms on a Pi 4) */
            SDL_BlendMode bm;
            SDL_GetTextureBlendMode(mt, &bm);
            SDL_SetTextureBlendMode(mt, SDL_BLENDMODE_NONE);
            SDL_RenderTexture(ren, mt, NULL, NULL);
            SDL_SetTextureBlendMode(mt, bm);
        } else draw_screens(base_of(depth - 1), depth - 1);
        pushed_in = frame_no + 1;
        SDL_SetRenderTarget(ren, mt);
        under_for = s;
    }
    double pt1 = app_time();
    stack[depth++] = s;
    s->anim = 0;
    if (org_pending && !s->overlay) { org_screen = s; org_pending = false; }
    if (s->enter) s->enter(s);
    double pt2 = app_time();
    if (pt2 - pt0 > 0.010 && show_fps())
        SDL_Log("åbn %s: billede af skærmen under %.1f ms, enter %.1f ms", s->name ? s->name : "?", (pt1 - pt0) * 1000, (pt2 - pt1) * 1000);
}

static void destroy_screen(Screen *s) {
    if (s == under_for) under_for = NULL;
    if (s == org_screen) org_screen = NULL;
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

static int out_w, out_h; /* the output size gfx was set up for */
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
    out_w = ow; out_h = oh;
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
    /* the transition textures belong to this renderer: destroy them before it goes away */
    if (ren && tx_ren == ren) {
        if (tx_open) SDL_DestroyTexture(tx_open);
        if (tx_close) SDL_DestroyTexture(tx_close);
        if (tx_under) SDL_DestroyTexture(tx_under);
    }
    tx_open = tx_close = tx_under = NULL;
    under_for = NULL;
    tx_ren = NULL;
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

static double resume_t; /* when the UI came back after playback: the first frame after it is logged */
void app_video_resume(void) {
    ui_dim = 1; /* fades in from black */
    close_t = 0;
    for (int i = 0; i < 20; i++) {
        double t0 = app_time();
        if (video_init()) { resume_t = app_time(); SDL_Log("video igen: init %.0f ms", (resume_t - t0) * 1000); return; }
        SDL_Log("video gen-init fejlede, prøver igen");
        video_quit();
        SDL_Delay(250);
    }
    SDL_Log("kan ikke genstarte video - afslutter (init genstarter os)");
    cfg_flush();
    quit_now(1);
}

/* ---------- shared UI helpers ---------- */
float anim_to(float cur, float target, float dt, float speed) {
    float k = 1.0f - expf(-dt * speed);
    float r = cur + (target - cur) * k;
    if (fabsf(r - target) < 0.001f) r = target;
    return r;
}

float spring_to(Spring *s, float target, float dt, float stiffness, float damping) {
    if (dt > 0.1f) dt = 0.1f; /* a long frame (loading) must not throw it */
    int n = (int)ceilf(dt / 0.004f); /* small steps: stable at any frame rate */
    float h = n ? dt / n : 0;
    for (int i = 0; i < n; i++) {
        s->v += (stiffness * (target - s->x) - damping * s->v) * h;
        s->x += s->v * h;
    }
    if (fabsf(s->x - target) < 0.0005f && fabsf(s->v) < 0.01f) { s->x = target; s->v = 0; }
    return s->x;
}

/* ---------- shared focus animation ----------
   ui_focus(key, on) gives 0..1 (with a small spring overshoot) for a card that can have focus, so every screen gets
   the same lift without keeping animation state itself. The key is anything stable per card (the item's pointer).
   Entries that are not asked for in a while are reused. */
static float g_dt = 1 / 60.0f;
static unsigned long g_frame;
typedef struct { const void *key; Spring s; unsigned long seen, stepped; } FocusEnt;
static FocusEnt focus_tab[192];
float focus_amt(const void *key, bool on) {
    FocusEnt *e = NULL, *old = &focus_tab[0];
    for (size_t i = 0; i < sizeof focus_tab / sizeof focus_tab[0]; i++) {
        FocusEnt *t = &focus_tab[i];
        if (t->key == key && t->seen + 30 >= g_frame) { e = t; break; }
        if (t->seen < old->seen) old = t;
    }
    if (!e) {
        if (!on) return 0; /* nothing to animate: an item without focus that was not animating */
        e = old;
        *e = (FocusEnt){key, {0, 0}, g_frame, g_frame - 1};
    }
    e->seen = g_frame;
    if (e->stepped != g_frame) { e->stepped = g_frame; spring_to(&e->s, on ? 1 : 0, g_dt, SPRING_SNAPPY); }
    return e->s.x;
}
/* a position that glides (spring) to target - for focus pills that slide between rows. The first call jumps */
typedef struct { const void *key; Spring s; unsigned long seen, stepped; } SlideEnt;
static SlideEnt slide_tab[48];
float slide_to(const void *key, float target) {
    SlideEnt *e = NULL, *old = &slide_tab[0];
    for (size_t i = 0; i < sizeof slide_tab / sizeof slide_tab[0]; i++) {
        SlideEnt *t = &slide_tab[i];
        if (t->key == key && t->seen + 30 >= g_frame) { e = t; break; }
        if (t->seen < old->seen) old = t;
    }
    if (!e) { e = old; *e = (SlideEnt){key, {target, 0}, g_frame, g_frame}; }
    e->seen = g_frame;
    if (e->stepped != g_frame) { e->stepped = g_frame; spring_to(&e->s, target, g_dt, SPRING_SOFT); }
    return e->s.x;
}
/* a dialog/panel: frosted dark glass with light from above, a thin edge and a deep shadow */
void draw_panel(float x, float y, float w, float h, float r, float a) {
    draw_shadow(x, y, w, h, r, 48, a);
    fill_rrect(x, y, w, h, r, RGBA(26, 30, 40, (uint8_t)(246 * a)));
    fill_rrect_vgrad(x, y, w, h * 0.4f, r, RGBA(255, 255, 255, (uint8_t)(12 * a)), RGBA(255, 255, 255, 0));
    stroke_rrect(x, y, w, h, r, 1.5f, RGBA(255, 255, 255, (uint8_t)(28 * a)));
}

/* under a lifted card: a soft white glow (seen from across the room) and a shadow */
void focus_under(float x, float y, float w, float h, float r, float f) {
    float a = f < 0 ? 0 : f > 1 ? 1 : f;
    if (a < 0.01f) return;
    draw_glow(x, y, w, h, r, 34, RGB(255, 255, 255), 0.16f * a);
    draw_shadow(x, y, w, h, r, 26, 0.95f * a);
}
/* over a lifted card: light from above and a thin bright edge */
void focus_over(float x, float y, float w, float h, float r, float f) {
    float a = f < 0 ? 0 : f > 1 ? 1 : f;
    if (a < 0.01f) return;
    fill_rrect_vgrad(x, y, w, h * 0.5f, r, RGBA(255, 255, 255, (uint8_t)(34 * a)), RGBA(255, 255, 255, 0));
    stroke_rrect(x, y, w, h, r, 2, RGBA(255, 255, 255, (uint8_t)(120 * a)));
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

float draw_chips(float x, float y, const char *const *labels, int n, int sel) {
    for (int i = 0; i < n; i++) {
        if (!labels[i] || !*labels[i]) continue;
        float f = focus_amt(labels[i], i == sel), fc = f < 0 ? 0 : f > 1 ? 1 : f;
        float w = text_width(F_SEMI, 24, labels[i]) + 48, h = 50;
        float sc = 1 + 0.05f * f, bw = w * sc, bh = h * sc, bx = x + (w - bw) / 2, by = y + (h - bh) / 2;
        if (fc > 0.01f) draw_shadow(bx, by, bw, bh, bh / 2, 16, fc);
        fill_rrect(bx, by, bw, bh, bh / 2, color_mix(RGBA(255, 255, 255, 30), C_FOCUS, fc));
        text_draw(F_SEMI, 24, x + w / 2, y + 10, color_mix(C_TEXT, RGB(10, 10, 10), fc), AL_CENTER, labels[i]);
        x += w + 16;
    }
    return x;
}

/* shared action bar at the bottom: always the same order OK · Green · Yellow · Blue · Red (NULL = not shown).
   The buttons are plain (no color dots): they are reached with ↓ and OK, the color keys are only shortcuts */
float draw_keybar(float x, float y, const char *ok, const char *green, const char *yellow, const char *blue, const char *red) {
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
        float w = text_width(F_SEMI, 22, l[i]) + 48, h = 44, by = y - 5;
        fill_rrect(x, by, w, h, h / 2, f ? C_FOCUS : RGBA(255, 255, 255, 28));
        text_draw(F_SEMI, 22, x + w / 2, by + 9, f ? RGB(10, 10, 10) : C_TEXT, AL_CENTER, l[i]);
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
        float f = focus_amt(labels[i], i == sel), fc = f < 0 ? 0 : f > 1 ? 1 : f;
        float sc = 1 + 0.05f * f, bw = w[i] * sc, bh = h * sc, bx = x + (w[i] - bw) / 2, by = y + (h - bh) / 2;
        if (fc > 0.01f) draw_shadow(bx, by, bw, bh, bh / 2, 20, fc);
        fill_rrect(bx, by, bw, bh, bh / 2, color_mix(RGB(44, 50, 64), C_FOCUS, fc));
        text_draw(F_SEMI, 28, x + w[i] / 2, y + (h - 34) / 2, color_mix(C_TEXT, RGB(10, 10, 10), fc), AL_CENTER, labels[i]);
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
        char *sv_line;
        for (char *line = strtok_r(p->kv, "\n", &sv_line); line; line = strtok_r(NULL, "\n", &sv_line)) {
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
    char *sv_line;
    for (char *line = strtok_r(val, "\n", &sv_line); line; line = strtok_r(NULL, "\n", &sv_line)) {
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
        else if (!strcmp(e->d_name, "_wake")) { screensaver = false; input_touch(); } /* "Turn on" on the phone page */
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
    idle_sleep_tick(playing); /* automatic sleep - also while a video plays ("Are you still watching?") */
    if (playing) return;
    dlna_tick();
    mirror_tick();
    apps_tick(); /* the installed apps' background services (YouTube cast, Jellyfin "Play on") */
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

/* ---------- app overlays (app_set_overlay) ---------- */
#define MAX_OVERLAYS 8 /* one per app that shows something in the top bar */
static struct { char id[32]; void (*draw)(void); } overlays[MAX_OVERLAYS];
void app_set_overlay(const char *app_id, void (*draw)(void)) {
    int free_i = -1;
    for (int i = 0; i < MAX_OVERLAYS; i++) {
        if (overlays[i].draw && !strcmp(overlays[i].id, app_id)) { overlays[i].draw = draw; return; }
        if (!overlays[i].draw && free_i < 0) free_i = i;
    }
    if (draw && free_i >= 0) { snprintf(overlays[free_i].id, sizeof overlays[0].id, "%s", app_id); overlays[free_i].draw = draw; }
}
/* The top bar on the home screen. Pills are stacked from the right, in this order: the ones that are always there
   (weather, price, cars), then what comes and goes (Music, Radio), so a song never moves the others. They stop at
   the greeting: one that does not fit is left out. The date gives way first (ui_home.c, home_overlay_left). */
#define BAR_RIGHT (UI_W - 360.0f) /* left of the control center's button and the no-network icon */
static float overlay_left = UI_W, overlay_left_prev = UI_W, overlay_limit;
static bool overlay_hidden;
void home_overlay_limit(float x) { overlay_limit = x; }
float home_overlay_left(void) { return overlay_left_prev; }
float app_overlay_place(float right, float w) {
    if (overlay_limit > 0) right = overlay_left; /* home: plain stacking, whatever the app asked for */
    else if (right > overlay_left) right = overlay_left;
    float x = right - w;
    /* w = 0 asks how far left there is room. Music and Radio compare the answer with their own fixed limit (700):
       on the home screen the real limit is the greeting, so the answer is moved by the difference */
    if (w <= 0) return overlay_limit > 0 ? x + (700 - overlay_limit) : x;
    if (overlay_limit > 0 && x < overlay_limit) { /* no room: this one is not drawn, the bar stays as it is */
        if (!overlay_hidden) { clip_push(0, 0, 0, 0); overlay_hidden = true; }
        return x;
    }
    overlay_left = x - 14;
    return x;
}
static bool overlay_transient(const char *id) { return !strcmp(id, "music") || !strcmp(id, "radio"); }
static void draw_overlays(void) {
    double pos, dur;
    bool paused;
    Screen *top = app_top();
    bool home = top && top->name && !strcmp(top->name, "home");
    if (!home) overlay_limit = 0;
    overlay_left = home ? BAR_RIGHT : UI_W;
    if (!player_state(&pos, &dur, &paused)) /* video: the player's own OSD */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < MAX_OVERLAYS; i++) {
                if (!overlays[i].draw || overlay_transient(overlays[i].id) != pass) continue;
                const NotaApp *a = app_find(overlays[i].id);
                if (!a || !app_installed(a)) continue;
                overlays[i].draw();
                if (overlay_hidden) { clip_pop(); overlay_hidden = false; }
            }
    overlay_left_prev = overlay_left;
}

/* ---------- toast + pause screen ---------- */
static void draw_toast(void) {
    double age = app_time() - toast_t;
    if (age > 3.5) return;
    float a = age < 0.2 ? (float)(age / 0.2) : (age > 3.0 ? (float)((3.5 - age) / 0.5) : 1.0f);
    /* top center, the same pill as the player's messages (notatv-osd.lua) */
    float w = text_width(F_BOLD, 28, toast_msg) + 72;
    float x = UI_W / 2 - w / 2, y = 48 - (1 - a) * 16;
    draw_panel(x, y, w, 66, 33, a);
    text_draw(F_BOLD, 28, UI_W / 2, y + 16, color_alpha(C_TEXT, a), AL_CENTER, toast_msg);
}

static void draw_screensaver(float a) {
    fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(255 * a)));
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
    text_draw(F_SEMI, 120, x, y - 90, RGBA(255, 255, 255, (uint8_t)(150 * a)), AL_CENTER, clk);
    text_draw(F_REG, 36, x, y + 50, RGBA(255, 255, 255, (uint8_t)(110 * a)), AL_CENTER, date);
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
    gfx_frame_begin();
    kms_present_begin(ren);
    if (kms_present_active()) SDL_SetRenderTarget(ren, kms_present_target());
    else if (ui_rt) SDL_SetRenderTarget(ren, ui_rt);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (screensaver && ss_fade >= 0.999f) { draw_screensaver(1); return; }
    int top = depth - 1;
    Screen *ts = stack[top];
    if (ts->anim < 0.995f && top > 0 && tx_ensure()) {
        /* opening: what is underneath (a photo taken when the screen was pushed, darkened by the GPU while it is
           drawn - no extra full-screen layer), and the new screen grows/fades in from a texture */
        float e0 = ts->anim;
        if (ts == under_for && tx_under) {
            float k = ts->overlay ? 1 : 1 - 0.66f * e0;
            SDL_SetTextureColorModFloat(tx_under, k, k, k);
            SDL_SetTextureAlphaModFloat(tx_under, 1);
            SDL_RenderTexture(ren, tx_under, NULL, NULL);
            SDL_SetTextureColorModFloat(tx_under, 1, 1, 1);
        } else {
            draw_screens(base_of(top - 1), top - 1);
            if (!ts->overlay) fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(170 * e0)));
        }
        SDL_Texture *mt = main_target();
        SDL_SetRenderTarget(ren, tx_open);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);
        drawing = ts;
        if (ts->draw) { double t0 = app_time(); ts->draw(ts); scr_draw_ms += (app_time() - t0) * 1000; }
        drawing = NULL;
        SDL_SetRenderTarget(ren, mt);
        float e = ts->anim, a = e * 2.5f > 1 ? 1 : e * 2.5f; /* opaque early: no ghost of the screen underneath */
        if (ts == org_screen) blit_from_rect(tx_open, a, e, org_rect.x, org_rect.y, org_rect.w, org_rect.h, org_rect.r);
        else blit_scaled(tx_open, ts->overlay ? e : a, ts->overlay ? 0.97f + 0.03f * e : 0.94f + 0.06f * e);
    } else {
        draw_screens(base_of(top), top); /* overlays are drawn on top of the screen underneath */
    }
    if (close_t > 0.01f && tx_close) {
        float a = close_t * 2.5f > 1 ? 1 : close_t * 2.5f;
        if (close_to_rect) blit_from_rect(tx_close, a, close_t, close_rect.x, close_rect.y, close_rect.w, close_rect.h, close_rect.r);
        else if (close_overlay) blit_scaled(tx_close, close_t, 1.0f);
        else blit_scaled(tx_close, a, 0.96f + 0.04f * close_t); /* back: the screen sinks away */
    }
    draw_overlays();
    draw_toast();
    draw_fps();
    if (ui_dim > 0.003f) fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(ui_dim * 255)));
    if (ss_fade > 0.003f) draw_screensaver(ss_fade);
}

static void set_kd_graphics(void) {
    int fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    ioctl(fd, KDSETMODE, KD_GRAPHICS);
    close(fd);
}

/* the boot animation (notasplash, S01notasplash) holds the display until now: stop it and wait until it has
   closed the device (it removes its pid file after that), so we become the display's master */
static void splash_stop(void) {
    FILE *f = fopen("/run/notasplash.pid", "r");
    if (!f) return;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    char path[64], comm[32] = "";
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    f = pid > 1 ? fopen(path, "r") : NULL;
    if (f) {
        if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
        fclose(f);
    }
    /* "S01notasplash": the init script's subshell, still loading the display driver before it becomes notasplash */
    if (strncmp(comm, "notasplash", 10) && strncmp(comm, "S01notasplash", 13)) {
        unlink("/run/notasplash.pid"); /* stale file */
        return;
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 100 && access("/run/notasplash.pid", F_OK) == 0 && kill(pid, 0) == 0; i++) SDL_Delay(10);
    unlink("/run/notasplash.pid");
    SDL_Log("boot animation stopped");
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
    if (getenv("SDL_VIDEO_DRIVER") && !strcmp(getenv("SDL_VIDEO_DRIVER"), "kmsdrm")) splash_stop();
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
        double ph0 = app_time(); /* frame phases: logged for slow frames (Show FPS on) */
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && ren) {
                int ow, oh;
                SDL_GetRenderOutputSize(ren, &ow, &oh);
                /* also sent right after the display was set up again (after playback): same size = nothing to do,
                   otherwise every texture and glyph is thrown away a second time (a 55 ms hitch on a Pi 4) */
                if (ow == out_w && oh == out_h) continue;
                out_w = ow; out_h = oh;
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
        g_dt = dt;
        g_frame++;
        org_pending = false; /* an origin is only for a screen opened in the same frame */
        quick_tick();

        double ph1 = app_time();
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

        double ph2 = app_time();
        for (int i = 0; i < depth; i++) {
            Screen *s = stack[i];
            if (i == depth - 1) s->anim = anim_to(s->anim, 1, dt, 13);
            if (s->update && (i == depth - 1 || i == depth - 2)) s->update(s, dt);
        }
        close_t = anim_to(close_t, 0, dt, 13);
        ui_dim = anim_to(ui_dim, 0, dt, 7);
        if (screensaver) { /* ease-in-out fade in: glides in over a second */
            float p = (float)((now - ss_t) / 1.0);
            if (p > 1) p = 1;
            ss_fade = p * p * (3 - 2 * p);
        } else {
            ss_fade = anim_to(ss_fade, 0, dt, 16); /* quick fade back to the UI */
        }
        if (!ren) continue;
        double t_draw = app_time();
        double ph3 = t_draw;
        frame_no++;
        draw_frame();
        draw_ms_sum += (app_time() - t_draw) * 1000;
        const char *shot = script_shot();
        if (shot) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
            if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); SDL_Log("screenshot: %s", shot); }
        }
        double ph4 = app_time();
        present();
        double ph5 = app_time();
        double fg, fi, fm;
        int ng, ni, nm;
        gfx_frame_times(&fg, &fi, &fm, &ng, &ni, &nm);
        if (ph5 - ph0 > 0.030 && show_fps()) {
            Screen *ts = app_top();
            SDL_Log("langsomt billede %.0f ms: før/baggrund %.1f, taster %.1f, update %.1f, tegning %.1f (skærme %.1f, glyffer %d/%.1f, billeder %d/%.1f, mip %d/%.1f), present %.1f (%s)",
                    (ph5 - ph0) * 1000, (ph1 - ph0) * 1000, (ph2 - ph1) * 1000, (ph3 - ph2) * 1000, (ph4 - ph3) * 1000, scr_draw_ms,
                    ng, fg, ni, fi, nm, fm, (ph5 - ph4) * 1000, ts && ts->name ? ts->name : "?");
        }
        scr_draw_ms = 0;
        if (resume_t > 0) { SDL_Log("video igen: første billede efter %.0f ms", (app_time() - resume_t) * 1000); resume_t = 0; }
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
        bool ss_idle = screensaver && ss_fade >= 0.999f; /* full frame rate while it glides in/out */
        if (!target_mode) SDL_Delay(ss_idle ? 50 : 1);
        else if (ss_idle) SDL_Delay(100);
        img_cache_trim();
        gfx_idle_tick();
        tx_prewarm();
    }
    cfg_flush();
    while (depth) destroy_screen(stack[--depth]);
    video_quit();
    jobs_shutdown();
    SDL_Quit();
    quit_now(0);
}
