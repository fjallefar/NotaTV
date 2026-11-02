/* NotaTV shell - App Store: install, update and uninstall apps
 * The apps come from the catalog on GitHub (appcat.c); installed apps are shown even when the catalog cannot be
 * reached. Two sections, Installed and Not installed. OK on an app that is not installed downloads and installs it.
 * OK on an installed app offers Open, Update (when the catalog has a newer version) and Uninstall. Uninstalling
 * deletes everything the app has saved on the box (see app_uninstall). Settings and the App Store are not shown. */
#include "tv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORE_COLS 3
#define STORE_PITCH 250.0f
#define STORE_HDR 70.0f
#define STORE_GAP 50.0f
#define STORE_MAX 64

/* one app in the store: loaded (app) and/or in the catalog (cat) */
typedef struct {
    char id[40];
    const NotaApp *app;
    const CatApp *cat;
    bool installed;
} StoreItem;

typedef struct { char sel[40]; float sy; bool asked_restart; } Store;

typedef struct { StoreItem it; int row, col; float y; } StoreCell;
typedef struct { StoreCell c[STORE_MAX]; int n, rows; float hdr_y[2]; int cnt[2]; } StoreLayout;

static int item_order(const StoreItem *s) { return s->app ? s->app->order : s->cat->order; }
static const char *item_name(const StoreItem *s) { return s->app ? T(s->app->name) : s->cat->name; }
static const char *item_desc(const StoreItem *s) { return s->app ? T(s->app->desc) : s->cat->desc; }

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
    }
    for (int i = 0; i < appcat_count() && n < STORE_MAX; i++) {
        const CatApp *c = appcat_at(i);
        if (app_find(c->id)) continue;
        StoreItem *s = &out[n++];
        snprintf(s->id, sizeof s->id, "%s", c->id);
        s->app = NULL;
        s->cat = c;
        s->installed = false;
    }
    for (int i = 1; i < n; i++) /* by order (insertion sort, stable) */
        for (int j = i; j > 0 && item_order(&out[j - 1]) > item_order(&out[j]); j--) { StoreItem t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}

