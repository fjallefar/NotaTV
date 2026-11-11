/* NotaTV shell - App Store: install, update and uninstall apps
 * The apps come from the catalog on GitHub (appcat.c); installed apps are shown even when the catalog cannot be
 * reached. Two sections, Installed and Not installed. OK on an app that is not installed downloads and installs it.
 * OK on an installed app offers Open, Update (when the catalog has a newer version) and Uninstall. Uninstalling
 * deletes everything the app has saved on the box (see app_uninstall). Settings and the App Store are not shown.
 * When apps have updates, a bar at the top updates them all at once. Before an update, a card shows what is new
 * (the manifest's "changes", see scripts/apps-catalog.py). A check box at the bottom lists apps that need closed
 * software (manifest "nonfree", e.g. a DRM module); they are hidden until it is ticked (cfg store_nonfree), and then
 * listed below it, apart from the other apps. Installed ones are in Installed like any other. */
#define _GNU_SOURCE /* nftw */
#include "tv.h"
#include "vendor/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ftw.h>

#define STORE_COLS 3
#define STORE_PITCH 250.0f
#define STORE_HDR 70.0f
#define STORE_GAP 50.0f
#define STORE_MAX 64
#define STORE_BAR 120.0f /* the "Update all" bar */
#define ALL_ID "*all"    /* its id in the layout (never an app id) */
#define NF_ID "*nonfree" /* the check box for apps with closed software */
#define NF_H 110.0f

static bool show_nonfree(void) { return !strcmp(cfg_get("store_nonfree", "0"), "1"); }

/* one app in the store: loaded (app) and/or in the catalog (cat) */
typedef struct {
    char id[40];
    const NotaApp *app;
    const CatApp *cat;
    bool installed;
    bool nonfree; /* needs closed software: listed under the check box */
} StoreItem;

typedef struct { char sel[40]; float sy; bool asked_restart; } Store;

typedef struct { StoreItem it; int row, col; float y; } StoreCell;
typedef struct { StoreCell c[STORE_MAX + 2]; int n, rows; float hdr_y[2]; int cnt[2], first_row[2]; } StoreLayout;

static int item_order(const StoreItem *s) { return s->app ? s->app->order : s->cat->order; }
static const char *item_name(const StoreItem *s) { return s->app ? T(s->app->name) : s->cat->name; }
static const char *item_desc(const StoreItem *s) { return s->app ? T(s->app->desc) : s->cat->desc; }

/* "48 KB" / "1.3 MB" (an app package is often well under 1 MB) */
static const char *fmt_size(long b, char *buf, size_t n) {
    if (b <= 0) buf[0] = 0;
    else if (b < 1000000) snprintf(buf, n, "%ld KB", (b + 999) / 1000);
    else snprintf(buf, n, "%.1f MB", b / 1e6);
    return buf;
}

/* bytes an installed app takes in <apps-pkg>/<id>; measured again only when apps change */
static long du_sum;
static int du_add(const char *p, const struct stat *sb, int type, struct FTW *f) {
    (void)p; (void)f;
    if (type == FTW_F) du_sum += sb->st_size;
    return 0;
}
static long app_disk_size(const NotaApp *a) {
    static struct { char id[40]; long size; int gen; } cache[STORE_MAX];
    static int nc;
    int gen = apps_gen(), i = 0;
    while (i < nc && strcmp(cache[i].id, a->id)) i++;
    if (i == nc) {
        if (nc == STORE_MAX) i = 0; else nc++;
        snprintf(cache[i].id, sizeof cache[i].id, "%s", a->id);
        cache[i].gen = gen - 1;
    }
    if (cache[i].gen != gen) {
        du_sum = 0;
        if (*app_pkg_dir(a)) nftw(app_pkg_dir(a), du_add, 8, FTW_PHYS);
        cache[i].size = du_sum;
        cache[i].gen = gen;
    }
    return cache[i].size;
}

/* from the package itself, so the app is in the right place before the catalog has arrived */
static bool pkg_nonfree(const NotaApp *a) {
    if (!*app_pkg_dir(a)) return false;
    char path[600];
    snprintf(path, sizeof path, "%s/manifest.json", app_pkg_dir(a));
    char *txt = file_read_all(path, 65536);
    if (!txt) return false;
    cJSON *m = cJSON_Parse(txt);
    free(txt);
    bool nf = m && cJSON_IsTrue(cJSON_GetObjectItem(m, "nonfree"));
    cJSON_Delete(m);
    return nf;
}

