/* NotaTV shell - home screen */
#include "tv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* the apps on the home screen: the installed ones from the app register (apps.c), in their order */
#define MAX_VIS 64
static const NotaApp *vis[MAX_VIS];
static int n_vis;
/* the user's own order (Green on a tile moves it) is kept in cfg home_order: "plex,iptv,...". Apps that are not
   in it (installed later) go right after the app before them in the default order, or first */
static int reg_index(const NotaApp *a) {
    for (int i = 0; i < app_count(); i++) if (app_at(i) == a) return i;
    return -1;
}
static void build_vis(void) {
    n_vis = 0;
    char ord[1024];
    snprintf(ord, sizeof ord, "%s", cfg_get("home_order", "")); /* copy: cfg_get's pointer can change */
    bool placed[MAX_VIS] = {0}; /* by register index (MAX_VIS >= the register's size) */
    for (char *sv = NULL, *id = strtok_r(ord, ",", &sv); id; id = strtok_r(NULL, ",", &sv)) {
        const NotaApp *a = app_find(id);
        int ri = a ? reg_index(a) : -1;
        if (ri < 0 || ri >= MAX_VIS || placed[ri] || !app_installed(a) || n_vis >= MAX_VIS) continue;
        placed[ri] = true;
        vis[n_vis++] = a;
    }
    for (int i = 0; i < app_count() && i < MAX_VIS && n_vis < MAX_VIS; i++) {
        const NotaApp *a = app_at(i);
        if (placed[i] || !app_installed(a)) continue;
        int at = 0; /* after the nearest app before it in the default order that is already on the home screen */
        for (int p = i - 1; p >= 0 && !at; p--)
            for (int k = 0; k < n_vis; k++) if (vis[k] == app_at(p)) { at = k + 1; break; }
        memmove(&vis[at + 1], &vis[at], (n_vis - at) * sizeof vis[0]);
        vis[at] = a;
        n_vis++;
        placed[i] = true;
    }
}

static void save_order(void) {
    char ord[1024] = "";
    size_t len = 0;
    for (int i = 0; i < n_vis; i++) {
        int w = snprintf(ord + len, sizeof ord - len, "%s%s", i ? "," : "", vis[i]->id);
        if (w < 0 || (size_t)w >= sizeof ord - len) break;
        len += w;
    }
    cfg_set("home_order", ord);
}

/* move the tile at from to place to (the others shift one step) */
static void move_tile(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= n_vis || to >= n_vis) return;
    const NotaApp *a = vis[from];
    if (from < to) memmove(&vis[from], &vis[from + 1], (to - from) * sizeof vis[0]);
    else memmove(&vis[to + 1], &vis[to], (from - to) * sizeof vis[0]);
    vis[to] = a;
}

/* app grid: up to 6 per row, left-aligned on the screen margin (the same edge as the header and the rows) */
#define MARGIN 96.0f
#define TILE_W 248.0f
#define TILE_H 140.0f
#define TILE_GAP 40.0f
#define TILE_PITCH 214.0f /* tile + room for the name of the focused one */
#define TILE_R 26.0f
#define APPS_Y 500.0f
static int grid_cols(void) { return n_vis < 6 ? (n_vis ? n_vis : 1) : 6; }
static int grid_rows(void) { return (n_vis + grid_cols() - 1) / grid_cols(); }

