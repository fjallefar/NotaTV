/* NotaTV shell - setup guide: shown on first boot (cfg setup_done) and from Settings → System
 * Five steps with a progress line at the top: language → network → region → name → apps, then a summary.
 * Left: what the step is about. Right: the choices (a list, or the app grid). Daylight saving time follows the
 * region by itself (Settings → Date and time), and the phone page code is shown in the summary.
 * Everything is chosen here on the box; nothing is looked up online. */
#include "tv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ST_LANG, ST_NET, ST_REGION, ST_NAME, ST_APPS, ST_DONE, ST_N };

/* ---------- code for the phone page (cgi-bin/set checks it) ---------- */
void phone_pin_new(void) {
    unsigned v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(&v, sizeof v, 1, f) != 1) v = (unsigned)SDL_rand_bits();
    if (f) fclose(f);
    char pin[8];
    snprintf(pin, sizeof pin, "%06u", v % 1000000);
    cfg_set("phone_pin", pin);
}
const char *phone_pin(void) {
    if (!*cfg_get("phone_pin", "")) phone_pin_new();
    return cfg_get("phone_pin", "");
}
static void pin_typed(void *ud, const char *t) {
    void (*done)(void) = (void (*)(void))ud;
    if (!t) return;
    char pin[16] = "";
    size_t n = 0;
    for (const char *c = t; *c && n < 8; c++) if (*c >= '0' && *c <= '9') pin[n++] = *c;
    pin[n] = 0;
    if (n < 4) { app_toast(T("The code must be 4-8 digits")); return; }
    cfg_set("phone_pin", pin);
    app_toast(T("New code: %s"), pin);
    if (done) done();
}
static void pin_choice(void *ud, int idx) {
    if (idx == 0) { phone_pin_new(); app_toast(T("New code: %s"), phone_pin()); if (ud) ((void (*)(void))ud)(); }
    else if (idx == 1) keyboard_open(T("Phone page code (4-8 digits)"), "", false, pin_typed, ud);
}
void phone_pin_change(void (*done)(void)) {
    static const char *const items[] = {N_("New random code"), N_("Choose my own")};
    static const char *tr[2]; tr[0] = T(items[0]); tr[1] = T(items[1]);
    choice_open(T("Phone page code"), tr, 2, 0, pin_choice, (void *)done);
}


/* ---------- the guide ---------- */
#define LEFT_X 120.0f
#define LEFT_W 640.0f
#define RIGHT_X 900.0f
#define RIGHT_W 900.0f
#define ROW_H 84.0f
#define ROW_GAP 16.0f
#define APP_COLS 3
#define APP_H 136.0f
#define APP_GAP 20.0f
#define MAX_ROWS 8
#define MAX_GAPPS 32

typedef struct {
    int step, sel;
    float fade, dir; /* fade-in of the step, and the side it slides in from */
    bool on_continue; /* apps: focus on the Continue button below the grid */
    bool installing;  /* apps: the chosen apps are being downloaded */
} Setup;

static Setup *live;

/* a row in the list on the right; action rows open something (a chevron is drawn), current rows get a check */
typedef enum { R_NEXT, R_ACTION, R_PICK } RowKind;
typedef struct { const char *label; RowKind kind; bool current; int id; } Row;

/* name suggestions (translated when chosen, so the box gets a name in the user's language) */
static const char *const name_sugg[] = {N_("Living room"), N_("Bedroom"), N_("Kitchen"), N_("Office")};
#define N_SUGG (int)(sizeof name_sugg / sizeof name_sugg[0])

