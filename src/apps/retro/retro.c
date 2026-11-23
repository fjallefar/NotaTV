/* NotaTV app package "retro" - Retro games with RetroArch and free libretro cores (Debat 4).
 * The package carries RetroArch and the cores as helpers (built by Buildroot into staging, copied by app.mk):
 *   fceumm (NES, GPL-2.0) · mGBA (Game Boy / Color / Advance, MPL-2.0) · PCSX ReARMed (PlayStation, GPL-2.0)
 * Only cores with a free license (no Snes9x/Genesis Plus GX/PicoDrive: non-commercial licenses).
 * Games are your own files: the app looks for them on USB drives and network drives (/media) and in its own folder.
 * Box art comes from the libretro-thumbnails repositories on GitHub by the file name (No-Intro names), if it is there.
 * A game runs full screen (app_run_fullscreen); the remote's Home key, Back held 2 s, or Start+Select+L+R on a
 * controller stops it. Saves (in-game and save states) are kept in the app's own folder. */
#define _GNU_SOURCE
#include "tv.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

extern const NotaApp nota_app;

#define MAXG 3000
#define COLS 5
#define TW 320
#define TH 240
#define GAP 32
#define PITCH (TH + 74)

typedef struct {
    const char *name, *core, *thumbs; /* thumbs: the folder on thumbnails.libretro.com */
    const char *ext[6];
    Color c;
} System;
static const System SYS[] = {
    {"NES", "fceumm_libretro.so", "Nintendo - Nintendo Entertainment System", {"nes", "fds", "unf", NULL}, {200, 40, 40, 255}},
    {"Game Boy", "mgba_libretro.so", "Nintendo - Game Boy", {"gb", NULL}, {110, 130, 70, 255}},
    {"Game Boy Color", "mgba_libretro.so", "Nintendo - Game Boy Color", {"gbc", NULL}, {120, 70, 170, 255}},
    {"Game Boy Advance", "mgba_libretro.so", "Nintendo - Game Boy Advance", {"gba", NULL}, {70, 60, 160, 255}},
    {"PlayStation", "pcsx_rearmed_libretro.so", "Sony - PlayStation", {"cue", "chd", "pbp", "m3u", NULL}, {90, 90, 100, 255}},
};
#define NSYS ((int)(sizeof SYS / sizeof SYS[0]))

typedef struct { char *path, *name; int sys; } Game;

typedef struct {
    Game *g;
    int n, sel, chip;
    bool on_chips, loading;
    int vis[MAXG], nvis;
    float scroll;
} Retro;

static Retro *live;

/* ---------- where the helpers are ---------- */
static bool helper(const char *rel, char *out, size_t n) {
    const char *dir = app_pkg_dir(&nota_app);
    snprintf(out, n, "%s/%s", dir && *dir ? dir : ".", rel);
    return access(out, R_OK) == 0;
}
static bool installed_ok(void) {
    char p[512];
    return helper("retroarch", p, sizeof p);
}

/* ---------- finding games ---------- */
static int sys_of(const char *file) {
    const char *dot = strrchr(file, '.');
    if (!dot) return -1;
    for (int s = 0; s < NSYS; s++)
        for (int k = 0; SYS[s].ext[k]; k++)
            if (!strcasecmp(dot + 1, SYS[s].ext[k])) return s;
    return -1;
}