typedef struct {
    int row, col, col2;
    Spring foc[MAX_VIS];  /* focus per tile (0..1 with a small overshoot) */
    Spring foc2[64];      /* focus per item in the row below the apps */
    const NotaApp *tint_app, *tint_prev; /* the background glow follows the focused app (cross-faded) */
    float tint_fade;
    float sx2; /* scroll in row 2 */
    float row_anim;
    HomeItem *cont;
    int n_cont;
    const NotaApp *cont_app; /* the row below the apps follows the app in focus */
    bool keep_row;           /* moved down onto an app without a row: the row of the app above stays (see pick_cont_app) */
    double refreshed, fetched;
    /* "top shelf": a big image from the focused app behind the top (drawn in a layer) */
    char bg_url[512], prev_url[512], bgl_key[600];
    float bg_fade, bgl_fade;
    Layer *bgl;
    bool moving;       /* move mode (Green on a tile): the arrows move the focused tile, OK/Back ends it */
    float rest, hint_a; /* time on the same tile; the "Move app" hint fades in after a while there */
    float wiggle;
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

/* row 2 per app: title (NULL = no row) */
static const char *cont_title(const NotaApp *a) { return a && a->row ? T(a->row_title ? a->row_title : a->name) : NULL; }

/* the row below the apps: the focused app's, if it has one. Moving DOWN onto an app without a row keeps the row of
 * the app above, so e.g. "Recent channels" can be reached through the second row of apps; moving sideways to an
 * app without a row shows no row (it used to follow along all the way to Settings) */
static bool app_visible(const NotaApp *app) {
    for (int i = 0; i < n_vis; i++) if (vis[i] == app) return true;
    return false;
}
static const NotaApp *pick_cont_app(const Home *h) {
    const NotaApp *a = vis[h->col];
    if (cont_title(a)) return a;
    if (h->keep_row && cont_title(h->cont_app) && app_visible(h->cont_app)) return h->cont_app;
    return a;
}

static bool str_same(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }
static bool item_same(const HomeItem *a, const HomeItem *b) {
    return str_same(a->title, b->title) && str_same(a->subtitle, b->subtitle) && str_same(a->image, b->image) &&
           str_same(a->key, b->key) && str_same(a->logo, b->logo) && a->progress == b->progress;
}

/* removed from the row (Green on an item): kept in <data>/home-hidden.txt as app, progress, place, title, subtitle.
 * An item comes back by itself when you watch it again: its progress changes, or it becomes the first in the row.
 * Cleared on every sign-out (wipe_session_traces) and when the app is uninstalled. */
#define MAX_HID 64
typedef struct { char app[32]; double prog; int idx; char *title, *sub; } Hidden;
static Hidden hid[MAX_HID];
static int n_hid = -1; /* -1 = not loaded yet */
static void hid_path(char *p, size_t n) { snprintf(p, n, "%s/home-hidden.txt", data_dir()); }
static void hid_load(void) {
    if (n_hid >= 0) return;
    n_hid = 0;
    char p[512], line[1024];
    hid_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (n_hid < MAX_HID && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *sv, *app = strtok_r(line, "\t", &sv), *pr = strtok_r(NULL, "\t", &sv), *ix = strtok_r(NULL, "\t", &sv);
        char *ti = strtok_r(NULL, "\t", &sv), *su = strtok_r(NULL, "\t", &sv);
        if (!app || !pr || !ix || !ti) continue;
        Hidden *e = &hid[n_hid++];
        snprintf(e->app, sizeof e->app, "%s", app);
        e->prog = atof(pr);
        e->idx = atoi(ix);
        e->title = str_dup(ti);
        e->sub = su && strcmp(su, "-") ? str_dup(su) : NULL;
    }
    fclose(f);
}
static void hid_save(void) {
    char p[512], t[520];
    hid_path(p, sizeof p);
    if (n_hid <= 0) { unlink(p); return; }
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w");
    if (!f) return;
    for (int i = 0; i < n_hid; i++)
        fprintf(f, "%s\t%.4f\t%d\t%s\t%s\n", hid[i].app, hid[i].prog, hid[i].idx, hid[i].title, hid[i].sub && *hid[i].sub ? hid[i].sub : "-");
    fclose(f);
    rename(t, p);
}
/* no tabs or line breaks in what is saved */
static char *hid_clean(const char *s) {
    char *d = str_dup(s ? s : "");
    for (char *c = d; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
    return d;
}
static int hid_find(const char *app, const HomeItem *it) {
    char *t = hid_clean(it->title), *u = hid_clean(it->subtitle);
    int r = -1;
    for (int i = 0; i < n_hid && r < 0; i++)
        if (!strcmp(hid[i].app, app) && !strcmp(hid[i].title, t) && !strcmp(hid[i].sub ? hid[i].sub : "", u)) r = i;
    free(t); free(u);
    return r;
}
static void hid_del(int i) {
    free(hid[i].title); free(hid[i].sub);
    memmove(&hid[i], &hid[i + 1], (n_hid - i - 1) * sizeof hid[0]);
    n_hid--;
}
static void hid_add(const char *app, const HomeItem *it, int idx) {
    hid_load();
    if (hid_find(app, it) >= 0) return;
    if (n_hid == MAX_HID) hid_del(0); /* the oldest goes */
    Hidden *e = &hid[n_hid++];
    snprintf(e->app, sizeof e->app, "%s", app);
    e->prog = it->progress;
    e->idx = idx;
    char *t = hid_clean(it->title), *u = hid_clean(it->subtitle);
    e->title = t;
    e->sub = *u ? u : (free(u), NULL);
    hid_save();
}
/* sign-out / uninstall (called from wipe_session_traces and app_uninstall); app NULL = all apps */
void home_hidden_forget(const char *app) {
    hid_load();
    for (int i = n_hid - 1; i >= 0; i--) if (!app || !strcmp(hid[i].app, app)) hid_del(i);
    hid_save();
}

static void collect_continue(Home *h) {
    const NotaApp *app = pick_cont_app(h);
    HomeItem *all = NULL;
    int nall = app && app->row ? app->row(&all) : 0;
    if (!all) nall = 0;
    /* leave out what was removed from the row - unless it has been watched again since */
    hid_load();
    const HomeItem *a[64];
    int na = 0;
    bool dirty = false;
    for (int i = 0; i < nall && na < 64; i++) {
        int k = n_hid ? hid_find(app->id, &all[i]) : -1;
        if (k >= 0) {
            if (fabs(all[i].progress - hid[k].prog) > 0.01 || (i == 0 && hid[k].idx > 0)) { hid_del(k); dirty = true; } /* watched again */
            else { if (i != hid[k].idx) { hid[k].idx = i; dirty = true; } continue; }
        }
        a[na++] = &all[i];
    }
    if (dirty) hid_save();
    if (app == h->cont_app && na == h->n_cont && h->cont) { /* every 2 s: nothing to copy when nothing changed */
        bool same = true;
        for (int i = 0; i < na && same; i++) same = item_same(&h->cont[i], a[i]);
        if (same) return;
    }
    free_continue(h);
    h->cont_app = app;
    h->cont = calloc(na + 1, sizeof(HomeItem));
    for (int i = 0; i < na; i++) copy_item(&h->cont[h->n_cont++], a[i]);
    if (h->col2 >= h->n_cont) h->col2 = h->n_cont ? h->n_cont - 1 : 0;
    if (h->n_cont == 0 && h->row == 1) h->row = 0;
}

static void home_enter(Screen *s) {
    Home *h = s->st;
    if (h->moving) save_order();
    h->moving = false;
    build_vis();
    if (h->col >= n_vis) h->col = 0;
    for (int i = 0; i < n_vis; i++) if (vis[i]->row_refresh) vis[i]->row_refresh();
    collect_continue(h);
    h->refreshed = h->fetched = app_time();
}

static void home_resume(Screen *s) { home_enter(s); }

static void home_update(Screen *s, float dt) {
    Home *h = s->st;
    static int seen_gen = -1;
    if (seen_gen != apps_gen()) { /* apps installed/uninstalled meanwhile (e.g. downloaded in the background) */
        seen_gen = apps_gen();
        if (h->moving) save_order(); /* keep what has been moved so far */
        build_vis();
        if (h->col >= n_vis) h->col = 0;
        collect_continue(h);
    }
    if (h->moving) h->wiggle += dt;
    h->rest += dt;
    h->hint_a = anim_to(h->hint_a, h->rest > 3 && (h->row == 0 || h->n_cont > 0) && !h->moving ? 1 : 0, dt, h->rest > 3 ? 4 : 18);
    for (int i = 0; i < n_vis; i++)
        spring_to(&h->foc[i], (h->row == 0 && h->col == i && !h->moving) || (h->moving && h->col == i) ? 1 : 0, dt, SPRING_SNAPPY);
    for (int i = 0; i < 64; i++)
        spring_to(&h->foc2[i], h->row == 1 && h->col2 == i ? 1 : 0, dt, SPRING_SNAPPY);
    if (n_vis && vis[h->col] != h->tint_app) { h->tint_prev = h->tint_app; h->tint_app = vis[h->col]; h->tint_fade = 0; }
    h->tint_fade = anim_to(h->tint_fade, 1, dt, 9); /* short: two glow layers are drawn only while it fades */
    h->row_anim = anim_to(h->row_anim, (float)h->row, dt, 12);
    if (h->row == 0 && h->cont_app != pick_cont_app(h)) { h->col2 = 0; h->sx2 = 0; collect_continue(h); }
    h->bg_fade = anim_to(h->bg_fade, 1, dt, 6);
    /* the rows are fetched when the home screen opens and after playback (enter/resume); while it stays open they
       are fetched again every 5 minutes - not while nobody uses the box (screensaver, the box left on) */
    if (app_time() - h->refreshed > 2) {
        if (app_time() - h->fetched > 300 && app_time() - input_last_activity() < 600) {
            for (int i = 0; i < n_vis; i++) if (vis[i]->row_refresh) vis[i]->row_refresh();
            h->fetched = app_time();
        }
        collect_continue(h);
        h->refreshed = app_time();
    }
    float target = 0;
    float iw = 420, gap = 32; /* the row below the apps */
    float sel_x = h->col2 * (iw + gap);
    if (sel_x - target > UI_W - 2 * MARGIN - iw) target = sel_x - (UI_W - 2 * MARGIN - iw);
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

static void draw_shelf(Home *h, const NotaApp *a) {
    float x = MARGIN, y = 210;
    if (a->draw_shelf && a->draw_shelf(MARGIN, y)) return; /* the app's own (e.g. what is playing in Music) */
    /* default: the app's name and what it does - the tile below already shows its icon */
    text_draw(F_BOLD, 84, x - 4, y + 50, C_TEXT, AL_LEFT, T(a->name));
    text_draw_wrap(F_REG, 32, x, y + 160, 1100, 2, 42, C_TEXT2, T(a->desc));
    x -= 292; /* the lines below are placed relative to the old text column */
    if (!strcmp(a->id, "settings")) {
        char info[160];
        const char *ip = app_ip();
        snprintf(info, sizeof info, "NotaTV %s   ·   %s", TV_VERSION, ip[0] ? ip : T("No network connection"));
        text_draw(F_REG, 26, x + 292, y + 222, C_TEXT3, AL_LEFT, info);
    }
    int nu = !strcmp(a->id, "appstore") ? appcat_updates() : 0;
    if (nu > 0) {
        char t[96];
        snprintf(t, sizeof t, nu == 1 ? T("1 app has an update") : T("%d apps have an update"), nu);
        text_draw(F_SEMI, 26, x + 292, y + 222, C_ACCENT, AL_LEFT, t);
    }
}

static void home_draw(Screen *s) {
    Home *h = s->st;
    const NotaApp *focus_app = vis[h->col], *fa = focus_app;
    /* top shelf: Plex/YouTube show what you can continue (follows the choice in the row) */
    char want[512] = "";
    HomeItem *shelf_it = NULL;
    if (h->cont_app == focus_app && h->n_cont && !fa->draw_shelf) {
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
    snprintf(key, sizeof key, "%s|%d|%s", h->bg_url, ready, focus_app->id);
    bool settled = fa_ > 0.995f && h->bgl_fade > 0.995f;
    if ((!layer_valid(h->bgl) || strcmp(key, h->bgl_key) || !settled) && layer_begin(h->bgl)) {
        draw_background();
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
    /* a soft glow in the focused app's color from the top, cross-faded when the focus moves */
    if (h->tint_prev && h->tint_fade < 1)
        fill_vgrad(0, 0, UI_W, 760, color_alpha(h->tint_prev->c1, 0.28f * (1 - h->tint_fade)), RGBA(0, 0, 0, 0));
    if (h->tint_app) fill_vgrad(0, 0, UI_W, 760, color_alpha(h->tint_app->c1, 0.28f * h->tint_fade), RGBA(0, 0, 0, 0));

    char g[128];
    greeting(g, sizeof g);
    char *bar = strchr(g, '|');
    *bar = 0;
    text_draw(F_BOLD, 40, MARGIN, 58, C_TEXT, AL_LEFT, g);
    text_draw(F_REG, 26, MARGIN + text_width(F_BOLD, 40, g) + 24, 70, C_TEXT2, AL_LEFT, bar + 1);
    draw_topbar(NULL);
    const char *ip = app_ip();
    draw_icon(ip[0] ? ICON_WIFI : ICON_WIFI, UI_W - 250, 82, 34, ip[0] ? C_TEXT2 : C_DANGER);
    if (h->hint_a > 0.01f && n_vis > 1) { /* how to move a tile: Green (there is no "hold OK" on CEC) - only after
                                               resting on a tile for a while, so it is not mistaken for a status */
        const char *l = h->row == 1 ? T("Remove from the row") : T("Move app");
        float hx = UI_W - 310 - 36 - text_width(F_REG, 22, l);
        fill_rrect(hx - 14, 58, text_width(F_REG, 22, l) + 64, 46, 23, RGBA(255, 255, 255, (uint8_t)(16 * h->hint_a)));
        fill_circle(hx + 15, 81, 12, RGBA(64, 186, 92, (uint8_t)(255 * h->hint_a)));
        text_draw(F_REG, 22, hx + 36, 68, color_alpha(C_TEXT2, h->hint_a), AL_LEFT, l);
    }

    float shift = h->row_anim * 340; /* scroll up when row 2 is selected */
    clip_push(0, 150, UI_W, APPS_Y - 170);
    bool shelf_vis = h->row_anim < 0.2f; /* the row below the apps scrolls up over the top */
    if (shelf_vis && shelf_it && h->bg_url[0]) { /* big image behind: the title of what you can continue */
        float x = MARGIN, y = 230;
        text_draw(F_BOLD, 24, x, y, color_mix(fa->c1, C_TEXT, 0.45f), AL_LEFT,
                  T(fa->shelf_label ? fa->shelf_label : fa->name));
        int tl = text_draw_wrap(F_BOLD, 60, x, y + 40, 1100, 2, 70, C_TEXT, shelf_it->title ? shelf_it->title : "");
        float sy = y + 40 + tl * 70 + 10;
        if (shelf_it->subtitle) { text_draw_clip(F_REG, 30, x, sy, 1100, C_TEXT2, AL_LEFT, shelf_it->subtitle); sy += 50; }
        if (shelf_it->progress > 0) {
            fill_rrect(x, sy + 6, 420, 6, 3, RGBA(255, 255, 255, 60));
            fill_rrect(x, sy + 6, 420 * (float)shelf_it->progress, 6, 3, C_TEXT);
        }
    } else if (shelf_vis) draw_shelf(h, focus_app);
    clip_pop();

    int cols = grid_cols();
    int focus_i = -1; /* the focused tile is drawn last, on top of its neighbours */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n_vis; i++) {
        const NotaApp *a = vis[i];
        float f = h->foc[i].x, fc = f < 0 ? 0 : f > 1 ? 1 : f;
        bool top = fc > 0.02f && (focus_i < 0 || focus_i == i);
        if (pass == 0 && top) { focus_i = i; continue; }
        if (pass == 1 && i != focus_i) continue;
        int r = i / cols, c = i % cols;
        float x0 = MARGIN + c * (TILE_W + TILE_GAP), y0 = APPS_Y + r * TILE_PITCH - shift;
        if (y0 > UI_H || y0 + TILE_PITCH < 0) continue;
        float sc = 1 + 0.09f * f; /* lifts toward you, with the spring's small overshoot */
        float w = TILE_W * sc, hh = TILE_H * sc;
        float cx = x0 + TILE_W / 2, cy = y0 + TILE_H / 2;
        float x = cx - w / 2, y = cy - hh / 2 - 6 * f;
        bool mv = h->moving && i == h->col;
        if (mv) y -= 10 + 4 * sinf(h->wiggle * 6); /* picked up: lifted and bobbing */
        if (fc > 0.01f) {
            draw_glow(x, y, w, hh, TILE_R, 46, color_mix(a->c1, RGB(255, 255, 255), 0.15f), 0.55f * fc);
            draw_shadow(x, y, w, hh, TILE_R, 28, 0.9f * fc);
        }
        /* not focused: dark glass with a hint of the app's color; focused: the app's full color */
        if (draw_app_art(a->id, x, y, w, hh, TILE_R * sc, 1)) {
            /* the app's own icon; without focus it rests a little darker */
            if (fc < 0.99f) fill_rrect(x, y, w, hh, TILE_R * sc, RGBA(8, 10, 14, (uint8_t)(60 * (1 - fc))));
        } else {
            Color t1 = color_mix(color_mix(RGB(34, 38, 50), a->c1, 0.30f), a->c1, fc);
            Color t2 = color_mix(color_mix(RGB(24, 27, 36), a->c2, 0.22f), a->c2, fc);
            fill_rrect_vgrad(x, y, w, hh, TILE_R * sc, t1, t2);
            draw_icon(a->icon, cx, y + hh / 2, 66 * sc, color_alpha(a->fg, 0.82f + 0.18f * fc));
        }
        fill_rrect_vgrad(x, y, w, hh * 0.55f, TILE_R * sc, RGBA(255, 255, 255, (uint8_t)(6 + 26 * fc)), RGBA(255, 255, 255, 0)); /* light from above */
        stroke_rrect(x, y, w, hh, TILE_R * sc, 1.5f, RGBA(255, 255, 255, (uint8_t)(22 + 40 * fc)));
        if (mv) stroke_rrect(x - 5, y - 5, w + 10, hh + 10, TILE_R * sc + 5, 3, C_ACCENT);
        if (!strcmp(a->id, "appstore") && appcat_updates() > 0) { /* updates waiting: a small badge with the count */
            char nb[8];
            snprintf(nb, sizeof nb, "%d", appcat_updates());
            float bs = 40 * sc, bx = x + w - bs * 0.7f, by = y - bs * 0.3f;
            float bw = bs + (strlen(nb) > 1 ? text_width(F_BOLD, 24 * sc, nb) - 14 * sc : 0);
            fill_rrect(bx - (bw - bs) / 2, by, bw, bs, bs / 2, C_ACCENT);
            text_draw(F_BOLD, 24 * sc, bx + bs / 2, by + (bs - 24 * sc) / 2 - sc, C_TEXT, AL_CENTER, nb);
        }
        /* every tile has its name (not every app is well known); the focused one is bright and moves down with the lift */
        text_draw(F_SEMI, 26, cx, y0 + TILE_H + 18 + 5 * fc, color_mix(C_TEXT3, C_TEXT, fc), AL_CENTER, T(a->name));
    }

    if (h->moving) {
        text_draw(F_REG, 26, UI_W / 2, UI_H - 70, C_TEXT, AL_CENTER, T("Move with the arrows · OK when you are done"));
        return;
    }
    if (h->n_cont > 0) {
        float ry = APPS_Y + (grid_rows() - 1) * TILE_PITCH + 244 - shift;
        text_draw(F_SEMI, 30, MARGIN, ry - 48, h->row == 1 ? C_TEXT : C_TEXT2, AL_LEFT, cont_title(h->cont_app));
        clip_push(0, ry - 20, UI_W, 420);
        float iw = 420, ih = 236, gap2 = 32;
        for (int i = 0; i < h->n_cont; i++) {
            HomeItem *it = &h->cont[i];
            float x = MARGIN + i * (iw + gap2) - h->sx2, y = ry;
            if (x > UI_W || x + iw < 0) continue;
            bool f = h->row == 1 && h->col2 == i;
            float fs = i < 64 ? h->foc2[i].x : (f ? 1 : 0), fc = fs < 0 ? 0 : fs > 1 ? 1 : fs;
            float sc = 1 + 0.07f * fs;
            float w = iw * sc, hh = ih * sc;
            float xx = x - (w - iw) / 2, yy = y - (hh - ih) / 2 - 4 * fs;
            if (fc > 0.01f) draw_shadow(xx, yy, w, hh, 18, 26, fc);
            bool logo = h->cont_app && (h->cont_app->flags & APP_ROW_LOGOS); /* channel logos: cleaned, on the dark card */
            fill_rrect(xx, yy, w, hh, 18, logo ? color_mix(C_SURFACE, C_SURFACE2, fc) : C_SURFACE);
            Image *im = logo ? img_get_logo(it->image) : img_get(it->image);
            if (logo ? !img_draw(im, xx + w * 0.2f, yy + hh * 0.16f, w * 0.6f, hh * 0.42f, 12, 1) : !img_draw_cover(im, xx, yy, w, hh, 18, 1)) {
                /* no logo (none, still loading or not readable): the name on a logo card, otherwise the app's icon */
                if (logo && it->title && *it->title)
                    text_draw_wrap(F_BOLD, 34, xx + 28, yy + hh / 2 - 44, w - 56, 2, 42, C_TEXT2, it->title);
                else draw_icon(h->cont_app ? h->cont_app->icon : ICON_TV, xx + w / 2, yy + hh / 2 - 20, 70, C_TEXT3);
            }
            if (it->progress > 0) {
                fill_rrect(xx + 14, yy + hh - 16, w - 28, 6, 3, RGBA(0, 0, 0, 150));
                fill_rrect(xx + 14, yy + hh - 16, (w - 28) * (float)it->progress, 6, 3, C_TEXT);
            }
            if (!logo && fc < 0.99f) fill_rrect(xx, yy, w, hh, 18, RGBA(0, 0, 0, (uint8_t)(70 * (1 - fc)))); /* only the focused one is bright */
            stroke_rrect(xx, yy, w, hh, 18, 1.5f, RGBA(255, 255, 255, (uint8_t)(18 + 40 * fc)));
            /* title under the tile (like in Plex) - no text on the image */
            text_draw_clip(F_SEMI, 25, x, y + ih + 16, iw, f ? C_TEXT : C_TEXT2, AL_LEFT, it->title ? it->title : "");
            if (it->subtitle) text_draw_clip(F_REG, 22, x, y + ih + 48, iw, C_TEXT3, AL_LEFT, it->subtitle);
        }
        clip_pop();
    }
}

/* Green on an item in the row below the apps: remove it from the row (until it is watched again) */
static Home *rm_home;
static HomeItem rm_item;
static char rm_app[32];
static int rm_idx;
static void remove_cb(void *ud, bool yes) {
    (void)ud;
    if (yes && rm_home) {
        hid_add(rm_app, &rm_item, rm_idx);
        free_continue(rm_home); /* collected again without it */
        collect_continue(rm_home);
    }
    free(rm_item.title); free(rm_item.subtitle);
    rm_item = (HomeItem){0};
}
static void remove_ask(Home *h) {
    const HomeItem *it = &h->cont[h->col2];
    free(rm_item.title); free(rm_item.subtitle);
    rm_item = (HomeItem){str_dup(it->title), str_dup(it->subtitle), NULL, NULL, it->progress, NULL};
    snprintf(rm_app, sizeof rm_app, "%s", h->cont_app->id);
    /* its place in the app's own list (the row on screen leaves out what is removed) */
    HomeItem *all = NULL;
    int nall = h->cont_app->row ? h->cont_app->row(&all) : 0;
    rm_idx = h->col2;
    for (int i = 0; all && i < nall; i++)
        if (str_same(all[i].title, it->title) && str_same(all[i].subtitle, it->subtitle)) { rm_idx = i; break; }
    rm_home = h;
    char title[256];
    snprintf(title, sizeof title, T("Remove \"%s\" from the row?"), it->title ? it->title : "");
    confirm_open(title, T("It comes back by itself when you watch it again."), T("Remove"), T("Cancel"), remove_cb, NULL);
}

static bool home_input(Screen *s, const InputEvent *ev) {
    Home *h = s->st;
    h->rest = 0;
    if (h->moving) { /* move mode: the arrows move the tile, OK/Back/Home put it down */
        int cols = grid_cols(), to = h->col;
        switch (ev->a) {
        case A_LEFT: to = h->col - 1; break;
        case A_RIGHT: to = h->col + 1; break;
        case A_UP: if (h->col >= cols) to = h->col - cols; break;
        case A_DOWN: if (h->col / cols + 1 < grid_rows()) to = h->col + cols < n_vis ? h->col + cols : n_vis - 1; break;
        case A_OK: case A_INFO: case A_BACK: case A_HOME:
            h->moving = false;
            save_order();
            return true;
        case A_MENU: return true;
        default: return false;
        }
        if (to >= 0 && to < n_vis && to != h->col) {
            move_tile(h->col, to);
            Spring f = h->foc[h->col]; h->foc[h->col] = h->foc[to]; h->foc[to] = f; /* the focus glow moves with it */
            h->col = to;
        }
        return true;
    }
    switch (ev->a) {
    case A_LEFT:
        if (h->row == 0) { h->col = (h->col + n_vis - 1) % n_vis; h->keep_row = false; }
        else if (h->col2 > 0) h->col2--;
        return true;
    case A_RIGHT:
        if (h->row == 0) { h->col = (h->col + 1) % n_vis; h->keep_row = false; }
        else if (h->col2 < h->n_cont - 1) h->col2++;
        return true;
    case A_DOWN:
        if (h->row == 0) {
            int cols = grid_cols();
            if (h->col / cols + 1 < grid_rows()) { h->col = h->col + cols < n_vis ? h->col + cols : n_vis - 1; h->keep_row = true; }
            else if (h->n_cont > 0) h->row = 1;
        }
        return true;
    case A_UP:
        if (h->row == 1) h->row = 0;
        else if (h->col >= grid_cols()) { h->col -= grid_cols(); h->keep_row = false; }
        return true;
    case A_OK:
        if (h->row == 0) {
            /* the app grows out of its tile (and shrinks back into it on Back) */
            int cols = grid_cols(), r = h->col / cols, c = h->col % cols;
            float sc = 1.09f, w = TILE_W * sc, hh = TILE_H * sc;
            float cx = MARGIN + c * (TILE_W + TILE_GAP) + TILE_W / 2, cy = APPS_Y + r * TILE_PITCH - h->row_anim * 340 + TILE_H / 2;
            app_set_origin(cx - w / 2, cy - hh / 2 - 6, w, hh, TILE_R * sc);
            Screen *sc2 = vis[h->col]->open ? vis[h->col]->open() : NULL;
            if (sc2) app_push(sc2);
        } else if (h->col2 < h->n_cont && h->cont_app && h->cont_app->row_open) {
            float iw = 420, ih = 236, sc = 1.07f;
            float x = MARGIN + h->col2 * (iw + 32) - h->sx2, y = APPS_Y + (grid_rows() - 1) * TILE_PITCH + 244 - h->row_anim * 340;
            app_set_origin(x - iw * (sc - 1) / 2, y - ih * (sc - 1) / 2 - 4, iw * sc, ih * sc, 18);
            h->cont_app->row_open(&h->cont[h->col2], h->col2);
        }
        return true;
    case A_INFO: /* green: pick up the tile to move it (LG CEC has no real "held OK", see STATUS) */
        if (h->row == 0 && n_vis > 1 && !ev->repeat) { h->moving = true; h->wiggle = 0; }
        else if (h->row == 1 && h->col2 < h->n_cont && h->cont_app && !ev->repeat) remove_ask(h);
        return true;
    case A_MENU:
        power_menu_open();
        return true;
    case A_BACK: case A_HOME:
        h->row = 0;
        h->col = 0;
        h->keep_row = false;
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