static int step_rows(const Setup *v, Row *r) {
    int n = 0;
    switch (v->step) {
    case ST_LANG:
        for (int i = 0; i18n_langs[i].code && n < MAX_ROWS; i++)
            r[n++] = (Row){i18n_langs[i].name, R_PICK, !strcmp(i18n_langs[i].code, i18n_lang()), i};
        break;
    case ST_NET:
        if (app_ip()[0]) {
            r[n++] = (Row){T("Continue"), R_NEXT, false, 0};
            r[n++] = (Row){T("Use Wi-Fi instead"), R_ACTION, false, 1};
        } else {
            r[n++] = (Row){T("Choose Wi-Fi network"), R_ACTION, false, 1};
            r[n++] = (Row){T("Continue without network"), R_NEXT, false, 0};
        }
        break;
    case ST_REGION:
        if (*cfg_get("tz_zone", "")) {
            r[n++] = (Row){T("Continue"), R_NEXT, false, 0};
            r[n++] = (Row){T("Choose another country"), R_ACTION, false, 1};
        } else r[n++] = (Row){T("Choose country"), R_ACTION, false, 1};
        break;
    case ST_NAME: {
        const char *cur = cfg_get("device_name", "NotaTV");
        bool listed = !strcmp(cur, "NotaTV");
        for (int i = 0; i < N_SUGG; i++) if (!strcmp(cur, T(name_sugg[i]))) listed = true;
        if (!listed) r[n++] = (Row){cur, R_PICK, true, -2};
        r[n++] = (Row){"NotaTV", R_PICK, !strcmp(cur, "NotaTV"), -1};
        for (int i = 0; i < N_SUGG; i++) r[n++] = (Row){T(name_sugg[i]), R_PICK, !strcmp(cur, T(name_sugg[i])), i};
        r[n++] = (Row){T("Other name…"), R_ACTION, false, 100};
        break;
    }
    default: break;
    }
    return n;
}

/* the apps that can be chosen: the App Store catalog, plus apps that are loaded already (installed earlier, or local
   packages during development). The choice is kept by id, so it survives the catalog arriving. */
