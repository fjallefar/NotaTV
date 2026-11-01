/* NotaTV shell - home screen */
#include "tv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *name, *desc;
    IconId icon;
    Color c1, c2, fg;
    Screen *(*open)(void);
    const char *key; /* cfg app_<key> = 0 hides the app (App Store); NULL = cannot be turned off */
    bool off;        /* not turned on from the start (chosen in the App Store) */
} App;

Screen *appstore_screen(void);

static const App apps[] = {
    {"Live TV", N_("Watch live TV and channels from your IPTV provider"), ICON_TV, {43, 95, 217, 255}, {22, 52, 140, 255}, {255, 255, 255, 255}, iptv_screen, "iptv"},
    {"Plex", N_("Movies and shows from your Plex server"), ICON_PLEX, {40, 40, 44, 255}, {22, 22, 26, 255}, {229, 160, 13, 255}, plex_screen, "plex"},
    {"Jellyfin", N_("Movies and shows from your Jellyfin server"), ICON_FILM, {0, 164, 220, 255}, {120, 70, 190, 255}, {255, 255, 255, 255}, jellyfin_screen, "jellyfin", true},
    {"YouTube", N_("Search and watch videos on YouTube"), ICON_YOUTUBE, {229, 45, 39, 255}, {170, 18, 20, 255}, {255, 255, 255, 255}, youtube_screen, "youtube"},
    {"Twitch", N_("Live streams from the channels you follow and all of Twitch"), ICON_PLAY, {145, 70, 255, 255}, {88, 38, 170, 255}, {255, 255, 255, 255}, twitch_screen, "twitch"},
    {"Seerr", N_("Find and request movies and shows for Plex"), ICON_STAR, {99, 102, 241, 255}, {55, 48, 163, 255}, {255, 255, 255, 255}, seerr_screen, "seerr"},
    {N_("Music"), N_("Play music from your phone via AirPlay"), ICON_MUSIC, {250, 45, 85, 255}, {150, 20, 60, 255}, {255, 255, 255, 255}, music_screen, "music"},
    {N_("Games"), N_("Play games from your PC via Sunshine (Moonlight)"), ICON_PLAY, {30, 160, 90, 255}, {12, 90, 50, 255}, {255, 255, 255, 255}, games_screen, "games"},
    {N_("Media"), N_("Movies, music and pictures from USB and network"), ICON_FOLDER, {123, 77, 255, 255}, {75, 42, 179, 255}, {255, 255, 255, 255}, media_screen, "media"},
    {"App Store", N_("Choose which apps are shown on the home screen"), ICON_CHECK, {16, 150, 140, 255}, {8, 92, 96, 255}, {255, 255, 255, 255}, appstore_screen, NULL},
    {N_("Settings"), N_("Network, picture, sound, HDMI-CEC and system"), ICON_GEAR, {98, 106, 125, 255}, {58, 64, 80, 255}, {255, 255, 255, 255}, settings_screen, NULL},
};
#define N_APPS (int)(sizeof apps / sizeof apps[0])

static bool app_on(int i) {
    if (!apps[i].key) return true;
    char k[48];
    snprintf(k, sizeof k, "app_%s", apps[i].key);
    return cfg_int(k, apps[i].off ? 0 : 1) != 0;
}

/* visible apps on the home screen */
static int vis[N_APPS], n_vis;
static void build_vis(void) {
    n_vis = 0;
    for (int i = 0; i < N_APPS; i++) if (app_on(i)) vis[n_vis++] = i;
}

/* app grid: one row up to 6 apps, otherwise two rows (the top row gets the extra one for an odd count) */
#define TILE_W 230.0f
#define TILE_H 120.0f
#define TILE_GAP 36.0f
#define TILE_PITCH 186.0f /* tile + name */
#define APPS_Y 510.0f
static int grid_cols(void) { return n_vis <= 6 ? n_vis : (n_vis + 1) / 2; }
static int grid_rows(void) { return n_vis <= 6 ? 1 : 2; }

typedef struct {
    int row, col, col2;
    float foc[N_APPS];
    float sx2; /* scroll in row 2 */
    float row_anim;
    HomeItem *cont;
    int n_cont, cont_app; /* the row below the apps follows the app in focus */
    double refreshed;
    /* "top shelf": a big image from the focused app behind the top (drawn in a layer) */
    char bg_url[512], prev_url[512], bgl_key[600];
    float bg_fade, bgl_fade;
    Layer *bgl;
} Home;