static int store_items(StoreItem *out) {
    int n = 0;
    for (int i = 0; i < app_count() && n < STORE_MAX; i++) {
        const NotaApp *a = app_at(i);
        if (a->flags & APP_SYSTEM) continue;
        StoreItem *s = &out[n++];
        snprintf(s->id, sizeof s->id, "%s", a->id);
        s->app = a;
        s->cat = appcat_find(a->id);
        s->installed = app_installed(a);
        s->nonfree = (s->cat && s->cat->nonfree) || pkg_nonfree(a);
    }
    for (int i = 0; i < appcat_count() && n < STORE_MAX; i++) {
        const CatApp *c = appcat_at(i);
        if (app_find(c->id) || (c->nonfree && !show_nonfree())) continue;
        StoreItem *s = &out[n++];
        snprintf(s->id, sizeof s->id, "%s", c->id);
        s->app = NULL;
        s->cat = c;
        s->installed = false;
        s->nonfree = c->nonfree;
    }
    for (int i = 1; i < n; i++) /* by order (insertion sort, stable) */
        for (int j = i; j > 0 && item_order(&out[j - 1]) > item_order(&out[j]); j--) { StoreItem t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}

/* y is in content coordinates (0 = top of the list); an empty section gets one row for its message */
static void store_layout_build(StoreLayout *L);
/* update, draw and input all need the layout every frame: it is only built again when something it depends on
   changed (apps loaded/installed, settings, the catalog, the install queue) */
static void store_layout(StoreLayout *L) {
    static StoreLayout cache;
    static int key[6] = {-1};
    int done = 0, total = 0;
    bool busy = appcat_busy(&done, &total, NULL);
    int now[6] = {apps_gen(), cfg_gen(), (int)appcat_state(), appcat_count(), done * 1000 + total, busy};
    if (memcmp(key, now, sizeof key)) { store_layout_build(&cache); memcpy(key, now, sizeof key); }
    *L = cache;
}
static void store_layout_build(StoreLayout *L) {
    StoreItem items[STORE_MAX];
    int n = store_items(items);
    L->n = 0;
    int row = 0;
    float y = 0;
    if (appcat_updates() > 0) {
        StoreCell *b = &L->c[L->n++];
        memset(b, 0, sizeof *b);
        snprintf(b->it.id, sizeof b->it.id, ALL_ID);
        row = 1;
        y = STORE_BAR + 30;
    }
    for (int sec = 0; sec < 2; sec++) {
        L->first_row[sec] = row;
        L->hdr_y[sec] = y;
        y += STORE_HDR;
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (items[i].installed != (sec == 0)) continue;
            if (items[i].nonfree) continue; /* listed under the check box, installed or not */
            L->c[L->n++] = (StoreCell){items[i], row + k / STORE_COLS, k % STORE_COLS, y + (k / STORE_COLS) * STORE_PITCH};
            k++;
        }
        L->cnt[sec] = k;
        int r = k ? (k + STORE_COLS - 1) / STORE_COLS : 0;
        row += r;
        y += (r ? r * STORE_PITCH : 80) + STORE_GAP;
    }
    StoreCell *nf = &L->c[L->n++]; /* the check box, in its own row at the bottom */
    memset(nf, 0, sizeof *nf);
    snprintf(nf->it.id, sizeof nf->it.id, NF_ID);
    nf->row = row++;
    nf->y = y;
    y += NF_H + 30;
    int k = 0; /* the apps with closed software, below the check box: installed ones always, the others when ticked */
    for (int i = 0; i < n; i++) {
        if (!items[i].nonfree) continue;
        L->c[L->n++] = (StoreCell){items[i], row + k / STORE_COLS, k % STORE_COLS, y + (k / STORE_COLS) * STORE_PITCH};
        k++;
    }
    row += (k + STORE_COLS - 1) / STORE_COLS;
    L->rows = row;
}