typedef struct { char id[40]; const NotaApp *app; const CatApp *cat; } GApp;
static int guide_apps(GApp *out) {
    int n = 0;
    for (int i = 0; i < app_count() && n < MAX_GAPPS; i++) {
        const NotaApp *a = app_at(i);
        if (a->flags & APP_SYSTEM) continue;
        snprintf(out[n].id, sizeof out[n].id, "%s", a->id);
        out[n].app = a;
        out[n].cat = appcat_find(a->id);
        n++;
    }
    for (int i = 0; i < appcat_count() && n < MAX_GAPPS; i++) {
        const CatApp *c = appcat_at(i);
        if (app_find(c->id)) continue;
        if (c->nonfree && strcmp(cfg_get("store_nonfree", "0"), "1")) continue; /* closed software: only via the App Store's check box */
        snprintf(out[n].id, sizeof out[n].id, "%s", c->id);
        out[n].app = NULL;
        out[n].cat = c;
        n++;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            int a = out[j - 1].app ? out[j - 1].app->order : out[j - 1].cat->order, b = out[j].app ? out[j].app->order : out[j].cat->order;
            if (a <= b) break;
            GApp t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}
static char chosen[MAX_GAPPS][40];
static int n_chosen;
static bool chosen_ready;
static bool is_chosen(const char *id) {
    for (int i = 0; i < n_chosen; i++) if (!strcmp(chosen[i], id)) return true;
    return false;
}
static void set_chosen(const char *id, bool on) {
    for (int i = 0; i < n_chosen; i++)
        if (!strcmp(chosen[i], id)) { if (!on) { memmove(chosen[i], chosen[i + 1], (size_t)(n_chosen - i - 1) * sizeof chosen[0]); n_chosen--; } return; }
    if (on && n_chosen < MAX_GAPPS) snprintf(chosen[n_chosen++], sizeof chosen[0], "%s", id);
}
/* the first choice: nothing is ticked beforehand - only the apps that are installed already (the guide run again) */
static void init_chosen(void) {
    if (chosen_ready || appcat_state() == CAT_LOADING) return;
    GApp g[MAX_GAPPS];
    int n = guide_apps(g);
    if (!n) return;
    n_chosen = 0;
    for (int i = 0; i < n; i++) if (g[i].app && app_installed(g[i].app)) set_chosen(g[i].id, true);
    chosen_ready = true;
}

static void go(Setup *v, int step) {
    v->dir = step > v->step ? 1 : -1;
    v->step = step;
    v->sel = 0;
    v->on_continue = false;
    v->fade = 0;
    if (step == ST_APPS && appcat_state() != CAT_OK && appcat_state() != CAT_LOADING) appcat_refresh();
    if (step == ST_LANG || step == ST_NAME) { /* start on the current choice */
        Row r[MAX_ROWS];
        int n = step_rows(v, r);
        for (int i = 0; i < n; i++) if (r[i].current) v->sel = i;
    }
}
static void next_step(Setup *v) { if (v->step < ST_DONE) go(v, v->step + 1); }

static void tz_done(void *ud, bool ok) {
    Setup *v = ud;
    if (v != live) return;
    if (ok) next_step(v);
}
static void set_name(Setup *v, const char *name) {
    if (strcmp(name, cfg_get("device_name", "NotaTV"))) { cfg_set("device_name", name); settings_apply_name(); }
    next_step(v);
}
static void name_done(void *ud, const char *t) {
    Setup *v = ud;
    if (!t || v != live) return;
    char *n = str_dup(t);
    str_trim(n);
    if (*n) set_name(v, n);
    free(n);
}

static char pending_id[40];
static void uninstall_ok(void *ud, bool yes) {
    (void)ud;
    const NotaApp *a = app_find(pending_id);
    if (yes && a) { app_uninstall(a); set_chosen(pending_id, false); }
}
static void toggle_app(const GApp *g) {
    if (!is_chosen(g->id)) { set_chosen(g->id, true); return; }
    /* an installed app may hold a login - ask first (like the App Store) */
    if (g->app && app_installed(g->app)) {
        snprintf(pending_id, sizeof pending_id, "%s", g->id);
        char t[160];
        snprintf(t, sizeof t, T("Uninstall %s?"), T(g->app->name));
        confirm_open(t, T("The app disappears from the home screen, and everything it has saved on this box is deleted - also your sign-in. You can install it again later."),
                     T("Uninstall"), T("Cancel"), uninstall_ok, NULL);
    } else set_chosen(g->id, false);
}

/* Continue on the apps step: download the chosen apps that are not installed (or skip when there is no store) */
static void install_chosen(Setup *v) {
    GApp g[MAX_GAPPS];
    int n = guide_apps(g);
    for (int i = 0; i < n; i++) {
        if (!is_chosen(g[i].id) || (g[i].app && app_installed(g[i].app))) continue;
        if (g[i].cat) appcat_install(g[i].id);
        else if (g[i].app) app_install(g[i].app); /* a local package (development) */
    }
    if (appcat_busy(NULL, NULL, NULL)) v->installing = true;
    else next_step(v);
}

static void press(Setup *v) {
    Row r[MAX_ROWS];
    int n = step_rows(v, r);
    const Row *row = v->sel < n ? &r[v->sel] : NULL;
    switch (v->step) {
    case ST_LANG:
        if (!row) return;
        cfg_set("language", i18n_langs[row->id].code); /* always saved - also the default (English), see lang_init */
        if (strcmp(i18n_langs[row->id].code, i18n_lang())) {
            /* new language: restart the user interface; the guide continues with the network */
            cfg_set_int("setup_resume", 1);
            app_quit();
        } else next_step(v);
        break;
    case ST_NET: if (row && row->id == 1) settings_wifi(); else next_step(v); break;
    case ST_REGION: if (row && row->id == 1) tz_pick(tz_done, v); else next_step(v); break;
    case ST_NAME:
        if (!row) return;
        if (row->id == 100) keyboard_open(T("Name of the box"), cfg_get("device_name", "NotaTV"), false, name_done, v);
        else if (row->id == -1) set_name(v, "NotaTV");
        else if (row->id >= 0) set_name(v, T(name_sugg[row->id]));
        else next_step(v); /* the current custom name */
        break;
    case ST_APPS: {
        if (v->installing) break;
        GApp g[MAX_GAPPS];
        int na = guide_apps(g);
        if (!na) { /* no store: Try again / Continue without apps */
            if (appcat_state() == CAT_ERROR && v->sel == 0) appcat_refresh();
            else if (appcat_state() != CAT_LOADING) next_step(v);
        } else if (v->on_continue) install_chosen(v);
        else if (v->sel < na) toggle_app(&g[v->sel]);
        break;
    }
    default:
        cfg_set_int("setup_done", 1);
        cfg_set_int("apps_migrated", 1); /* the apps come from the App Store (nothing to move from NotaTV 1.0) */
        app_pop();
        break;
    }
}

static void setup_update(Screen *s, float dt) {
    Setup *v = s->st;
    v->fade = anim_to(v->fade, 1, dt, 7);
    if (v->step == ST_APPS) init_chosen();
    if (v->installing && !appcat_busy(NULL, NULL, NULL)) { v->installing = false; next_step(v); }
}

/* ---------- drawing ---------- */
static void draw_progress(const Setup *v) {
    static const char *const names[] = {N_("Language"), N_("Network"), N_("Region"), N_("Name"), N_("Apps")};
    int n = 5;
    float x0 = LEFT_X, w = (UI_W - 2 * LEFT_X - (n - 1) * 16) / n;
    for (int i = 0; i < n; i++) {
        float x = x0 + i * (w + 16);
        bool done = i < v->step, cur = i == v->step;
        fill_rrect(x, 64, w, 6, 3, done ? C_TEXT : cur ? C_ACCENT : RGBA(255, 255, 255, 40));
        text_draw(F_SEMI, 22, x, 84, done ? C_TEXT2 : cur ? C_TEXT : C_TEXT3, AL_LEFT, T(names[i]));
    }
}

static void draw_row(float x, float y, float w, const Row *r, bool f, float a) {
    if (f) draw_shadow(x, y, w, ROW_H, 18, 22, a);
    fill_rrect(x, y, w, ROW_H, 18, f ? color_alpha(C_FOCUS, a) : color_alpha(C_SURFACE, a));
    Color tc = f ? RGBA(10, 10, 10, (uint8_t)(255 * a)) : color_alpha(C_TEXT, a);
    text_draw_clip(F_SEMI, 30, x + 36, y + 24, w - 140, tc, AL_LEFT, r->label);
    float ix = x + w - 48, iy = y + ROW_H / 2;
    if (r->current) {
        fill_circle(ix, iy, 18, f ? RGBA(10, 10, 10, (uint8_t)(255 * a)) : color_alpha(C_OK, a));
        draw_icon(ICON_CHECK, ix, iy, 20, f ? color_alpha(C_FOCUS, a) : color_alpha(C_TEXT, a));
    } else if (r->kind == R_ACTION) fill_tri(ix - 6, iy - 10, ix + 6, iy, ix - 6, iy + 10, tc);
}

static void draw_card(float x, float y, float w, float h, float a) {
    fill_rrect(x, y, w, h, 22, color_alpha(RGB(24, 28, 37), a));
    stroke_rrect(x, y, w, h, 22, 2, RGBA(255, 255, 255, (uint8_t)(18 * a)));
}

static void draw_apps(const Setup *v, float x, float y, float a) {
    GApp g[MAX_GAPPS];
    int n = guide_apps(g);
    if (v->installing) { /* progress while the chosen apps are downloaded */
        int done = 0, total = 0;
        const char *cur = "";
        appcat_busy(&done, &total, &cur);
        draw_card(x, y, RIGHT_W, 200, a);
        draw_spinner(x + 80, y + 100, 30, color_alpha(C_TEXT, a));
        char t[160];
        snprintf(t, sizeof t, T("Installing apps… %d of %d"), done + 1 > total ? total : done + 1, total);
        text_draw(F_BOLD, 34, x + 140, y + 54, color_alpha(C_TEXT, a), AL_LEFT, t);
        text_draw_clip(F_REG, 28, x + 140, y + 108, RIGHT_W - 180, color_alpha(C_TEXT2, a), AL_LEFT, cur);
        text_draw(F_REG, 22, x + 40, y + 216, color_alpha(C_TEXT3, a), AL_LEFT, T("Back: continue - the apps keep downloading"));
        fill_rrect(x + 40, y + 164, RIGHT_W - 80, 8, 4, RGBA(255, 255, 255, (uint8_t)(30 * a)));
        if (total) fill_rrect(x + 40, y + 164, (RIGHT_W - 80) * done / total, 8, 4, color_alpha(C_ACCENT, a));
        return;
    }
    if (!n) { /* the catalog is on its way, or cannot be reached */
        bool err = appcat_state() == CAT_ERROR;
        draw_card(x, y, RIGHT_W, 170, a);
        if (!err) draw_spinner(x + 80, y + 85, 30, color_alpha(C_TEXT, a));
        text_draw(F_BOLD, 32, x + (err ? 40 : 140), y + 40, color_alpha(C_TEXT, a), AL_LEFT,
                  err ? T("The App Store cannot be reached") : T("Looking for apps in the App Store…"));
        text_draw_wrap(F_REG, 26, x + (err ? 40 : 140), y + 90, RIGHT_W - (err ? 80 : 180), 2, 34, color_alpha(C_TEXT2, a),
                       err ? T("Check the network, or continue and install apps later from the App Store.") : T("The apps are downloaded from NotaTV's App Store on GitHub."));
        if (err) {
            Row r[2] = {{T("Try again"), R_ACTION, false, 0}, {T("Continue without apps"), R_NEXT, false, 1}};
            for (int i = 0; i < 2; i++) draw_row(x, y + 200 + i * (ROW_H + ROW_GAP), RIGHT_W, &r[i], v->sel == i, a);
        }
        return;
    }
    float cw = (RIGHT_W - (APP_COLS - 1) * APP_GAP) / APP_COLS;
    for (int i = 0; i < n; i++) {
        const NotaApp *ap = g[i].app;
        const CatApp *c = g[i].cat;
        float cx = x + (i % APP_COLS) * (cw + APP_GAP), cy = y + (i / APP_COLS) * (APP_H + APP_GAP);
        bool f = !v->on_continue && v->sel == i, on = is_chosen(g[i].id);
        if (f) draw_shadow(cx, cy, cw, APP_H, 20, 22, a);
        fill_rrect(cx, cy, cw, APP_H, 20, color_alpha(f ? C_SURFACE2 : C_SURFACE, a));
        if (on) stroke_rrect(cx, cy, cw, APP_H, 20, 3, color_alpha(C_OK, a * 0.9f));
        if (!draw_app_art(g[i].id, cx + 22, cy + 30, 76, 76, 18, a)) {
            fill_rrect_vgrad(cx + 22, cy + 30, 76, 76, 18, color_alpha(ap ? ap->c1 : c->c1, a), color_alpha(ap ? ap->c2 : c->c2, a));
            draw_icon(ap ? ap->icon : c->icon, cx + 60, cy + 68, 40, color_alpha(ap ? ap->fg : c->fg, a));
        }
        text_draw_clip(F_SEMI, 26, cx + 116, cy + 46, cw - 170, color_alpha(C_TEXT, a), AL_LEFT, ap ? T(ap->name) : c->name);
        float bx = cx + cw - 34, by = cy + 34;
        if (on) { fill_circle(bx, by, 16, color_alpha(C_OK, a)); draw_icon(ICON_CHECK, bx, by, 18, color_alpha(C_TEXT, a)); }
        else stroke_rrect(bx - 16, by - 16, 32, 32, 16, 3, color_alpha(C_TEXT3, a));
        if (f) stroke_rrect(cx - 4, cy - 4, cw + 8, APP_H + 8, 24, 4, color_alpha(C_FOCUS, a));
    }
    int rows = (n + APP_COLS - 1) / APP_COLS;
    float by = y + rows * (APP_H + APP_GAP) + 10;
    Row cont = {T("Continue"), R_NEXT, false, 0};
    draw_row(x, by, RIGHT_W, &cont, v->on_continue, a);
}

static void setup_draw(Screen *s) {
    Setup *v = s->st;
    draw_background();
    fill_hgrad(0, 0, UI_W * 0.55f, UI_H, RGBA(79, 140, 255, 14), RGBA(79, 140, 255, 0));
    if (v->step < ST_DONE) draw_progress(v);
    float a = v->fade, dx = (1 - a) * 50 * v->dir;
    char buf[700];
    const char *title = "", *body = "", *kicker = "";
    IconId icon = ICON_TV;
    switch (v->step) {
    case ST_LANG:
        title = N_("Welcome to NotaTV");
        body = "Choose your language.\nVælg dit sprog.";
        break;
    case ST_NET:
        icon = ICON_WIFI;
        title = N_("Connect to the internet");
        body = N_("NotaTV uses the network for apps, streaming and updates. A cable is fastest; Wi-Fi works too.");
        break;
    case ST_REGION:
        icon = ICON_CLOCK;
        title = N_("Where is the box?");
        body = N_("Choose the country (and region), so the clock is right. Daylight saving time follows the region by itself.");
        break;
    case ST_NAME:
        title = N_("Name your box");
        body = N_("The name is shown when you send to the TV from your phone - AirPlay, Bluetooth, Plex and YouTube.");
        break;
    case ST_APPS:
        icon = ICON_CHECK;
        title = N_("Choose your apps");
        body = N_("Select the apps you want on the home screen. You can install and uninstall apps later in the App Store.");
        break;
    default:
        icon = ICON_CHECK;
        title = N_("You're all set");
        body = N_("NotaTV is ready. You can run this guide again under Settings → System.");
        break;
    }
    if (v->step < ST_DONE) {
        snprintf(buf, sizeof buf, T("STEP %d OF %d"), v->step + 1, 5);
        kicker = buf;
    }
    /* left: what the step is about */
    float ly = 300;
    fill_rrect_vgrad(LEFT_X + dx, ly, 120, 120, 30, color_alpha(RGB(79, 140, 255), a), color_alpha(RGB(40, 80, 190), a));
    draw_icon(icon, LEFT_X + 60 + dx, ly + 60, 60, color_alpha(C_TEXT, a));
    if (*kicker) text_draw(F_SEMI, 22, LEFT_X + dx, ly + 170, color_alpha(C_ACCENT, a), AL_LEFT, kicker);
    int tl = text_draw_wrap(F_BOLD, 60, LEFT_X + dx, ly + 206, LEFT_W, 2, 70, color_alpha(C_TEXT, a), T(title));
    text_draw_wrap(F_REG, 30, LEFT_X + dx, ly + 226 + tl * 70, LEFT_W, 5, 42, color_alpha(C_TEXT2, a), v->step == ST_LANG ? body : T(body));

    /* right: the choices */
    float rx = RIGHT_X + dx, ry = 300;
    char line[256];
    if (v->step == ST_NET) {
        const char *ip = app_ip();
        draw_card(rx, ry, RIGHT_W, 130, a);
        fill_circle(rx + 70, ry + 65, 34, color_alpha(ip[0] ? C_OK : C_DANGER, a));
        draw_icon(ICON_WIFI, rx + 70, ry + 65, 34, color_alpha(C_TEXT, a));
        text_draw(F_BOLD, 32, rx + 130, ry + 28, color_alpha(C_TEXT, a), AL_LEFT, ip[0] ? T("Connected") : T("Not connected"));
        if (ip[0]) snprintf(line, sizeof line, T("IP address %s"), ip);
        else snprintf(line, sizeof line, "%s", T("Plug in a network cable, or choose a Wi-Fi network"));
        text_draw_clip(F_REG, 26, rx + 130, ry + 74, RIGHT_W - 160, color_alpha(C_TEXT2, a), AL_LEFT, line);
        ry += 130 + 30;
    } else if (v->step == ST_REGION) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char clk[16];
        snprintf(clk, sizeof clk, "%02d:%02d", tm.tm_hour, tm.tm_min);
        draw_card(rx, ry, RIGHT_W, 150, a);
        text_draw(F_BOLD, 72, rx + 40, ry + 30, color_alpha(C_TEXT, a), AL_LEFT, clk);
        float cw = text_width(F_BOLD, 72, clk);
        text_draw_clip(F_SEMI, 28, rx + 80 + cw, ry + 44, RIGHT_W - cw - 120, color_alpha(C_TEXT, a), AL_LEFT, tz_label());
        text_draw(F_REG, 24, rx + 80 + cw, ry + 86, color_alpha(C_TEXT2, a), AL_LEFT, T("The clock shown on the box"));
        ry += 150 + 30;
    }
    if (v->step == ST_APPS) draw_apps(v, rx, ry, a);
    else if (v->step == ST_DONE) {
        int inst = 0;
        for (int i = 0; i < app_count(); i++) inst += !(app_at(i)->flags & APP_SYSTEM) && app_installed(app_at(i));
        const char *lang = "English";
        for (const LangInfo *l = i18n_langs; l->code; l++) if (!strcmp(l->code, i18n_lang())) lang = l->name;
        const char *ip = app_ip();
        char apps[64], net[96];
        snprintf(apps, sizeof apps, T("%d installed"), inst);
        snprintf(net, sizeof net, "%s", ip[0] ? T("Connected") : T("Not connected"));
        const char *k[] = {T("Language"), T("Network"), T("Region"), T("Name"), T("Apps")};
        const char *val[] = {lang, net, tz_label(), cfg_get("device_name", "NotaTV"), apps};
        float ch = 5 * 64 + 40;
        draw_card(rx, ry, RIGHT_W, ch, a);
        for (int i = 0; i < 5; i++) {
            float yy = ry + 30 + i * 64;
            text_draw(F_REG, 28, rx + 40, yy, color_alpha(C_TEXT2, a), AL_LEFT, k[i]);
            text_draw_clip(F_SEMI, 28, rx + RIGHT_W - 40, yy, RIGHT_W * 0.6f, color_alpha(C_TEXT, a), AL_RIGHT, val[i]);
            if (i < 4) fill_rect(rx + 40, yy + 50, RIGHT_W - 80, 1, RGBA(255, 255, 255, (uint8_t)(20 * a)));
        }
        ry += ch + 24;
        /* the phone page: the most useful tip for typing logins and links */
        if (ip[0]) snprintf(line, sizeof line, T("Tip: control the box and type from your phone at http://%s - code %s"), ip, phone_pin());
        else snprintf(line, sizeof line, T("Tip: when the box is online, you can control it and type from your phone (Settings → Network)"));
        text_draw_wrap(F_REG, 24, rx + 4, ry, RIGHT_W, 2, 32, color_alpha(C_TEXT2, a), line);
        ry += 100;
        Row go_home = {T("Go to the home screen"), R_NEXT, false, 0};
        draw_row(rx, ry, RIGHT_W, &go_home, true, a);
    } else {
        Row r[MAX_ROWS];
        int n = step_rows(v, r);
        for (int i = 0; i < n; i++) draw_row(rx, ry + i * (ROW_H + ROW_GAP), RIGHT_W, &r[i], v->sel == i, a);
    }

    const char *hint = v->step == ST_APPS ? T("Arrows: choose   ·   OK: select or deselect   ·   Back: previous step")
                     : v->step == ST_DONE ? T("OK: go to the home screen   ·   Back: previous step")
                     : v->step == ST_LANG && !cfg_int("setup_done", 0) ? T("↑ ↓ choose   ·   OK: select")
                     : T("↑ ↓ choose   ·   OK: select   ·   Back: previous step");
    text_draw(F_REG, 22, UI_W / 2, UI_H - 64, C_TEXT3, AL_CENTER, hint);
}