#define SHELF_H 820

/* the home screen owns its own copies: Plex/IPTV can rebuild their lists at any time */
static void free_continue(Home *h) {
    for (int i = 0; i < h->n_cont; i++) {
        free(h->cont[i].title); free(h->cont[i].subtitle); free(h->cont[i].image); free(h->cont[i].key); free(h->cont[i].logo);
    }
    free(h->cont);
    h->cont = NULL;
    h->n_cont = 0;
}

static void copy_item(HomeItem *d, const HomeItem *s) {
    d->title = str_dup(s->title);
    d->subtitle = str_dup(s->subtitle);
    d->image = str_dup(s->image);
    d->key = str_dup(s->key);
    d->progress = s->progress;
    d->logo = str_dup(s->logo);
}

/* row 2 per app: title and content (NULL = no row) */
static const char *cont_title(int app) {
    Screen *(*o)(void) = apps[app].open;
    if (o == plex_screen) return T("Continue watching");
    if (o == iptv_screen) return T("Recent channels");
    if (o == youtube_screen) return T("Recently watched");
    return NULL;
}

/* the row below the apps: the focused app, if it has a row; otherwise the last one stays
 * (so "Recent channels" can still be reached when moving down through the second app row) */
static bool app_visible(int app) {
    for (int i = 0; i < n_vis; i++) if (vis[i] == app) return true;
    return false;
}
static int pick_cont_app(const Home *h) {
    int a = vis[h->col];
    if (cont_title(a)) return a;
    if (cont_title(h->cont_app) && app_visible(h->cont_app)) return h->cont_app;
    for (int i = 0; i < n_vis; i++) if (cont_title(vis[i])) return vis[i];
    return a;
}

static void collect_continue(Home *h) {
    free_continue(h);
    h->cont_app = pick_cont_app(h);
    Screen *(*o)(void) = apps[h->cont_app].open;
    HomeItem *a = NULL;
    int na = o == plex_screen ? plex_ondeck(&a) : o == iptv_screen ? iptv_recent(&a) : o == youtube_screen ? youtube_recent(&a) : 0;
    h->cont = calloc(na + 1, sizeof(HomeItem));
    for (int i = 0; i < na; i++) copy_item(&h->cont[h->n_cont++], &a[i]);
    if (h->col2 >= h->n_cont) h->col2 = h->n_cont ? h->n_cont - 1 : 0;
    if (h->n_cont == 0 && h->row == 1) h->row = 0;
}

static void home_enter(Screen *s) {
    Home *h = s->st;
    build_vis();
    if (h->col >= n_vis) h->col = 0;
    plex_refresh_ondeck();
    collect_continue(h);
    h->refreshed = app_time();
}

static void home_resume(Screen *s) { home_enter(s); }

static void home_update(Screen *s, float dt) {
    Home *h = s->st;
    for (int i = 0; i < n_vis; i++)
        h->foc[i] = anim_to(h->foc[i], (h->row == 0 && h->col == i) ? 1 : 0, dt, 16);
    h->row_anim = anim_to(h->row_anim, (float)h->row, dt, 12);
    if (h->row == 0 && h->cont_app != pick_cont_app(h)) { h->col2 = 0; h->sx2 = 0; collect_continue(h); }
    h->bg_fade = anim_to(h->bg_fade, 1, dt, 6);
    /* Plex data arrives asynchronously */
    if (app_time() - h->refreshed > 2) {
        collect_continue(h);
        h->refreshed = app_time();
    }
    float target = 0;
    float iw = 420, gap = 32; /* the row below the apps */
    float sel_x = h->col2 * (iw + gap);
    if (sel_x - target > UI_W - 240 - iw) target = sel_x - (UI_W - 240 - iw);
    h->sx2 = anim_to(h->sx2, target, dt, 12);
}