typedef struct { Game *g; int n; char data[512]; } Scan;
static void scan_dir(Scan *s, const char *dir, int depth) {
    if (depth > 4 || s->n >= MAXG) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s->n < MAXG) {
        if (de->d_name[0] == '.') continue;
        char *full = str_printf("%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) scan_dir(s, full, depth + 1);
            else {
                int sy = sys_of(de->d_name);
                /* a .bin next to a .cue is part of that disc; .m3u lists discs of one game */
                if (sy >= 0) {
                    s->g = xrealloc(s->g, sizeof *s->g * (s->n + 1));
                    Game *g = &s->g[s->n++];
                    g->path = full;
                    full = NULL;
                    g->name = str_dup(de->d_name);
                    char *dot = strrchr(g->name, '.');
                    if (dot) *dot = 0;
                    g->sys = sy;
                }
            }
        }
        free(full);
    }
    closedir(d);
}
static int game_cmp(const void *a, const void *b) { return strcasecmp(((const Game *)a)->name, ((const Game *)b)->name); }
static void scan_work(void *arg) {
    Scan *s = arg;
    char roms[600];
    snprintf(roms, sizeof roms, "%s/roms", s->data);
    mkdir(roms, 0755);
    scan_dir(s, roms, 0);
    scan_dir(s, "/media", 0);
    if (s->n > 1) qsort(s->g, s->n, sizeof *s->g, game_cmp);
}
static void free_games(Game *g, int n) { for (int i = 0; i < n; i++) { free(g[i].path); free(g[i].name); } free(g); }
static void rebuild(Retro *v);
static void scan_done(void *arg) {
    Scan *s = arg;
    Retro *v = live;
    if (v) {
        free_games(v->g, v->n);
        v->g = s->g; v->n = s->n;
        s->g = NULL; s->n = 0;
        v->loading = false;
        rebuild(v);
    }
    free_games(s->g, s->n);
    free(s);
}
static void scan(Retro *v) {
    Scan *s = xcalloc(1, sizeof *s);
    snprintf(s->data, sizeof s->data, "%s", app_data(&nota_app));
    v->loading = true;
    job_submit_q(JQ_SLOW, scan_work, scan_done, s);
}

static int chip_sys(int chip, const Retro *v) { /* chip 0 = All; then the systems that have games */
    if (chip == 0) return -1;
    int k = 0;
    for (int s = 0; s < NSYS; s++) {
        bool has = false;
        for (int i = 0; i < v->n && !has; i++) has = v->g[i].sys == s;
        if (has && ++k == chip) return s;
    }
    return -1;
}
static int n_chips(const Retro *v) {
    int k = 1;
    for (int s = 0; s < NSYS; s++) {
        bool has = false;
        for (int i = 0; i < v->n && !has; i++) has = v->g[i].sys == s;
        k += has;
    }
    return k;
}
static void rebuild(Retro *v) {
    int sy = chip_sys(v->chip, v);
    v->nvis = 0;
    for (int i = 0; i < v->n && v->nvis < MAXG; i++) if (sy < 0 || v->g[i].sys == sy) v->vis[v->nvis++] = i;
    if (v->sel >= v->nvis) v->sel = v->nvis ? v->nvis - 1 : 0;
}

/* ---------- box art ---------- */
static char *thumb_url(const Game *g) {
    char name[300];
    snprintf(name, sizeof name, "%s", g->name);
    for (char *p = name; *p; p++) if (strchr("&*/:`<>?\\|\"", *p)) *p = '_'; /* the thumbnail server's rule */
    char repo[96];
    snprintf(repo, sizeof repo, "%s", SYS[g->sys].thumbs);
    for (char *p = repo; *p; p++) if (*p == ' ') *p = '_'; /* "Nintendo - Game Boy" -> the repo Nintendo_-_Game_Boy */
    char *n = url_encode(name);
    char *u = str_printf("https://raw.githubusercontent.com/libretro-thumbnails/%s/master/Named_Boxarts/%s.png", repo, n);
    free(n);
    return u;
}