static bool apps_nav(Setup *v, const InputEvent *ev) {
    GApp a[MAX_GAPPS];
    int n = guide_apps(a), rows = (n + APP_COLS - 1) / APP_COLS;
    if (v->installing) return true;
    if (!n) { /* Try again / Continue without apps */
        if (ev->a == A_UP && v->sel > 0) v->sel--;
        if (ev->a == A_DOWN && v->sel < 1) v->sel++;
        return true;
    }
    if (v->on_continue) {
        if (ev->a == A_UP && n) { v->on_continue = false; if (v->sel >= n) v->sel = n - 1; }
        return ev->a == A_UP || ev->a == A_DOWN || ev->a == A_LEFT || ev->a == A_RIGHT;
    }
    int r = v->sel / APP_COLS, c = v->sel % APP_COLS;
    switch (ev->a) {
    case A_LEFT: if (c > 0) v->sel--; return true;
    case A_RIGHT: if (c < APP_COLS - 1 && v->sel + 1 < n) v->sel++; return true;
    case A_UP: if (r > 0) v->sel -= APP_COLS; return true;
    case A_DOWN:
        if (r + 1 < rows) v->sel = v->sel + APP_COLS < n ? v->sel + APP_COLS : n - 1;
        else v->on_continue = true;
        return true;
    default: return false;
    }
}