static void greeting(char *buf, size_t n) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    const char *g = tm.tm_hour < 5 ? N_("Good night") : tm.tm_hour < 10 ? N_("Good morning") : tm.tm_hour < 12 ? N_("Good morning")
                    : tm.tm_hour < 18 ? N_("Good afternoon") : N_("Good evening");
    static const char *days[] = {N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"), N_("Thursday"), N_("Friday"), N_("Saturday")};
    static const char *mon[] = {N_("January"), N_("February"), N_("March"), N_("April"), N_("May"), N_("June"), N_("July"),
                                N_("August"), N_("September"), N_("October"), N_("November"), N_("December")};
    char d[64];
    /* date without year: 1 = weekday, 2 = day, 3 = month */
    snprintf(d, sizeof d, T("%1$s, %3$s %2$d"), T(days[tm.tm_wday]), tm.tm_mday, T(mon[tm.tm_mon]));
    snprintf(buf, n, "%s|%s", T(g), d);
}

static void draw_shelf(Home *h, int app) {
    const App *a = &apps[app];
    float x = 120, y = 210;
    if (apps[app].open == music_screen) { /* something is playing: cover and title instead of the icon */
        char *t = NULL, *ar = NULL, *cov = NULL;
        bool playing = false;
        if (music_now(&t, &ar, &cov, &playing) && t) {
            fill_rrect(x, y, 300, 300, 16, C_SURFACE);
            if (cov) img_draw_cover(img_get(cov), x, y, 300, 300, 16, 1);
            text_draw(F_SEMI, 24, x + 340, y + 70, C_ACCENT, AL_LEFT, playing ? T("NOW PLAYING") : T("PAUSED"));
            text_draw_clip(F_BOLD, 52, x + 340, y + 110, 1200, C_TEXT, AL_LEFT, t);
            text_draw_clip(F_REG, 32, x + 340, y + 180, 1200, C_TEXT2, AL_LEFT, ar ? ar : "");
            free(t); free(ar); free(cov);
            return;
        }
        free(t); free(ar); free(cov);
    }
    /* default: big icon + description */
    fill_rrect_vgrad(x, y + 20, 240, 240, 48, a->c1, a->c2);
    draw_icon(a->icon, x + 120, y + 140, 120, a->fg);
    text_draw(F_BOLD, 72, x + 290, y + 70, C_TEXT, AL_LEFT, T(a->name));
    text_draw(F_REG, 32, x + 292, y + 168, C_TEXT2, AL_LEFT, T(a->desc));
    if (apps[app].open == settings_screen) {
        char info[160];
        const char *ip = app_ip();
        snprintf(info, sizeof info, "NotaTV %s   ·   %s", TV_VERSION, ip[0] ? ip : T("No network connection"));
        text_draw(F_REG, 26, x + 292, y + 220, C_TEXT3, AL_LEFT, info);
    }
}