/* y is in content coordinates (0 = top of the list); an empty section gets one row for its message */
static void store_layout(StoreLayout *L) {
    StoreItem items[STORE_MAX];
    int n = store_items(items);
    L->n = 0;
    int row = 0;
    float y = 0;
    for (int sec = 0; sec < 2; sec++) {
        L->hdr_y[sec] = y;
        y += STORE_HDR;
        int k = 0;
        for (int i = 0; i < n; i++) {
            if (items[i].installed != (sec == 0)) continue;
            L->c[L->n++] = (StoreCell){items[i], row + k / STORE_COLS, k % STORE_COLS, y + (k / STORE_COLS) * STORE_PITCH};
            k++;
        }
        L->cnt[sec] = k;
        int r = k ? (k + STORE_COLS - 1) / STORE_COLS : 0;
        row += r;
        y += (r ? r * STORE_PITCH : 80) + STORE_GAP;
    }
    L->rows = row;
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
        if (L.c[i].row == 0 || (L.cnt[0] && L.c[i].row == (L.cnt[0] + STORE_COLS - 1) / STORE_COLS)) t -= STORE_HDR; /* show the section heading */
        st->sy = anim_to(st->sy, t > 0 ? t : 0, dt, 12);
    }
    /* an app was updated while it was running: the new version starts with the user interface */
    if (appcat_restart_needed() && !appcat_busy(NULL, NULL, NULL) && !st->asked_restart && app_top() == s) {
        st->asked_restart = true;
        confirm_open(T("Restart NotaTV?"), T("The updated apps are used after a restart of the user interface. It takes a few seconds."),
                     T("Restart now"), T("Later"), restart_cb, NULL);
    }
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
    text_draw(F_REG, 26, 120, 130, appcat_state() == CAT_ERROR ? C_DANGER : C_TEXT2, AL_LEFT, status);
    StoreLayout L;
    store_layout(&L);
    int sel = store_find(&L, st->sel);
    float cw = (UI_W - 240 - (STORE_COLS - 1) * 30) / STORE_COLS, ch = 220, top = 200 - st->sy;
    clip_push(0, 190, UI_W, UI_H - 190);
    static const char *hdr[2] = {N_("Installed"), N_("Not installed")};
    static const char *empty[2] = {N_("No apps are installed"), N_("All apps are installed")};
    for (int sec = 0; sec < 2; sec++) {
        float hy = top + L.hdr_y[sec];
        char t[48];
        snprintf(t, sizeof t, "%s (%d)", T(hdr[sec]), L.cnt[sec]);
        text_draw(F_BOLD, 34, 120, hy + 8, C_TEXT, AL_LEFT, t);
        if (!L.cnt[sec]) {
            const char *e = sec == 1 && appcat_state() == CAT_LOADING ? T("Looking for apps…")
                          : sec == 1 && appcat_state() == CAT_ERROR ? T("The App Store cannot be reached right now") : T(empty[sec]);
            text_draw(F_REG, 24, 120, hy + STORE_HDR + 10, C_TEXT2, AL_LEFT, e);
        }
    }
    for (int k = 0; k < L.n; k++) {
        const StoreItem *it = &L.c[k].it;
        float x = 120 + L.c[k].col * (cw + 30), y = top + L.c[k].y;
        if (y > UI_H || y + ch < 190) continue;
        bool f = k == sel;
        IconId icon = it->app ? it->app->icon : it->cat->icon;
        Color c1 = it->app ? it->app->c1 : it->cat->c1, c2 = it->app ? it->app->c2 : it->cat->c2, fg = it->app ? it->app->fg : it->cat->fg;
        if (f) draw_shadow(x, y, cw, ch, 22, 26, 1);
        fill_rrect(x, y, cw, ch, 22, f ? C_SURFACE2 : C_SURFACE);
        fill_rrect_vgrad(x + 30, y + 30, 110, 110, 26, c1, c2);
        draw_icon(icon, x + 85, y + 85, 56, fg);
        text_draw_clip(F_BOLD, 32, x + 165, y + 34, cw - 195, C_TEXT, AL_LEFT, item_name(it));
        text_draw_wrap(F_REG, 22, x + 165, y + 80, cw - 195, 2, 30, C_TEXT2, item_desc(it));
        int pend = appcat_pending(it->id);
        bool upd = appcat_has_update(it->id);
        const char *pill = pend == 2 ? T("Installing…") : pend == 1 ? T("Waiting…")
                         : upd ? T("Update") : it->installed ? T("Installed") : T("Install");
        Color pc = pend ? RGBA(255, 255, 255, 40) : upd ? C_ACCENT : it->installed ? C_OK : (f ? C_ACCENT : RGBA(255, 255, 255, 40));
        float pw = text_width(F_SEMI, 22, pill) + 36 + (pend == 2 ? 30 : 0);
        fill_rrect(x + 30, y + ch - 60, pw, 38, 19, pc);
        if (pend == 2) draw_spinner(x + 52, y + ch - 41, 9, C_TEXT);
        text_draw(F_SEMI, 22, x + 30 + pw / 2 + (pend == 2 ? 14 : 0), y + ch - 53, C_TEXT, AL_CENTER, pill);
        char v[64];
        if (upd) snprintf(v, sizeof v, "%s → %s", it->app->version, it->cat->version);
        else if (it->app && it->app->version) snprintf(v, sizeof v, T("Version %s"), it->app->version);
        else if (it->cat) snprintf(v, sizeof v, "%s  ·  %.1f MB", it->cat->version, it->cat->size / 1048576.0);
        else v[0] = 0;
        text_draw(F_REG, 20, x + cw - 30, y + ch - 50, C_TEXT3, AL_RIGHT, v);
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

static char pending_id[40]; /* the app a dialog is about */
static int menu_kind[3];    /* what the items of the installed-app menu do */
enum { M_OPEN, M_UPDATE, M_UNINSTALL };

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
    case M_UPDATE: appcat_install(a->id); break;
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
    s->st = calloc(1, sizeof(Store));
    s->enter = store_enter;
    s->update = store_update;
    s->draw = store_draw;
    s->input = store_input;
    return s;
}
