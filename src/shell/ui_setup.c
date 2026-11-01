/* NotaTV shell - setup guide: shown on first boot (cfg setup_done) and from Settings → System
 * Steps: language → welcome → network → country and region → summer time → name → apps → done.
 * Everything is chosen here on the box; nothing is looked up online. */
#include "tv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ST_LANG, ST_WELCOME, ST_NET, ST_TZ, ST_DST, ST_NAME, ST_PHONE, ST_APPS, ST_DONE, ST_N };

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

typedef struct {
    int step, btn;
    float fade;
} Setup;

static Setup *live;

static void next_step(Setup *v) {
    v->step++;
    /* summer time only makes sense if the region has it */
    if (v->step == ST_DST && !tz_zone_has_dst(cfg_get("tz_zone", ""))) v->step++;
    v->btn = 0;
    v->fade = 0;
}
static void prev_step(Setup *v) {
    if (v->step == 0) return;
    v->step--;
    if (v->step == ST_DST && !tz_zone_has_dst(cfg_get("tz_zone", ""))) v->step--;
    v->btn = 0;
    v->fade = 0;
}

static void tz_done(void *ud, bool ok) {
    Setup *v = ud;
    if (v != live) return;
    if (ok) next_step(v);
}
static void name_done(void *ud, const char *t) {
    Setup *v = ud;
    if (!t || v != live) return;
    char *n = str_dup(t);
    str_trim(n);
    if (*n) { cfg_set("device_name", n); settings_apply_name(); next_step(v); }
    free(n);
}

/* buttons per step; the first button is the "natural" one (focus starts there) */
static int step_buttons(int step, const char **b) {
    switch (step) {
    case ST_LANG: { int n = 0; for (const LangInfo *l = i18n_langs; l->code && n < 4; l++) b[n++] = l->name; return n; }
    case ST_WELCOME: b[0] = N_("Start"); return 1;
    case ST_NET: b[0] = N_("Next"); b[1] = N_("Choose Wi-Fi"); return 2;
    case ST_TZ:
        if (*cfg_get("tz_zone", "")) { b[0] = N_("Next"); b[1] = N_("Choose another country"); return 2; }
        b[0] = N_("Choose country"); return 1;
    case ST_DST: b[0] = N_("Follow daylight saving time"); b[1] = N_("Standard time all year"); return 2;
    case ST_NAME: b[0] = N_("Keep the name"); b[1] = N_("Change name"); return 2;
    case ST_PHONE: b[0] = N_("Next"); b[1] = N_("Change code"); return 2;
    case ST_APPS: b[0] = N_("Next"); b[1] = N_("Choose apps"); return 2;
    default: b[0] = N_("Go to the home screen"); return 1;
    }
}

static void press(Setup *v) {
    switch (v->step) {
    case ST_LANG:
        if (strcmp(i18n_langs[v->btn].code, i18n_lang())) {
            /* new language: save and restart the user interface; the guide continues after the language step */
            cfg_set("language", i18n_langs[v->btn].code);
            cfg_set_int("setup_resume", 1);
            app_quit();
        } else next_step(v);
        break;
    case ST_WELCOME: next_step(v); break;
    case ST_NET: if (v->btn == 0) next_step(v); else settings_wifi(); break;
    case ST_TZ:
        if (*cfg_get("tz_zone", "") && v->btn == 0) next_step(v);
        else tz_pick(tz_done, v);
        break;
    case ST_DST:
        cfg_set_int("tz_dst", v->btn == 0);
        tz_apply();
        next_step(v);
        break;
    case ST_NAME:
        if (v->btn == 0) next_step(v);
        else keyboard_open(T("Name of the box (shown in AirPlay, Plex etc.)"), cfg_get("device_name", "NotaTV"), false, name_done, v);
        break;
    case ST_PHONE: if (v->btn == 0) next_step(v); else phone_pin_change(NULL); break;
    case ST_APPS: if (v->btn == 0) next_step(v); else app_push(appstore_screen()); break;
    default:
        cfg_set_int("setup_done", 1);
        app_pop();
        break;
    }
}

static void setup_update(Screen *s, float dt) {
    Setup *v = s->st;
    v->fade = anim_to(v->fade, 1, dt, 8);
}