static void home_draw(Screen *s) {
    Home *h = s->st;
    int focus_app = vis[h->col];
    const App *fa = &apps[focus_app];
    /* top shelf: Plex/YouTube show what you can continue (follows the choice in the row) */
    char want[512] = "";
    HomeItem *shelf_it = NULL;
    if (h->cont_app == focus_app && h->n_cont && apps[focus_app].open != iptv_screen) {
        shelf_it = &h->cont[h->row == 1 && h->col2 < h->n_cont ? h->col2 : 0];
        if (shelf_it->image) snprintf(want, sizeof want, "%s", shelf_it->image);
    }
    if (strcmp(want, h->bg_url)) {
        snprintf(h->prev_url, sizeof h->prev_url, "%s", h->bg_url);
        snprintf(h->bg_url, sizeof h->bg_url, "%s", want);
        h->bg_fade = 0;
    }
    Image *ni = h->bg_url[0] ? img_get(h->bg_url) : NULL, *pi = h->prev_url[0] ? img_get(h->prev_url) : NULL;
    bool ready = ni && img_state(ni) == IMG_READY;
    if (h->bg_url[0] && !ready) h->bg_fade = 0; /* the fade starts when the image is ready */
    float fa_ = h->bg_fade;
    if (!h->bgl) { h->bgl = layer_new(UI_W, UI_H); layer_set_opaque(h->bgl, true); }
    char key[600];
    snprintf(key, sizeof key, "%s|%d|%d", h->bg_url, ready, focus_app);
    bool settled = fa_ > 0.995f && h->bgl_fade > 0.995f;
    if ((!layer_valid(h->bgl) || strcmp(key, h->bgl_key) || !settled) && layer_begin(h->bgl)) {
        draw_background();
        fill_vgrad(0, 0, UI_W, 700, color_alpha(fa->c1, 0.18f), RGBA(0, 0, 0, 0)); /* color tint from the app */
        if (pi && fa_ < 1) img_draw_cover(pi, 0, 0, UI_W, SHELF_H, 0, 0.75f * (1 - fa_));
        if (ni) img_draw_cover(ni, 0, 0, UI_W, SHELF_H, 0, 0.75f * fa_);
        if (ni || pi) {
            fill_hgrad(0, 0, UI_W * 0.8f, SHELF_H, RGBA(10, 12, 17, 240), RGBA(10, 12, 17, 0));
            fill_vgrad(0, SHELF_H - 420, UI_W, 420, RGBA(11, 13, 19, 0), RGBA(11, 13, 19, 255));
        }
        layer_end(h->bgl);
        snprintf(h->bgl_key, sizeof h->bgl_key, "%s", key);
        h->bgl_fade = fa_;
    }
    if (layer_valid(h->bgl)) layer_draw(h->bgl, 0, 0, 1);
    else draw_background();

    char g[128];
    greeting(g, sizeof g);
    char *bar = strchr(g, '|');
    *bar = 0;
    text_draw(F_BOLD, 40, 120, 58, C_TEXT, AL_LEFT, g);
    text_draw(F_REG, 26, 120 + text_width(F_BOLD, 40, g) + 24, 70, C_TEXT2, AL_LEFT, bar + 1);
    draw_topbar(NULL);
    const char *ip = app_ip();
    draw_icon(ip[0] ? ICON_WIFI : ICON_WIFI, UI_W - 250, 82, 34, ip[0] ? C_TEXT2 : C_DANGER);

    float shift = h->row_anim * 340; /* scroll up when row 2 is selected */
    clip_push(0, 150, UI_W, APPS_Y - 170);
    bool shelf_vis = h->row_anim < 0.2f; /* the row below the apps scrolls up over the top */
    if (shelf_vis && shelf_it && h->bg_url[0]) { /* big image behind: the title of what you can continue */
        float x = 120, y = 230;
        text_draw(F_BOLD, 24, x, y, color_mix(fa->c1, C_TEXT, 0.45f), AL_LEFT,
                  apps[focus_app].open == plex_screen ? T("CONTINUE IN PLEX") : apps[focus_app].open == youtube_screen ? T("RECENTLY WATCHED ON YOUTUBE") : T(fa->name));
        int tl = text_draw_wrap(F_BOLD, 60, x, y + 40, 1100, 2, 70, C_TEXT, shelf_it->title ? shelf_it->title : "");
        float sy = y + 40 + tl * 70 + 10;
        if (shelf_it->subtitle) { text_draw_clip(F_REG, 30, x, sy, 1100, C_TEXT2, AL_LEFT, shelf_it->subtitle); sy += 50; }
        if (shelf_it->progress > 0) {
            fill_rrect(x, sy + 6, 420, 6, 3, RGBA(255, 255, 255, 60));
            fill_rrect(x, sy + 6, 420 * (float)shelf_it->progress, 6, 3, C_TEXT);
        }
    } else if (shelf_vis && apps[focus_app].open == iptv_screen && h->cont_app == focus_app && h->n_cont) {
        /* Live TV: latest channel with logo (press down for the others) */
        HomeItem *it = &h->cont[0];
        float x = 120, y = 220;
        draw_shadow(x, y, 360, 200, 28, 30, 1);
        fill_rrect(x, y, 360, 200, 28, RGB(235, 238, 244));
        if (!img_draw(img_get(it->image), x + 50, y + 30, 260, 140, 0, 1))
            text_draw_clip(F_BOLD, 34, x + 180, y + 78, 320, RGB(60, 60, 70), AL_CENTER, it->title ? it->title : "");
        text_draw(F_BOLD, 24, x + 410, y + 34, color_mix(fa->c1, C_TEXT, 0.45f), AL_LEFT, T("RECENTLY WATCHED"));
        text_draw_clip(F_BOLD, 60, x + 410, y + 70, 1100, C_TEXT, AL_LEFT, it->title ? it->title : "");
        text_draw(F_REG, 28, x + 410, y + 150, C_TEXT2, AL_LEFT, T("Live TV  ·  press down for your recent channels"));
    } else if (shelf_vis) draw_shelf(h, focus_app);
    clip_pop();

    int cols = grid_cols();
    float gap = TILE_GAP, tw = TILE_W, th = TILE_H;
    for (int i = 0; i < n_vis; i++) {
        const App *a = &apps[vis[i]];
        int r = i / cols, c = i % cols;
        int in_row = r == 0 ? (n_vis < cols ? n_vis : cols) : n_vis - cols;
        float total = in_row * tw + (in_row - 1) * gap;
        float x0 = UI_W / 2 - total / 2, y0 = APPS_Y + r * TILE_PITCH - shift;
        float f = h->foc[i];
        float sc = 1 + 0.1f * f;
        float w = tw * sc, hh = th * sc;
        float cx = x0 + c * (tw + gap) + tw / 2, cy = y0 + th / 2;
        float x = cx - w / 2, y = cy - hh / 2 - 8 * f;
        if (f > 0.01f) draw_shadow(x, y, w, hh, 24, 30 * f, f);
        fill_rrect_vgrad(x, y, w, hh, 24, a->c1, a->c2);
        draw_icon(a->icon, cx, y + hh / 2, 70 * sc, a->fg);
        if (f > 0.01f) stroke_rrect(x - 4, y - 4, w + 8, hh + 8, 28, 4, color_alpha(C_FOCUS, f));
        Color tc = color_mix(C_TEXT3, C_TEXT, f);
        text_draw(F_SEMI, 26, cx, y0 + th + 22, tc, AL_CENTER, T(a->name));
    }

    if (h->n_cont > 0) {
        float ry = APPS_Y + (grid_rows() - 1) * TILE_PITCH + 244 - shift;
        text_draw(F_SEMI, 30, 120, ry - 48, h->row == 1 ? C_TEXT : C_TEXT2, AL_LEFT, cont_title(h->cont_app));
        clip_push(0, ry - 20, UI_W, 420);
        float iw = 420, ih = 236, gap2 = 32;
        for (int i = 0; i < h->n_cont; i++) {
            HomeItem *it = &h->cont[i];
            float x = 120 + i * (iw + gap2) - h->sx2, y = ry;
            if (x > UI_W || x + iw < 0) continue;
            bool f = h->row == 1 && h->col2 == i;
            float sc = f ? 1.06f : 1;
            float w = iw * sc, hh = ih * sc;
            float xx = x - (w - iw) / 2, yy = y - (hh - ih) / 2;
            if (f) draw_shadow(xx, yy, w, hh, 18, 24, 1);
            bool logo = it->key && !strncmp(it->key, "iptv:", 5); /* channel logos: light surface, the whole logo */
            fill_rrect(xx, yy, w, hh, 18, logo ? RGB(235, 238, 244) : C_SURFACE);
            Image *im = img_get(it->image);
            if (logo ? !img_draw(im, xx + w * 0.2f, yy + hh * 0.12f, w * 0.6f, hh * 0.42f, 0, 1) : !img_draw_cover(im, xx, yy, w, hh, 18, 1)) {
                draw_icon(ICON_TV, xx + w / 2, yy + hh / 2 - 20, 70, C_TEXT3);
            }
            if (it->progress > 0) {
                fill_rrect(xx + 14, yy + hh - 16, w - 28, 6, 3, RGBA(0, 0, 0, 150));
                fill_rrect(xx + 14, yy + hh - 16, (w - 28) * (float)it->progress, 6, 3, C_TEXT);
            }
            if (f) stroke_rrect(xx - 4, yy - 4, w + 8, hh + 8, 22, 4, C_FOCUS);
            /* title under the tile (like in Plex) - no text on the image */
            text_draw_clip(F_SEMI, 25, x, y + ih + 16, iw, f ? C_TEXT : C_TEXT2, AL_LEFT, it->title ? it->title : "");
            if (it->subtitle) text_draw_clip(F_REG, 22, x, y + ih + 48, iw, C_TEXT3, AL_LEFT, it->subtitle);
        }
        clip_pop();
    }
}