/* ---------- playing ---------- */
static void write_cfg(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    char joy[512];
    bool has_joy = helper("autoconfig", joy, sizeof joy);
    fprintf(f,
            "video_driver = \"gl\"\nvideo_context_driver = \"kms\"\nvideo_fullscreen = \"true\"\nvideo_vsync = \"true\"\n"
            "video_smooth = \"false\"\nvideo_scale_integer = \"false\"\naspect_ratio_index = \"22\"\n"
            "audio_driver = \"alsa\"\naudio_device = \"\"\naudio_latency = \"64\"\n"
            "input_driver = \"udev\"\ninput_joypad_driver = \"udev\"\ninput_autodetect_enable = \"true\"\n"
            "%s%s%s"
            "menu_driver = \"rgui\"\nmenu_show_online_updater = \"false\"\nmenu_show_core_updater = \"false\"\n"
            "input_menu_toggle_gamepad_combo = \"4\"\ninput_quit_gamepad_combo = \"3\"\n"
            "input_exit_emulator = \"escape\"\ninput_menu_toggle = \"f1\"\nconfirm_quit = \"false\"\nquit_press_twice = \"false\"\n"
            "savefile_directory = \"%s/saves\"\nsavestate_directory = \"%s/states\"\nsystem_directory = \"%s/bios\"\n"
            "screenshot_directory = \"%s/screenshots\"\nsavestate_auto_save = \"true\"\nsavestate_auto_load = \"true\"\n"
            "config_save_on_exit = \"false\"\nnetwork_cmd_enable = \"false\"\nmenu_enable_widgets = \"true\"\n"
            "notification_show_autoconfig = \"true\"\nlog_verbosity = \"false\"\n",
            has_joy ? "joypad_autoconfig_dir = \"" : "", has_joy ? joy : "", has_joy ? "\"\n" : "",
            data, data, data, data);
    fclose(f);
}

static void play(const Game *g) {
    if (!app_run_fullscreen) { app_toast(T("Retro games need a newer NotaTV")); return; }
    char ra[512], core[512];
    char rel[96];
    snprintf(rel, sizeof rel, "cores/%s", SYS[g->sys].core);
    if (!helper("retroarch", ra, sizeof ra) || !helper(rel, core, sizeof core)) { app_toast(T("This system is not in the package")); return; }
    const char *data = app_data(&nota_app);
    static const char *sub[] = {"saves", "states", "bios", "screenshots"};
    for (int i = 0; i < 4; i++) { char *d = str_printf("%s/%s", data, sub[i]); mkdir(d, 0755); free(d); }
    char *cfg = str_printf("%s/retroarch.cfg", data);
    write_cfg(cfg, data);
    char *argv[] = {"retroarch", "-c", cfg, "-L", core, g->path, NULL};
    int rc = app_run_fullscreen(ra, argv, "/tmp/retroarch.log");
    if (rc != 0 && rc != 143) {
        SDL_Log("retro: retroarch sluttede med %d", rc);
        app_toast(T("The game stopped - see /tmp/retroarch.log"));
    }
    free(cfg);
}

/* ---------- the screen ---------- */
static void retro_draw(Screen *s) {
    Retro *v = s->st;
    draw_background();
    draw_topbar(T("Retro games"));
    if (!installed_ok()) {
        draw_empty(T("RetroArch is missing"), T("This package was built without RetroArch. Update the app from the App Store."));
        return;
    }
    int nc = n_chips(v);
    const char *chips[NSYS + 1];
    chips[0] = T("All");
    for (int c = 1; c < nc; c++) chips[c] = SYS[chip_sys(c, v)].name;
    draw_chips(96, 128, chips, nc, v->on_chips ? v->chip : -1);
    if (!v->nvis) {
        if (v->loading) { draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT); return; }
        char *msg = str_printf(T("Put your own games on a USB drive or a network drive, or in %s/roms. NES, Game Boy, Game Boy Advance and PlayStation games are found by themselves."),
                               app_data(&nota_app));
        draw_empty(T("No games found"), msg);
        free(msg);
        return;
    }
    float y0 = 220;
    clip_push(0, y0 - 30, UI_W, UI_H - y0 + 30);
    for (int k = 0; k < v->nvis; k++) {
        const Game *g = &v->g[v->vis[k]];
        float x = 96 + (k % COLS) * (TW + GAP), y = y0 + (k / COLS) * PITCH - v->scroll;
        if (y + PITCH < y0 - 40 || y > UI_H) continue;
        bool f = !v->on_chips && k == v->sel;
        float fa = ui_focus(g, f), sc = 1 + 0.06f * fa, w = TW * sc, h = TH * sc, xx = x - (w - TW) / 2, yy = y - (h - TH) / 2;
        ui_focus_under(xx, yy, w, h, 16, fa);
        Color c = SYS[g->sys].c;
        fill_rrect_vgrad(xx, yy, w, h, 16, c, color_mix(c, RGB(0, 0, 0), 0.5f));
        char *u = thumb_url(g);
        Image *im = img_get(u);
        free(u);
        if (img_state(im) == IMG_READY) img_draw(im, xx + 8, yy + 8, w - 16, h - 16, 10, 1);
        else text_draw_wrap(F_SEMI, 26, xx + 20, yy + 24, w - 40, 4, 32, RGB(255, 255, 255), g->name);
        ui_focus_over(xx, yy, w, h, 16, fa);
        text_draw_clip(F_SEMI, 24, x, y + TH + 14, TW, f ? C_TEXT : C_TEXT2, AL_LEFT, g->name);
        text_draw_clip(F_REG, 20, x, y + TH + 44, TW, C_TEXT3, AL_LEFT, SYS[g->sys].name);
    }
    clip_pop();
}