static void setup_draw(Screen *s) {
    Setup *v = s->st;
    draw_background();
    float cx = UI_W / 2;
    /* fremskridt: prikker */
    for (int i = 0; i < ST_N; i++) {
        float x = cx + (i - (ST_N - 1) / 2.0f) * 34;
        fill_circle(x, 90, i == v->step ? 9 : 6, i <= v->step ? C_TEXT : C_TEXT3);
    }
    float a = v->fade;
    Color t1 = color_alpha(C_TEXT, a), t2 = color_alpha(C_TEXT2, a);
    const char *title = "", *body = "";
    char buf[600];
    IconId icon = ICON_TV;
    switch (v->step) {
    case ST_LANG:
        icon = ICON_GEAR;
        title = "Language  ·  Sprog";
        body = "Choose the language for NotaTV.\nVælg sproget til NotaTV.";
        break;
    case ST_WELCOME:
        title = N_("Welcome to NotaTV");
        body = N_("Let's get the box ready. It takes a minute.\n\nEverything is chosen here on the box - nothing is looked up online.");
        break;
    case ST_NET: {
        icon = ICON_WIFI;
        title = N_("Network");
        const char *ip = app_ip();
        if (ip[0]) snprintf(buf, sizeof buf, T("The box is connected (IP address %s).\n\nIf you would rather use Wi-Fi, choose it here."), ip);
        else snprintf(buf, sizeof buf, T("The box is not connected yet.\n\nPlug in a network cable, or choose a Wi-Fi network."));
        body = buf;
        break;
    }
    case ST_TZ:
        icon = ICON_CLOCK;
        title = N_("Country and region");
        if (*cfg_get("tz_zone", "")) snprintf(buf, sizeof buf, T("Selected: %s\n\nThe clock follows this region."), tz_label());
        else snprintf(buf, sizeof buf, T("Choose the country (and region, if any) where the box is, so the clock is right."));
        body = buf;
        break;
    case ST_DST:
        icon = ICON_CLOCK;
        title = N_("Daylight saving time");
        snprintf(buf, sizeof buf, T("%s has daylight saving time.\n\nShould the clock switch between summer and winter time by itself, or show standard time all year?"), tz_label());
        body = buf;
        break;
    case ST_NAME:
        icon = ICON_TV;
        title = N_("Name of the box");
        snprintf(buf, sizeof buf, T("The box is called \"%s\".\n\nThe name is shown when you send to the box from your phone (AirPlay, Bluetooth, Plex, YouTube)."),
                 cfg_get("device_name", "NotaTV"));
        body = buf;
        break;
    case ST_PHONE: {
        icon = ICON_LOCK;
        title = N_("Phone page");
        const char *ip = app_ip();
        snprintf(buf, sizeof buf,
                 T("From your phone you can control the box and type text at  http://%s\n\nThe page is locked with this code:   %s\n\nLogins and server addresses sent from the phone must always be approved here on the TV."),
                 ip[0] ? ip : T("<box IP>"), phone_pin());
        body = buf;
        break;
    }
    case ST_APPS:
        icon = ICON_CHECK;
        title = N_("Apps on the home screen");
        body = N_("Choose which apps should be shown on the home screen. You can always change it later in the App Store.");
        break;
    default:
        icon = ICON_CHECK;
        title = N_("All set");
        body = N_("Enjoy NotaTV.\n\nYou can run the guide again under Settings → System.");
        break;
    }
    draw_icon(icon, cx, 300, 110, t1);
    text_draw(F_BOLD, 48, cx, 420, t1, AL_CENTER, v->step == ST_LANG ? title : T(title));
    text_draw_wrap(F_REG, 30, cx - 560, 500, 1120, 6, 42, t2, body == buf || v->step == ST_LANG ? body : T(body));
    if (v->step == ST_TZ || v->step == ST_DST) { /* show the clock, so you can see that it is right */
        char clk[32];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(clk, sizeof clk, "%02d:%02d", tm.tm_hour, tm.tm_min);
        text_draw(F_BOLD, 40, UI_W - 120, 60, t1, AL_RIGHT, clk);
    }
    const char *b[4];
    int n = step_buttons(v->step, b);
    if (v->step != ST_LANG) for (int i = 0; i < n; i++) b[i] = T(b[i]);
    draw_buttons(cx, 760, b, n, v->btn);
    text_draw(F_REG, 22, cx, UI_H - 70, C_TEXT3, AL_CENTER, v->step > 0 ? T("← → choose   ·   OK: press   ·   Back: previous step") : T("OK: press"));
}

static bool setup_input(Screen *s, const InputEvent *ev) {
    Setup *v = s->st;
    const char *b[4];
    int n = step_buttons(v->step, b);
    if (buttons_nav(&v->btn, n, ev)) return true;
    if (ev->a == A_OK) { if (!ev->repeat) press(v); return true; }
    if (ev->a == A_BACK) { prev_step(v); return true; }
    return true; /* the guide must be finished (Home etc. are ignored) */
}

static void setup_enter(Screen *s) { live = s->st; }
static void setup_leave(Screen *s) { if (live == s->st) live = NULL; }

Screen *setup_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "setup";
    Setup *v = calloc(1, sizeof(Setup));
    s->st = v;
    if (cfg_int("setup_resume", 0)) { cfg_set("setup_resume", ""); v->step = ST_WELCOME; } /* after a language change */
    else for (int i = 0; i18n_langs[i].code; i++) if (!strcmp(i18n_langs[i].code, i18n_lang())) v->btn = i;
    s->enter = setup_enter;
    s->resume = setup_enter;
    s->leave = setup_leave;
    s->update = setup_update;
    s->draw = setup_draw;
    s->input = setup_input;
    return s;
}