static bool home_input(Screen *s, const InputEvent *ev) {
    Home *h = s->st;
    switch (ev->a) {
    case A_LEFT:
        if (h->row == 0) h->col = (h->col + n_vis - 1) % n_vis;
        else if (h->col2 > 0) h->col2--;
        return true;
    case A_RIGHT:
        if (h->row == 0) h->col = (h->col + 1) % n_vis;
        else if (h->col2 < h->n_cont - 1) h->col2++;
        return true;
    case A_DOWN:
        if (h->row == 0) {
            int cols = grid_cols();
            if (h->col / cols + 1 < grid_rows()) h->col = h->col + cols < n_vis ? h->col + cols : n_vis - 1;
            else if (h->n_cont > 0) h->row = 1;
        }
        return true;
    case A_UP:
        if (h->row == 1) h->row = 0;
        else if (h->col >= grid_cols()) h->col -= grid_cols();
        return true;
    case A_OK:
        if (h->row == 0) app_push(apps[vis[h->col]].open());
        else if (h->col2 < h->n_cont) {
            HomeItem *it = &h->cont[h->col2];
            if (it->key && !strncmp(it->key, "iptv:", 5)) iptv_play_recent(atoi(it->key + 5));
            else if (it->key && !strncmp(it->key, "yt:", 3)) youtube_play_recent(atoi(it->key + 3));
            else if (it->key) plex_play_key(it->key);
        }
        return true;
    case A_MENU:
        power_menu_open();
        return true;
    case A_BACK: case A_HOME:
        h->row = 0;
        h->col = 0;
        return true;
    default:
        return false;
    }
}