static bool setup_input(Screen *s, const InputEvent *ev) {
    Setup *v = s->st;
    if (ev->a == A_BACK) {
        if (v->installing) { /* do not wait (a slow connection can take long): the downloads go on in the background */
            v->installing = false;
            app_toast(T("The apps keep downloading in the background"));
            next_step(v);
            return true;
        }
        if (v->step > 0) go(v, v->step - 1);
        else if (cfg_int("setup_done", 0)) app_pop(); /* run again from Settings: Back on the first step leaves */
        return true;
    }
    if (ev->a == A_OK) { if (!ev->repeat) press(v); return true; }
    if (v->step == ST_APPS) { apps_nav(v, ev); return true; }
    if (v->step == ST_DONE) return true;
    Row r[MAX_ROWS];
    int n = step_rows(v, r);
    if (ev->a == A_UP && v->sel > 0) v->sel--;
    else if (ev->a == A_DOWN && v->sel < n - 1) v->sel++;
    return true; /* the guide must be finished (Home etc. are ignored) */
}

static void setup_enter(Screen *s) { live = s->st; }
static void setup_leave(Screen *s) { if (live == s->st) live = NULL; }

Screen *setup_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "setup";
    Setup *v = calloc(1, sizeof(Setup));
    s->st = v;
    chosen_ready = false; /* a new run: the app choice starts over */
    v->dir = 1;
    if (cfg_int("setup_resume", 0)) { cfg_set("setup_resume", ""); v->step = ST_NET; } /* after a language change */
    else go(v, ST_LANG);
    v->dir = 1;
    s->enter = setup_enter;
    s->resume = setup_enter;
    s->leave = setup_leave;
    s->update = setup_update;
    s->draw = setup_draw;
    s->input = setup_input;
    return s;
}