static void retro_update(Screen *s, float dt) {
    Retro *v = s->st;
    int row = v->sel / COLS;
    v->scroll = anim_to(v->scroll, row > 1 ? (row - 1) * PITCH : 0, dt, 12);
}

static bool retro_input(Screen *s, const InputEvent *ev) {
    Retro *v = s->st;
    if (v->on_chips) {
        int c = v->chip;
        if (buttons_nav(&c, n_chips(v), ev)) { if (c != v->chip) { v->chip = c; v->sel = 0; rebuild(v); } return true; }
        if (ev->a == A_DOWN) { if (v->nvis) v->on_chips = false; return true; }
        if (ev->a == A_BACK || ev->a == A_LEFT) { app_pop(); return true; }
        return true;
    }
    int n = v->nvis;
    switch (ev->a) {
    case A_UP: if (v->sel >= COLS) v->sel -= COLS; else v->on_chips = true; return true;
    case A_DOWN: if (v->sel + COLS < n) v->sel += COLS; else if (v->sel / COLS < (n - 1) / COLS) v->sel = n - 1; return true;
    case A_LEFT: if (v->sel % COLS) v->sel--; else v->on_chips = true; return true;
    case A_RIGHT: if (v->sel + 1 < n && (v->sel + 1) % COLS) v->sel++; return true;
    case A_OK: case A_PLAY: case A_PLAYPAUSE: if (n) play(&v->g[v->vis[v->sel]]); return true;
    case A_MENU: scan(v); return true;
    case A_BACK: app_pop(); return true;
    default: return false;
    }
}

static void retro_enter(Screen *s) { live = s->st; scan(live); }
static void retro_resume(Screen *s) { live = s->st; }
static void retro_destroy(Screen *s) {
    Retro *v = s->st;
    if (live == v) live = NULL;
    free_games(v->g, v->n);
    free(v);
    free(s);
}

static Screen *retro_screen(void) {
    if (!draw_chips || !app_run_fullscreen) { app_toast(T("Update NotaTV to use this app")); return NULL; } /* an older NotaTV */
    Screen *s = xcalloc(1, sizeof *s);
    Retro *v = xcalloc(1, sizeof *v);
    v->on_chips = false;
    s->name = "retro";
    s->st = v;
    s->enter = retro_enter;
    s->resume = retro_resume;
    s->update = retro_update;
    s->draw = retro_draw;
    s->input = retro_input;
    s->destroy = retro_destroy;
    return s;
}

const NotaApp nota_app = {
    NOTA_API, "retro", N_("Retro games"), N_("Play your own NES, Game Boy and PlayStation games"), APP_VERSION, 82, 0,
    ICON_STAR, {235, 120, 40, 255}, {160, 50, 120, 255}, {255, 255, 255, 255},
    .open = retro_screen,
};