/* [x] Show apps with closed software */
static void draw_nonfree(float x, float y, float w, bool f) {
    if (y > UI_H || y + NF_H < 190) return;
    static const char nf_key[] = "nonfree";
    float fa = focus_amt(nf_key, f), fc = fa < 0 ? 0 : fa > 1 ? 1 : fa;
    focus_under(x, y, w, NF_H, 26, fa);
    fill_rrect(x, y, w, NF_H, 26, color_mix(C_SURFACE, C_SURFACE2, fc));
    bool on = show_nonfree();
    fill_rrect(x + 30, y + 31, 48, 48, 12, on ? C_ACCENT : RGBA(255, 255, 255, 40));
    if (on) draw_icon(ICON_CHECK, x + 54, y + 55, 30, C_TEXT);
    text_draw(F_BOLD, 28, x + 104, y + 18, C_TEXT, AL_LEFT, T("Show apps that need closed software"));
    text_draw_clip(F_REG, 22, x + 104, y + 62, w - 134, C_TEXT2, AL_LEFT,
                   T("Some streaming services need a closed module from another company to play (DRM). It is not open source."));
    focus_over(x, y, w, NF_H, 26, fa);
}

static int store_find(const StoreLayout *L, const char *id) {
    for (int i = 0; i < L->n; i++) if (!strcmp(L->c[i].it.id, id)) return i;
    return L->n ? 0 : -1;
}

static void restart_cb(void *ud, bool yes) { (void)ud; if (yes) app_quit(); }

static void store_update(Screen *s, float dt) {
    Store *st = s->st;
    StoreLayout L;
    store_layout(&L);
    int i = store_find(&L, st->sel);
    if (i >= 0) {
        snprintf(st->sel, sizeof st->sel, "%s", L.c[i].it.id);
        float t = L.c[i].y - 300;
        if (L.c[i].row == L.first_row[0] || L.c[i].row == L.first_row[1]) t -= STORE_HDR; /* show the section heading */
        st->sy = anim_to(st->sy, t > 0 ? t : 0, dt, 12);
    }
    /* an app was updated while it was running: the new version starts with the user interface */
    if (appcat_restart_needed() && !appcat_busy(NULL, NULL, NULL) && !st->asked_restart && app_top() == s) {
        st->asked_restart = true;
        confirm_open(T("Restart NotaTV?"), T("The updated apps are used after a restart of the user interface. It takes a few seconds."),
                     T("Restart now"), T("Later"), restart_cb, NULL);
    }
}

/* the bar at the top: "Update all (3)" and which apps */
static void draw_update_bar(float x, float y, float w, bool f) {
    if (y > UI_H || y + STORE_BAR < 190) return;
    static const char bar_key[] = "updbar";
    float fa = focus_amt(bar_key, f), sc = 1 + 0.02f * fa;
    float ow = w;
    w *= sc; x -= (w - ow) / 2; y -= STORE_BAR * (sc - 1) / 2;
    focus_under(x, y, w, STORE_BAR, 26, fa);
    fill_rrect(x, y, w, STORE_BAR, 26, color_mix(C_SURFACE, C_SURFACE2, fa < 0 ? 0 : fa > 1 ? 1 : fa));
    fill_rrect_vgrad(x + 24, y + 20, 80, 80, 20, RGB(16, 150, 140), RGB(8, 92, 96));
    draw_icon(ICON_CHECK, x + 64, y + 60, 40, C_TEXT);
    int n = appcat_updates();
    char t[96], names[400] = "";
    snprintf(t, sizeof t, n == 1 ? T("1 app has an update") : T("%d apps have an update"), n);
    text_draw(F_BOLD, 32, x + 130, y + 22, C_TEXT, AL_LEFT, t);
    for (int i = 0; i < appcat_count(); i++) {
        const CatApp *c = appcat_at(i);
        if (!appcat_has_update(c->id)) continue;
        size_t l = strlen(names);
        snprintf(names + l, sizeof names - l, "%s%s %s", l ? ",  " : "", c->name, c->version);
    }
    text_draw_clip(F_REG, 24, x + 130, y + 66, w - 130 - 330, C_TEXT2, AL_LEFT, names);
    bool busy = appcat_busy(NULL, NULL, NULL);
    const char *pill = busy ? T("Updating…") : T("Update all");
    float pw = text_width(F_SEMI, 26, pill) + 60;
    fill_rrect(x + w - pw - 30, y + 34, pw, 52, 26, f ? C_FOCUS : C_ACCENT);
    text_draw(F_SEMI, 26, x + w - 30 - pw / 2, y + 44, f ? RGB(10, 10, 10) : C_TEXT, AL_CENTER, pill);
    focus_over(x, y, w, STORE_BAR, 26, fa);
}