Screen *home_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "home";
    s->st = calloc(1, sizeof(Home));
    s->enter = home_enter;
    s->resume = home_resume;
    s->update = home_update;
    s->draw = home_draw;
    s->input = home_input;
    return s;
}

/* ---------- App Store: choose which apps are shown ----------
 * Two sections: N_("Enabled") and N_("Disabled"). Apps that cannot be turned off (key == NULL) are not shown. */
typedef struct { int sel_app; float sy; } Store;

#define STORE_COLS 3
#define STORE_PITCH 250.0f
#define STORE_HDR 70.0f
#define STORE_GAP 50.0f

typedef struct { int app, row, col; float y; } StoreCell;
typedef struct { StoreCell c[N_APPS]; int n, rows; float hdr_y[2]; int cnt[2]; } StoreLayout;

/* y is in content coordinates (0 = top of the list); an empty section gets one row for "no apps" */
static void store_layout(StoreLayout *L) {
    L->n = 0;
    int row = 0;
    float y = 0;
    for (int sec = 0; sec < 2; sec++) {
        L->hdr_y[sec] = y;
        y += STORE_HDR;
        int k = 0;
        for (int i = 0; i < N_APPS; i++) {
            if (!apps[i].key || app_on(i) != (sec == 0)) continue;
            L->c[L->n++] = (StoreCell){i, row + k / STORE_COLS, k % STORE_COLS, y + (k / STORE_COLS) * STORE_PITCH};
            k++;
        }
        L->cnt[sec] = k;
        int r = k ? (k + STORE_COLS - 1) / STORE_COLS : 0;
        row += r;
        y += (r ? r * STORE_PITCH : 80) + STORE_GAP;
    }
    L->rows = row;
}

static int store_find(const StoreLayout *L, int app) {
    for (int i = 0; i < L->n; i++) if (L->c[i].app == app) return i;
    return L->n ? 0 : -1;
}

static void store_update(Screen *s, float dt) {
    Store *st = s->st;
    StoreLayout L;
    store_layout(&L);
    int i = store_find(&L, st->sel_app);
    if (i < 0) return;
    st->sel_app = L.c[i].app;
    float t = L.c[i].y - 300;
    if (L.c[i].row == 0 || (L.cnt[0] && L.c[i].row == (L.cnt[0] + STORE_COLS - 1) / STORE_COLS)) t -= STORE_HDR; /* vis sektionsoverskriften */
    st->sy = anim_to(st->sy, t > 0 ? t : 0, dt, 12);
}