static void store_draw(Screen *s) {
    Store *st = s->st;
    draw_background();
    draw_topbar("App Store");
    const char *status;
    switch (appcat_state()) {
    case CAT_LOADING: status = T("Looking for apps…"); break;
    case CAT_ERROR: status = T("Could not reach the App Store - check the network. Installed apps are shown."); break;
    default: status = T("Install the apps you want on the home screen. Uninstalling an app deletes everything it has saved on this box."); break;
    }
    text_draw(F_REG, 26, 96, 130, appcat_state() == CAT_ERROR ? C_DANGER : C_TEXT2, AL_LEFT, status);
    StoreLayout L;
    store_layout(&L);
    int sel = store_find(&L, st->sel);
    float cw = (UI_W - 192 - (STORE_COLS - 1) * 30) / STORE_COLS, ch = 220, top = 200 - st->sy;
    clip_push(0, 190, UI_W, UI_H - 190);
    static const char *hdr[2] = {N_("Installed"), N_("Not installed")};
    static const char *empty[2] = {N_("No apps are installed"), N_("All apps are installed")};
    for (int sec = 0; sec < 2; sec++) {
        float hy = top + L.hdr_y[sec];
        char t[48];
        snprintf(t, sizeof t, "%s (%d)", T(hdr[sec]), L.cnt[sec]);
        text_draw(F_BOLD, 34, 96, hy + 8, C_TEXT, AL_LEFT, t);
        if (!L.cnt[sec]) {
            const char *e = sec == 1 && appcat_state() == CAT_LOADING ? T("Looking for apps…")
                          : sec == 1 && appcat_state() == CAT_ERROR ? T("The App Store cannot be reached right now") : T(empty[sec]);
            text_draw(F_REG, 24, 96, hy + STORE_HDR + 10, C_TEXT2, AL_LEFT, e);
        }
    }
    for (int k = 0; k < L.n; k++) {
        const StoreItem *it = &L.c[k].it;
        float x = 96 + L.c[k].col * (cw + 30), y = top + L.c[k].y;
        bool f = k == sel;
        if (!strcmp(it->id, ALL_ID)) { draw_update_bar(96, y, UI_W - 192, f); continue; }
        if (!strcmp(it->id, NF_ID)) { draw_nonfree(96, y, UI_W - 192, f); continue; }
        if (y > UI_H || y + ch < 190) continue;
        /* the card lifts like everywhere else (the key: the app, which outlives this frame's layout) */
        float fa = focus_amt(it->app ? (const void *)it->app : (const void *)it->cat, f), fc = fa < 0 ? 0 : fa > 1 ? 1 : fa;
        float cs = 1 + 0.035f * fa, cw0 = cw, ch0 = ch;
        x -= cw0 * (cs - 1) / 2; y -= ch0 * (cs - 1) / 2;
        float cw = cw0 * cs, ch = ch0 * cs;
        IconId icon = it->app ? it->app->icon : it->cat->icon;
        Color c1 = it->app ? it->app->c1 : it->cat->c1, c2 = it->app ? it->app->c2 : it->cat->c2, fg = it->app ? it->app->fg : it->cat->fg;
        focus_under(x, y, cw, ch, 26, fa);
        fill_rrect(x, y, cw, ch, 26, color_mix(C_SURFACE, C_SURFACE2, fc));
        if (!draw_app_art(it->id, x + 30, y + 30, 110, 110, 26, 1)) {
            fill_rrect_vgrad(x + 30, y + 30, 110, 110, 26, c1, c2);
            draw_icon(icon, x + 85, y + 85, 56, fg);
        }
        text_draw_clip(F_BOLD, 32, x + 165, y + 34, cw - 195, C_TEXT, AL_LEFT, item_name(it));
        int pend = appcat_pending(it->id);
        bool upd = appcat_has_update(it->id);
        if (upd && it->cat->news[0]) { /* an update: what is new instead of the description */
            char t[240];
            snprintf(t, sizeof t, T("New: %s"), it->cat->news);
            text_draw_wrap(F_REG, 22, x + 165, y + 80, cw - 195, 2, 30, C_ACCENT, t);
        } else text_draw_wrap(F_REG, 22, x + 165, y + 80, cw - 195, 2, 30, C_TEXT2, item_desc(it));
        /* a status only when it says something: installed apps without an update have none */
        const char *pill = pend == 2 ? T("Installing…") : pend == 1 ? T("Waiting…")
                         : upd ? T("Update") : it->installed ? NULL : T("Install");
        if (pill) {
            Color pc = pend ? RGBA(255, 255, 255, 40) : upd ? C_ACCENT : color_mix(RGBA(255, 255, 255, 40), C_ACCENT, fc);
            float pw = text_width(F_SEMI, 22, pill) + 36 + (pend == 2 ? 30 : 0);
            fill_rrect(x + 30, y + ch - 60, pw, 38, 19, pc);
            if (pend == 2) draw_spinner(x + 52, y + ch - 41, 9, C_TEXT);
            text_draw(F_SEMI, 22, x + 30 + pw / 2 + (pend == 2 ? 14 : 0), y + ch - 53, C_TEXT, AL_CENTER, pill);
        }
        /* installed: the version and what it takes on the box; not installed (also one uninstalled since the
           last restart): the catalog version and how much it will take */
        char v[96], sz[24];
        if (upd) snprintf(v, sizeof v, "%s → %s", app_version(it->app), it->cat->version);
        else if (it->installed && *app_next_version(it->app)) snprintf(v, sizeof v, T("%s after a restart"), app_next_version(it->app));
        else if (it->installed && *app_version(it->app)) {
            fmt_size(app_disk_size(it->app), sz, sizeof sz);
            if (*sz) snprintf(v, sizeof v, "%s  ·  %s", app_version(it->app), sz);
            else snprintf(v, sizeof v, T("Version %s"), app_version(it->app));
        } else if (it->cat) {
            fmt_size(it->cat->isize ? it->cat->isize : it->cat->size, sz, sizeof sz);
            snprintf(v, sizeof v, *sz ? "%s  ·  %s" : "%s", it->cat->version, sz);
        } else if (it->app && *app_version(it->app)) snprintf(v, sizeof v, T("Version %s"), app_version(it->app));
        else v[0] = 0;
        text_draw(F_REG, 20, pill ? x + cw - 30 : x + 30, y + ch - 50, C_TEXT3, pill ? AL_RIGHT : AL_LEFT, v);
        focus_over(x, y, cw, ch, 26, fa);
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

static char pending_id[40]; /* the app a dialog is about */
static int menu_kind[3];    /* what the items of the installed-app menu do */
enum { M_OPEN, M_UPDATE, M_UNINSTALL };

static void update_confirmed(void *ud, bool yes) {
    (void)ud;
    if (yes) appcat_install(pending_id);
}
static void update_all_confirmed(void *ud, bool yes) {
    (void)ud;
    if (yes) appcat_update_all();
}

/* the card with what is new before an update (straight to the update when the catalog says nothing) */
static void ask_update(const NotaApp *a, const CatApp *c) {
    if (!c->news[0] && !c->n_points) { appcat_install(a->id); return; }
    char title[160];
    snprintf(title, sizeof title, "%s %s", T(a->name), c->version);
    const char *pts[4];
    for (int i = 0; i < c->n_points; i++) pts[i] = c->points[i];
    notes_open(T("WHAT'S NEW"), title, c->news, pts, c->n_points, T("Update"), T("Cancel"), update_confirmed, NULL);
}
static void ask_update_all(void) {
    const char *pts[8];
    char lines[8][200];
    int n = 0, total = appcat_updates();
    for (int i = 0; i < appcat_count() && n < 8; i++) {
        const CatApp *c = appcat_at(i);
        if (!appcat_has_update(c->id)) continue;
        if (c->news[0]) snprintf(lines[n], sizeof lines[n], "%s %s - %s", c->name, c->version, c->news);
        else snprintf(lines[n], sizeof lines[n], "%s %s", c->name, c->version);
        pts[n] = lines[n];
        n++;
    }
    char title[96];
    snprintf(title, sizeof title, total == 1 ? T("Update 1 app") : T("Update %d apps"), total);
    notes_open(T("WHAT'S NEW"), title, NULL, pts, n, T("Update all"), T("Cancel"), update_all_confirmed, NULL);
}

static void uninstall_confirmed(void *ud, bool yes) {
    (void)ud;
    const NotaApp *a = app_find(pending_id);
    if (!yes || !a) return;
    app_uninstall(a);
    app_toast(T("%s is uninstalled"), T(a->name));
}

static void installed_menu(void *ud, int idx) {
    (void)ud;
    const NotaApp *a = app_find(pending_id);
    if (!a || idx < 0 || idx > 2) return;
    switch (menu_kind[idx]) {
    case M_OPEN: { Screen *sc = a->open ? a->open() : NULL; if (sc) app_push(sc); break; }
    case M_UPDATE: { const CatApp *c = appcat_find(a->id); if (c) ask_update(a, c); break; }
    default: {
        char t[160];
        snprintf(t, sizeof t, T("Uninstall %s?"), T(a->name));
        confirm_open(t, T("The app disappears from the home screen, and everything it has saved on this box is deleted - also your sign-in. You can install it again later."),
                     T("Uninstall"), T("Cancel"), uninstall_confirmed, NULL);
    }
    }
}

static bool store_input(Screen *s, const InputEvent *ev) {
    Store *st = s->st;
    StoreLayout L;
    store_layout(&L);
    int k = store_find(&L, st->sel);
    if (k < 0) return ev->a == A_LEFT || ev->a == A_RIGHT || ev->a == A_UP || ev->a == A_DOWN || ev->a == A_OK;
    StoreCell c = L.c[k];
    int t = -1;
    switch (ev->a) {
    case A_LEFT: if (c.col > 0) t = k - 1; break;
    case A_RIGHT: if (k + 1 < L.n && L.c[k + 1].row == c.row) t = k + 1; break;
    case A_UP: if (c.row > 0) t = store_row_pick(&L, c.row - 1, c.col); break;
    case A_DOWN: if (c.row + 1 < L.rows) t = store_row_pick(&L, c.row + 1, c.col); break;
    case A_OK:
        if (ev->repeat || appcat_pending(c.it.id)) return true;
        if (!strcmp(c.it.id, ALL_ID)) { ask_update_all(); return true; }
        if (!strcmp(c.it.id, NF_ID)) { cfg_set("store_nonfree", show_nonfree() ? "0" : "1"); return true; }
        snprintf(pending_id, sizeof pending_id, "%s", c.it.id);
        if (c.it.installed) {
            const char *items[3];
            char upd[64];
            int n = 0;
            menu_kind[n] = M_OPEN; items[n++] = T("Open");
            if (appcat_has_update(c.it.id)) {
                snprintf(upd, sizeof upd, T("Update to version %s"), c.it.cat->version);
                menu_kind[n] = M_UPDATE; items[n++] = upd;
            }
            menu_kind[n] = M_UNINSTALL; items[n++] = T("Uninstall");
            choice_open(item_name(&c.it), items, n, 0, installed_menu, NULL);
        } else if (c.it.app && !c.it.cat) { /* development: a local package that is not installed */
            app_install(c.it.app);
            app_toast(T("%s is installed - you find it on the home screen"), item_name(&c.it));
        } else appcat_install(c.it.id); /* download (also when it was uninstalled earlier this session) */
        return true;
    default: return false;
    }
    if (t >= 0) snprintf(st->sel, sizeof st->sel, "%s", L.c[t].it.id);
    return true;
}

static void store_enter(Screen *s) {
    (void)s;
    if (appcat_state() != CAT_LOADING) appcat_refresh(); /* fresh each time the store opens */
}

Screen *appstore_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "appstore";
    s->st = xcalloc(1, sizeof(Store));
    s->enter = store_enter;
    s->update = store_update;
    s->draw = store_draw;
    s->input = store_input;
    return s;
}