static void store_draw(Screen *s) {
    Store *st = s->st;
    draw_background();
    draw_topbar("App Store");
    text_draw(F_REG, 26, 120, 130, C_TEXT2, AL_LEFT, T("Choose which apps are shown on the home screen. OK turns them on and off."));
    StoreLayout L;
    store_layout(&L);
    int sel = store_find(&L, st->sel_app);
    float cw = (UI_W - 240 - (STORE_COLS - 1) * 30) / STORE_COLS, ch = 220, top = 200 - st->sy;
    clip_push(0, 190, UI_W, UI_H - 190);
    static const char *hdr[2] = {N_("Enabled"), N_("Disabled")};
    static const char *empty[2] = {N_("No apps are enabled"), N_("All apps are enabled")};
    for (int sec = 0; sec < 2; sec++) {
        float hy = top + L.hdr_y[sec];
        char t[48];
        snprintf(t, sizeof t, "%s (%d)", T(hdr[sec]), L.cnt[sec]);
        text_draw(F_BOLD, 34, 120, hy + 8, C_TEXT, AL_LEFT, t);
        if (!L.cnt[sec]) text_draw(F_REG, 24, 120, hy + STORE_HDR + 10, C_TEXT2, AL_LEFT, T(empty[sec]));
    }
    for (int k = 0; k < L.n; k++) {
        int i = L.c[k].app;
        float x = 120 + L.c[k].col * (cw + 30), y = top + L.c[k].y;
        if (y > UI_H || y + ch < 190) continue;
        bool f = k == sel, on = app_on(i);
        if (f) draw_shadow(x, y, cw, ch, 22, 26, 1);
        fill_rrect(x, y, cw, ch, 22, f ? C_SURFACE2 : C_SURFACE);
        fill_rrect_vgrad(x + 30, y + 30, 110, 110, 26, apps[i].c1, apps[i].c2);
        draw_icon(apps[i].icon, x + 85, y + 85, 56, apps[i].fg);
        text_draw_clip(F_BOLD, 32, x + 165, y + 34, cw - 195, C_TEXT, AL_LEFT, T(apps[i].name));
        text_draw_wrap(F_REG, 22, x + 165, y + 80, cw - 195, 2, 30, C_TEXT2, T(apps[i].desc));
        const char *pill = on ? T("On") : T("Off");
        Color pc = on ? C_OK : RGBA(255, 255, 255, 40);
        float pw = text_width(F_SEMI, 22, pill) + 36;
        fill_rrect(x + 30, y + ch - 60, pw, 38, 19, pc);
        text_draw(F_SEMI, 22, x + 30 + pw / 2, y + ch - 53, C_TEXT, AL_CENTER, pill);
        if (f) stroke_rrect(x - 4, y - 4, cw + 8, ch + 8, 26, 4, C_FOCUS);
    }
    clip_pop();
}

/* move to the nearest cell in another row (same column or the last in the row) */
static int store_row_pick(const StoreLayout *L, int row, int col) {
    int best = -1;
    for (int k = 0; k < L->n; k++)
        if (L->c[k].row == row && L->c[k].col <= col) best = k;
    return best;
}

static bool store_input(Screen *s, const InputEvent *ev) {
    Store *st = s->st;
    StoreLayout L;
    store_layout(&L);
    int k = store_find(&L, st->sel_app);
    if (k < 0) return ev->a == A_LEFT || ev->a == A_RIGHT || ev->a == A_UP || ev->a == A_DOWN || ev->a == A_OK;
    StoreCell c = L.c[k];
    int t = -1;
    switch (ev->a) {
    case A_LEFT: if (c.col > 0) t = k - 1; break;
    case A_RIGHT: if (k + 1 < L.n && L.c[k + 1].row == c.row) t = k + 1; break;
    case A_UP: if (c.row > 0) t = store_row_pick(&L, c.row - 1, c.col); break;
    case A_DOWN: if (c.row + 1 < L.rows) t = store_row_pick(&L, c.row + 1, c.col); break;
    case A_OK:
        if (ev->repeat) return true;
        {
            char key[48];
            snprintf(key, sizeof key, "app_%s", apps[c.app].key);
            bool on = !app_on(c.app);
            cfg_set_int(key, on);
            app_toast(on ? T("%s is shown on the home screen") : T("%s is hidden"), T(apps[c.app].name));
            /* focus follows the app over into the other section */
        }
        return true;
    default: return false;
    }
    if (t >= 0) st->sel_app = L.c[t].app;
    return true;
}

Screen *appstore_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "appstore";
    s->st = calloc(1, sizeof(Store));
    ((Store *)s->st)->sel_app = -1;
    s->update = store_update;
    s->draw = store_draw;
    s->input = store_input;
    return s;
}
