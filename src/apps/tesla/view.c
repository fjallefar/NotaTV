/* NotaTV app package "tesla" - the car view: cards laid out in four columns (scroll with up/down), and a side
 * menu (Left/Red) to choose the car, what is shown (cfg tesla_f_<name>), and which cars are at the top of the
 * home screen (cfg tesla_home). */
#define _GNU_SOURCE
#include "tesla.h"
#include "vendor/cJSON.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NO_ICON ((IconId)99) /* draw_icon draws nothing for it: a feature that is off */
#define WARN RGB(240, 120, 90)
#define GOOD RGB(90, 200, 120)

/* ---------- formatting ---------- */

const char *tm_ctx(const char *t) {
    const char *p = strchr(t, '|');
    return p ? p + 1 : t;
}
#define OPEN tm_ctx(T("door|Open"))

bool tm_true(const char *v) { return v && !strcmp(v, "true"); }
bool tm_has(const char *v) { return v && *v; }

const char *tm_num(char *buf, size_t n, const char *v, int d) {
    if (!tm_has(v)) { snprintf(buf, n, "-"); return buf; }
    snprintf(buf, n, "%.*f", d, atof(v));
    if (!strcmp(i18n_lang(), "da")) for (char *p = buf; *p; p++) if (*p == '.') *p = ',';
    return buf;
}

static const char *numf(char *buf, size_t n, double v, int d) {
    char t[32];
    snprintf(t, sizeof t, "%f", v);
    return tm_num(buf, n, t, d);
}

const char *tm_num_unit(char *buf, size_t n, const char *v, int d, const char *unit) {
    char t[48];
    if (!tm_has(v)) snprintf(buf, n, "-");
    else snprintf(buf, n, "%s %s", tm_num(t, sizeof t, v, d), unit);
    return buf;
}

/* "2026-11-02T14:32:10.123Z" -> local "14:32" (today) or "2/11 14:32" */
const char *tm_since(char *buf, size_t n, const char *iso) {
    buf[0] = 0;
    struct tm tm = {0};
    if (!tm_has(iso) || !strptime(iso, "%Y-%m-%dT%H:%M:%S", &tm)) return buf;
    time_t t = timegm(&tm), now = time(NULL);
    struct tm lt, ln;
    localtime_r(&t, &lt);
    localtime_r(&now, &ln);
    if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) strftime(buf, n, "%H:%M", &lt);
    else snprintf(buf, n, "%d/%d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
    return buf;
}

static const char *minutes_text(char *buf, size_t n, int m) {
    if (m <= 0) snprintf(buf, n, "-");
    else if (m < 60) snprintf(buf, n, T("%d min"), m);
    else snprintf(buf, n, T("%d h %d min"), m / 60, m % 60);
    return buf;
}

/* hours as a float ("1.25") -> "1 h 15 min" */
static const char *hours_text(char *buf, size_t n, const char *v) {
    if (!tm_has(v)) { snprintf(buf, n, "-"); return buf; }
    return minutes_text(buf, n, (int)lround(atof(v) * 60));
}

const char *tm_state_text(const TmCar *c) {
    const char *s = tm_get(c, "state");
    if (!strcmp(s, "charging")) return T("Charging");
    if (!strcmp(s, "driving")) return T("Driving");
    if (!strcmp(s, "asleep")) return T("Asleep");
    if (!strcmp(s, "offline")) return T("Offline");
    if (!strcmp(s, "suspended")) return T("Falling asleep");
    if (!strcmp(s, "updating")) return T("Installing a software update");
    if (!strcmp(s, "online")) return T("Parked");
    if (!strcmp(s, "start")) return T("Starting");
    return tm_has(s) ? s : T("Unknown");
}

Color tm_state_color(const TmCar *c) {
    const char *s = tm_get(c, "state");
    if (!strcmp(s, "charging")) return RGB(64, 196, 110);
    if (!strcmp(s, "driving")) return RGB(70, 150, 240);
    if (!strcmp(s, "updating")) return RGB(238, 170, 50);
    if (!strcmp(s, "asleep") || !strcmp(s, "offline") || !strcmp(s, "suspended")) return C_TEXT3;
    return C_TEXT2;
}

const char *tm_car_name(const TmCar *c) {
    const char *n = tm_get(c, "display_name");
    if (tm_has(n)) return n;
    const char *m = tm_get(c, "model");
    static char buf[32];
    if (tm_has(m)) { snprintf(buf, sizeof buf, "Model %s", m); return buf; }
    return "Tesla";
}

const char *tm_range(const TmCar *c) {
    const char *r = tm_get(c, "rated_battery_range_km");
    return tm_has(r) ? r : tm_get(c, "est_battery_range_km");
}

/* "MidnightSilver" -> "Midnight Silver", "Gemini19" -> "Gemini 19" */
static void split_words(char *out, size_t n, const char *in) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < n; i++) {
        bool brk = i > 0 && ((isupper((unsigned char)in[i]) && islower((unsigned char)in[i - 1])) ||
                             (isdigit((unsigned char)in[i]) && isalpha((unsigned char)in[i - 1])));
        if (brk) out[o++] = ' ';
        out[o++] = in[i] == '_' ? ' ' : in[i];
    }
    out[o] = 0;
}

/* the paint as a color for the drawing (Tesla's names, e.g. PearlWhite, DeepBlue, MidnightCherryRed) */
Color tm_paint(const TmCar *c) {
    char l[64];
    snprintf(l, sizeof l, "%s", tm_get(c, "exterior_color"));
    for (char *p = l; *p; p++) *p = tolower((unsigned char)*p);
    if (strstr(l, "white")) return RGB(232, 234, 236);
    if (strstr(l, "black") || strstr(l, "obsidian")) return RGB(34, 35, 40);
    if (strstr(l, "blue")) return RGB(36, 72, 150);
    if (strstr(l, "cherry")) return RGB(110, 18, 30);
    if (strstr(l, "red")) return RGB(178, 24, 32);
    if (strstr(l, "midnight") || strstr(l, "stealth") || strstr(l, "grey") || strstr(l, "gray") || strstr(l, "pewter"))
        return RGB(84, 88, 96);
    if (strstr(l, "silver")) return RGB(168, 172, 178);
    return RGB(120, 124, 132);
}

/* ---------- features (cfg tesla_f_<key>) ---------- */

static const struct { const char *key, *label; bool def; } feats[TF_COUNT] = {
    [TF_CHARGING] = {"charging", N_("feature|Charging"), true},
    [TF_CURRENT] = {"current", N_("feature|Charge current"), true},
    [TF_CLIMATE] = {"climate", N_("feature|Climate"), true},
    [TF_TYRES] = {"tyres", N_("feature|Tyres"), true},
    [TF_SECURITY] = {"security", N_("feature|Locks and doors"), true},
    [TF_TOPVIEW] = {"topview", N_("feature|Car from above"), true},
    [TF_WHERE] = {"where", N_("feature|Where"), true},
    [TF_DRIVE] = {"drive", N_("feature|Gear and direction"), true},
    [TF_ROUTE] = {"route", N_("feature|Route"), true},
    [TF_MAP] = {"map", N_("feature|Map"), false},
    [TF_CAR] = {"car", N_("feature|Model and software"), true},
    [TF_DISPLAY] = {"display", N_("feature|Dog mode etc."), true},
    [TF_USABLE] = {"usable", N_("feature|Usable battery"), true},
};

static char feat_cfg[TF_COUNT][32];
static const char *feat_key(int f) {
    if (!feat_cfg[f][0]) snprintf(feat_cfg[f], sizeof feat_cfg[f], "tesla_f_%s", feats[f].key);
    return feat_cfg[f];
}
bool tm_feat(int f) { return cfg_int(feat_key(f), feats[f].def) != 0; }
static void feat_toggle(int f) {
    cfg_set_int(feat_key(f), !tm_feat(f));
    if (f == TF_MAP && tm_feat(f)) app_toast(T("The map comes from OpenStreetMap: their servers see roughly where the car is"));
}
void tm_feat_forget(void) { for (int f = 0; f < TF_COUNT; f++) cfg_unset(feat_key(f)); }

/* ---------- the cars at the top of the home screen (cfg tesla_home: unset = the first car, "none" = none) ---------- */

static int home_ids(int *ids, int max) {
    const char *v = cfg_get("tesla_home", "");
    int n = 0;
    if (!*v) return -1; /* automatic */
    for (const char *p = v; *p && n < max;) {
        char *e;
        long id = strtol(p, &e, 10);
        if (e == p) { p++; continue; }
        if (id > 0) ids[n++] = (int)id;
        p = e;
    }
    return n;
}

int tm_home_cars(const TmCar *cars, int n, const TmCar **out) {
    int ids[TM_HOME_MAX], k = home_ids(ids, TM_HOME_MAX), m = 0;
    if (k < 0) { if (n > 0) out[m++] = &cars[0]; return m; }
    for (int i = 0; i < k; i++)
        for (int j = 0; j < n; j++)
            if (cars[j].id == ids[i] && m < TM_HOME_MAX) out[m++] = &cars[j];
    return m;
}

static bool home_has(const TmCar *cars, int n, int id) {
    const TmCar *h[TM_HOME_MAX];
    int m = tm_home_cars(cars, n, h);
    for (int i = 0; i < m; i++) if (h[i]->id == id) return true;
    return false;
}

static void home_toggle(const TmCar *cars, int n, int id) {
    const TmCar *h[TM_HOME_MAX];
    int m = tm_home_cars(cars, n, h), ids[TM_HOME_MAX], k = 0;
    bool had = false;
    for (int i = 0; i < m; i++) {
        if (h[i]->id == id) had = true;
        else ids[k++] = h[i]->id;
    }
    if (!had) {
        if (k >= TM_HOME_MAX) { app_toast(T("At most %d cars at the top of the home screen"), TM_HOME_MAX); return; }
        ids[k++] = id;
    }
    char v[64] = "";
    for (int i = 0; i < k; i++) { size_t l = strlen(v); snprintf(v + l, sizeof v - l, "%s%d", i ? "," : "", ids[i]); }
    cfg_set("tesla_home", k ? v : "none");
}

/* ---------- drawing helpers ----------
 * Everything in the cards is sized through Z(): the view picks the largest scale S (1 = designed size) at which all
 * cards fit on one screen (the UI is always 1920x1080; 720p and 4K displays scale the whole picture). */

static float S = 1;
#define Z(v) ((v) * S)
#define LH 44 /* one line in a card */

static void card(float x, float y, float w, float h, const char *title) {
    fill_rrect(x, y, w, h, Z(22), C_SURFACE);
    text_draw(F_SEMI, Z(21), x + Z(30), y + Z(24), C_TEXT3, AL_LEFT, title);
}

static float line(float x, float y, float w, const char *label, const char *val, Color vc) {
    float vw = fminf(text_width(F_SEMI, Z(26), val), w * 0.6f);
    text_draw_clip(F_SEMI, Z(26), x + w - Z(30), y, w * 0.6f, vc, AL_RIGHT, val);
    text_draw_clip(F_REG, Z(26), x + Z(30), y, w - Z(60) - vw - Z(16), C_TEXT2, AL_LEFT, label);
    return y + Z(LH);
}

/* bad: 1 = "yes" is the one to notice (a door open), -1 = "no" is (not locked), 0 = neither */
static float line_bool(float x, float y, float w, const char *label, const char *v, const char *yes, const char *no, int bad) {
    if (!tm_has(v)) return line(x, y, w, label, "-", C_TEXT3);
    bool t = tm_true(v);
    return line(x, y, w, label, t ? yes : no, (bad > 0 && t) || (bad < 0 && !t) ? WARN : C_TEXT);
}

void tm_battery_bar(float x, float y, float w, float h, float pct, float limit, bool charging, float a) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fill_rrect(x, y, w, h, h / 2, color_alpha(C_SURFACE2, a));
    Color c = pct <= 10 ? RGB(232, 72, 60) : pct <= 20 ? RGB(238, 170, 50) : charging ? RGB(64, 196, 110) : RGB(225, 228, 235);
    if (pct > 0) fill_rrect(x, y, fmaxf(h, w * pct / 100), h, h / 2, color_alpha(c, a));
    if (limit > 0 && limit < 100) fill_rect(x + w * limit / 100 - 2, y - h / 5, 4, h * 1.4f, color_alpha(C_TEXT2, a));
}

/* center_display_state (Tesla's numbers) -> text; NULL = nothing worth showing */
static const char *display_text(const char *v, bool *special) {
    *special = false;
    if (!tm_has(v)) return NULL;
    switch (atoi(v)) {
    case 0: return T("Screen off");
    case 2: case 4: return T("Screen on");
    case 3: case 5: return T("Charging screen");
    case 6: return T("Ready to unlock");
    case 7: *special = true; return T("Sentry mode is on");
    case 8: *special = true; return T("Dog mode is on");
    case 9: *special = true; return T("Camp mode is on");
    default: return NULL;
    }
}

/* active_route: {"destination":..,"minutes_to_arrival":..,"miles_to_arrival":..,"energy_at_arrival":..,
   "traffic_minutes_delay":..}; false when there is no route */
typedef struct { char dest[160]; double minutes, km, energy, delay; bool has_energy; } Route;
static bool route_of(const TmCar *c, Route *r) {
    memset(r, 0, sizeof *r);
    cJSON *j = cJSON_Parse(tm_get(c, "active_route"));
    cJSON *d = cJSON_GetObjectItem(j, "destination");
    bool ok = cJSON_IsString(d) && d->valuestring[0];
    if (ok) {
        snprintf(r->dest, sizeof r->dest, "%s", d->valuestring);
        cJSON *v;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "minutes_to_arrival"))) r->minutes = v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "miles_to_arrival"))) r->km = v->valuedouble * 1.609344;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "energy_at_arrival"))) { r->energy = v->valuedouble; r->has_energy = true; }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "traffic_minutes_delay"))) r->delay = v->valuedouble;
    } else if (tm_has(tm_get(c, "active_route_destination")) && !strchr(tm_get(c, "active_route"), '{')) {
        snprintf(r->dest, sizeof r->dest, "%s", tm_get(c, "active_route_destination")); /* older TeslaMate */
        ok = true;
    }
    cJSON_Delete(j);
    return ok;
}

/* ---------- what each card shows (also gives its height, in lines) ---------- */

static bool is_charging(const TmCar *c) { return !strcmp(tm_get(c, "state"), "charging"); }
static bool is_plugged(const TmCar *c) { return tm_true(tm_get(c, "plugged_in")) || is_charging(c); }
static bool has_request(const TmCar *c) { return tm_feat(TF_CURRENT) && tm_has(tm_get(c, "charge_current_request")); }
static const char *screen_text(const TmCar *c, bool *special) {
    *special = false;
    return tm_feat(TF_DISPLAY) ? display_text(tm_get(c, "center_display_state"), special) : NULL;
}

static int charging_lines(const TmCar *c) {
    int n = 0;
    if (is_plugged(c)) {
        n = 3 + is_charging(c) + tm_has(tm_get(c, "charger_actual_current")) + !is_charging(c); /* power, added, voltage (+ ...) */
    } else n = 1 + tm_has(tm_get(c, "scheduled_charging_start_time")); /* "Not plugged in" */
    return n + has_request(c) + 1; /* + charge port */
}

static int where_lines(const TmCar *c) {
    int n = 0;
    if (!strcmp(tm_get(c, "state"), "driving")) n += 2;
    if (tm_feat(TF_DRIVE)) n += tm_has(tm_get(c, "shift_state")) + tm_has(tm_get(c, "heading")) + tm_has(tm_get(c, "elevation"));
    return n;
}

static int car_lines(const TmCar *c) {
    const char *sp = tm_get(c, "spoiler_type"), *hl = tm_get(c, "healthy");
    return tm_has(tm_get(c, "model")) + tm_has(tm_get(c, "exterior_color")) + tm_has(tm_get(c, "wheel_type")) +
           (tm_has(sp) && strcmp(sp, "None")) + 2 + tm_true(tm_get(c, "update_available")) + (tm_has(hl) && !tm_true(hl));
}

static float lines_h(int n) { return 70 + n * LH + 14; } /* title + lines + bottom margin (designed size) */

/* ---------- the cards ---------- */

#define HERO_H 400

static void card_hero(float x, float y, float w, float h, const TmCar *c) {
    char b1[64], b2[64], b3[160];
    float pct = atof(tm_get(c, "battery_level")), limit = atof(tm_get(c, "charge_limit_soc"));
    const char *s = tm_get(c, "state");
    bool charging = is_charging(c);
    fill_rrect(x, y, w, h, Z(26), C_SURFACE);
    text_draw(F_SEMI, Z(21), x + Z(40), y + Z(28), C_TEXT3, AL_LEFT, T("BATTERY"));
    const char *bl = tm_get(c, "battery_level");
    float tw = text_draw(F_BOLD, Z(150), x + Z(36), y + Z(54), C_TEXT, AL_LEFT, tm_has(bl) ? tm_num(b1, sizeof b1, bl, 0) : "-");
    text_draw(F_SEMI, Z(60), x + Z(46) + tw, y + Z(122), C_TEXT2, AL_LEFT, "%");
    text_draw(F_SEMI, Z(44), x + w - Z(40), y + Z(86), C_TEXT, AL_RIGHT, tm_num_unit(b2, sizeof b2, tm_range(c), 0, "km"));
    text_draw(F_REG, Z(24), x + w - Z(40), y + Z(144), C_TEXT3, AL_RIGHT, T("range"));
    if (tm_feat(TF_USABLE)) {
        const char *u = tm_get(c, "usable_battery_level"), *ir = tm_get(c, "ideal_battery_range_km");
        b3[0] = 0;
        if (tm_has(u)) snprintf(b3, sizeof b3, T("Usable %s %%"), tm_num(b1, sizeof b1, u, 0));
        if (tm_has(ir)) {
            size_t l = strlen(b3);
            snprintf(b3 + l, sizeof b3 - l, "%s%s %s", l ? "  ·  " : "", T("ideal"), tm_num_unit(b2, sizeof b2, ir, 0, "km"));
        }
        if (b3[0]) text_draw(F_REG, Z(22), x + w - Z(40), y + Z(182), C_TEXT3, AL_RIGHT, b3);
    }
    tm_battery_bar(x + Z(40), y + Z(236), w - Z(80), Z(30), pct, limit, charging, 1);
    if (limit > 0) {
        snprintf(b3, sizeof b3, T("Charge limit %s %%"), tm_num(b1, sizeof b1, tm_get(c, "charge_limit_soc"), 0));
        text_draw(F_REG, Z(22), x + w - Z(40), y + Z(280), C_TEXT3, AL_RIGHT, b3);
    }
    Color sc = tm_state_color(c);
    float sy = y + h - Z(78);
    fill_circle(x + Z(52), sy + Z(19), Z(10), sc);
    char st[200];
    if (!strcmp(s, "driving") && tm_has(tm_get(c, "speed")))
        snprintf(st, sizeof st, "%s · %s", tm_state_text(c), tm_num_unit(b2, sizeof b2, tm_get(c, "speed"), 0, "km/h"));
    else if (charging && tm_has(tm_get(c, "charger_power")))
        snprintf(st, sizeof st, "%s · %s", tm_state_text(c), tm_num_unit(b2, sizeof b2, tm_get(c, "charger_power"), 0, "kW"));
    else snprintf(st, sizeof st, "%s", tm_state_text(c));
    float sw = text_draw(F_SEMI, Z(32), x + Z(76), sy, sc.r == C_TEXT3.r ? C_TEXT2 : sc, AL_LEFT, st);
    const char *since = tm_since(b1, sizeof b1, tm_get(c, "since"));
    float bx = x + Z(92) + sw;
    if (*since) {
        snprintf(b3, sizeof b3, T("since %s"), since);
        bx += text_draw(F_REG, Z(26), bx, sy + Z(5), C_TEXT3, AL_LEFT, b3) + Z(24);
    }
    bool special;
    const char *ds = screen_text(c, &special);
    if (ds && special) { /* Dog/Camp/Sentry: a small badge */
        float bw = text_width(F_SEMI, Z(22), ds) + Z(32);
        if (bx + bw > x + w - Z(30)) bx = x + w - Z(30) - bw;
        fill_rrect(bx, sy - Z(2), bw, Z(40), Z(20), RGBA(238, 170, 50, 60));
        text_draw(F_SEMI, Z(22), bx + Z(16), sy + Z(6), RGB(250, 200, 110), AL_LEFT, ds);
    }
}

#define ROUTE_H 210

static void card_route(float x, float y, float w, float h, const TmCar *c) {
    Route r;
    char b1[48], b2[64], t[200];
    if (!route_of(c, &r)) return;
    card(x, y, w, h, T("ON THE WAY"));
    text_draw_clip(F_BOLD, Z(40), x + Z(30), y + Z(58), w - Z(60), C_TEXT, AL_LEFT, r.dest);
    float cx = x + Z(30), cy = y + Z(114);
    if (r.minutes > 0) {
        time_t eta = time(NULL) + (time_t)(r.minutes * 60);
        struct tm lt;
        localtime_r(&eta, &lt);
        char clk[16];
        strftime(clk, sizeof clk, "%H:%M", &lt);
        snprintf(t, sizeof t, T("Arrives %s"), clk);
        cx += text_draw(F_SEMI, Z(32), cx, cy, RGB(70, 150, 240), AL_LEFT, t) + Z(30);
        snprintf(t, sizeof t, T("in %s"), minutes_text(b1, sizeof b1, (int)lround(r.minutes)));
        text_draw(F_REG, Z(28), cx, cy + Z(3), C_TEXT2, AL_LEFT, t);
    }
    cx = x + Z(30);
    cy += Z(48);
    if (r.km > 0) {
        snprintf(t, sizeof t, "%s km", numf(b2, sizeof b2, r.km, r.km < 10 ? 1 : 0));
        cx += text_draw(F_REG, Z(28), cx, cy, C_TEXT2, AL_LEFT, t) + Z(30);
    }
    if (r.has_energy) {
        snprintf(t, sizeof t, T("%s %% at arrival"), numf(b1, sizeof b1, r.energy, 0));
        cx += text_draw(F_REG, Z(28), cx, cy, r.energy < 15 ? WARN : C_TEXT2, AL_LEFT, t) + Z(30);
    }
    if (r.delay >= 1) {
        snprintf(t, sizeof t, T("+%s traffic"), minutes_text(b1, sizeof b1, (int)lround(r.delay)));
        text_draw(F_REG, Z(28), cx, cy, WARN, AL_LEFT, t);
    }
}

static void card_charging(float x, float y, float w, float h, const TmCar *c) {
    char b1[48], b2[64];
    bool charging = is_charging(c);
    card(x, y, w, h, T("CHARGING"));
    float ly = y + Z(70);
    if (is_plugged(c)) {
        ly = line(x, ly, w, tm_ctx(T("charging|Power")), tm_num_unit(b1, sizeof b1, tm_get(c, "charger_power"), 0, "kW"), C_TEXT);
        ly = line(x, ly, w, T("Added"), tm_num_unit(b1, sizeof b1, tm_get(c, "charge_energy_added"), 1, "kWh"), C_TEXT);
        if (charging) ly = line(x, ly, w, T("Full in"), hours_text(b1, sizeof b1, tm_get(c, "time_to_full_charge")), C_TEXT);
        const char *a = tm_get(c, "charger_actual_current"), *ph = tm_get(c, "charger_phases");
        if (tm_has(a)) {
            if (tm_has(ph) && atoi(ph) > 1) snprintf(b2, sizeof b2, "%s A × %d", tm_num(b1, sizeof b1, a, 0), atoi(ph));
            else snprintf(b2, sizeof b2, "%s A", tm_num(b1, sizeof b1, a, 0));
            ly = line(x, ly, w, T("Current"), b2, C_TEXT);
        }
        ly = line(x, ly, w, T("Voltage"), tm_num_unit(b1, sizeof b1, tm_get(c, "charger_voltage"), 0, "V"), C_TEXT);
        if (!charging) {
            text_draw_clip(F_SEMI, Z(26), x + Z(30), ly, w - Z(60), C_TEXT2, AL_LEFT, T("Plugged in, not charging"));
            ly += Z(LH);
        }
    } else {
        text_draw(F_SEMI, Z(28), x + Z(30), ly, C_TEXT2, AL_LEFT, T("Not plugged in"));
        ly += Z(LH);
        const char *sch = tm_get(c, "scheduled_charging_start_time");
        if (tm_has(sch)) ly = line(x, ly, w, T("Scheduled start"), tm_since(b1, sizeof b1, sch), C_TEXT);
    }
    if (has_request(c)) {
        const char *rq = tm_get(c, "charge_current_request"), *rqm = tm_get(c, "charge_current_request_max");
        char m[24];
        if (tm_has(rqm)) snprintf(b2, sizeof b2, "%s / %s A", tm_num(b1, sizeof b1, rq, 0), tm_num(m, sizeof m, rqm, 0));
        else snprintf(b2, sizeof b2, "%s A", tm_num(b1, sizeof b1, rq, 0));
        ly = line(x, ly, w, tm_ctx(T("charging|Requested")), b2, C_TEXT);
    }
    line_bool(x, ly, w, T("Charge port"), tm_get(c, "charge_port_door_open"), OPEN, T("Closed"), 0);
}

static void card_climate(float x, float y, float w, float h, const TmCar *c) {
    char b1[48];
    card(x, y, w, h, T("CLIMATE"));
    float ly = y + Z(70);
    ly = line(x, ly, w, T("Inside"), tm_num_unit(b1, sizeof b1, tm_get(c, "inside_temp"), 1, "°C"), C_TEXT);
    ly = line(x, ly, w, T("Outside"), tm_num_unit(b1, sizeof b1, tm_get(c, "outside_temp"), 1, "°C"), C_TEXT);
    const char *on = tm_get(c, "is_climate_on");
    line(x, ly, w, T("Climate"), tm_true(tm_get(c, "is_preconditioning")) ? T("Preconditioning") : !tm_has(on) ? "-" :
         tm_true(on) ? T("On") : T("Off"), C_TEXT);
}

static const char *const tyre_warn[4] = {"tpms_soft_warning_fl", "tpms_soft_warning_fr", "tpms_soft_warning_rl", "tpms_soft_warning_rr"};
static bool tyre_warning(const TmCar *c) {
    for (int i = 0; i < 4; i++) if (tm_true(tm_get(c, tyre_warn[i]))) return true;
    return false;
}

static void card_tyres(float x, float y, float w, float h, const TmCar *c) {
    static const char *const pk[4] = {"tpms_pressure_fl", "tpms_pressure_fr", "tpms_pressure_rl", "tpms_pressure_rr"};
    static const char *const tl[4] = {N_("Front left"), N_("Front right"), N_("Rear left"), N_("Rear right")};
    char b1[32];
    card(x, y, w, h, T("TYRES (BAR)"));
    for (int i = 0; i < 4; i++) {
        bool warn = tm_true(tm_get(c, tyre_warn[i]));
        float tx = x + Z(30) + (i % 2) * (w / 2 - Z(10)), ty = y + Z(58) + (i / 2) * Z(70);
        text_draw(F_REG, Z(20), tx, ty, warn ? WARN : C_TEXT3, AL_LEFT, T(tl[i]));
        text_draw(F_SEMI, Z(30), tx, ty + Z(24), warn ? WARN : C_TEXT, AL_LEFT, tm_num(b1, sizeof b1, tm_get(c, pk[i]), 1));
    }
    if (tyre_warning(c)) text_draw_clip(F_SEMI, Z(22), x + Z(30), y + Z(204), w - Z(60), WARN, AL_LEFT, T("Tyre pressure warning"));
}

static void card_security(float x, float y, float w, float h, const TmCar *c) {
    card(x, y, w, h, T("SECURITY"));
    float ly = y + Z(70);
    ly = line_bool(x, ly, w, T("Locked"), tm_get(c, "locked"), T("Yes"), T("No"), -1);
    ly = line_bool(x, ly, w, T("Sentry mode"), tm_get(c, "sentry_mode"), T("On"), T("Off"), 0);
    ly = line_bool(x, ly, w, T("Doors"), tm_get(c, "doors_open"), OPEN, T("Closed"), 1);
    ly = line_bool(x, ly, w, T("Windows"), tm_get(c, "windows_open"), OPEN, T("Closed"), 1);
    ly = line_bool(x, ly, w, T("Trunk"), tm_get(c, "trunk_open"), OPEN, T("Closed"), 1);
    ly = line_bool(x, ly, w, T("Frunk"), tm_get(c, "frunk_open"), OPEN, T("Closed"), 1);
    ly = line_bool(x, ly, w, T("Someone in the car"), tm_get(c, "is_user_present"), T("Yes"), T("No"), 0);
    bool special;
    const char *ds = screen_text(c, &special);
    if (ds) line(x, ly, w, T("Screen"), ds, special ? RGB(250, 200, 110) : C_TEXT);
}

#define TOPVIEW_H 380

/* the car from above in its own paint: open doors, trunk, frunk, windows and the charge port in orange */
static void card_topview(float x, float y, float w, float h, const TmCar *c) {
    card(x, y, w, h, T("THE CAR FROM ABOVE"));
    Color paint = tm_paint(c), edge = color_mix(paint, RGB(255, 255, 255), 0.25f), glass = RGB(20, 24, 32);
    float bw = Z(104), bh = Z(226), cx = x + w / 2, top = y + Z(78), bx = cx - bw / 2;
    bool fl = tm_true(tm_get(c, "driver_front_door_open")), rl = tm_true(tm_get(c, "driver_rear_door_open")),
         fr = tm_true(tm_get(c, "passenger_front_door_open")), rr = tm_true(tm_get(c, "passenger_rear_door_open"));
    bool trunk = tm_true(tm_get(c, "trunk_open")), frunk = tm_true(tm_get(c, "frunk_open")),
         win = tm_true(tm_get(c, "windows_open")), port = tm_true(tm_get(c, "charge_port_door_open"));
    bool any_door = tm_true(tm_get(c, "doors_open")) && !(fl || rl || fr || rr); /* older TeslaMate: only "a door" */
    Color tyre = RGB(14, 14, 16);
    float ww = Z(14), wh = Z(38);
    fill_rrect(bx - Z(9), top + Z(34), ww, wh, Z(5), tyre);
    fill_rrect(bx + bw - Z(5), top + Z(34), ww, wh, Z(5), tyre);
    fill_rrect(bx - Z(9), top + bh - Z(72), ww, wh, Z(5), tyre);
    fill_rrect(bx + bw - Z(5), top + bh - Z(72), ww, wh, Z(5), tyre);
    fill_rrect(bx, top, bw, bh, Z(40), paint);
    stroke_rrect(bx, top, bw, bh, Z(40), 2, edge);
    Color gl = win ? color_mix(glass, WARN, 0.45f) : glass;
    fill_rrect(bx + Z(14), top + Z(60), bw - Z(28), Z(114), Z(19), gl);
    fill_rect(bx + Z(14), top + Z(110), bw - Z(28), Z(3), color_alpha(paint, 0.9f)); /* the line between the doors */
    if (frunk) fill_rrect(bx + Z(16), top - Z(24), bw - Z(32), Z(33), Z(9), WARN);
    if (trunk) fill_rrect(bx + Z(16), top + bh - Z(9), bw - Z(32), Z(33), Z(9), WARN);
    float dy[2] = {top + Z(66), top + Z(116)};
    bool left[2] = {fl, rl}, right[2] = {fr, rr};
    for (int i = 0; i < 2; i++) {
        if (left[i] || (any_door && i == 0)) fill_rrect(bx - Z(30), dy[i], Z(26), Z(43), Z(5), WARN);
        if (right[i]) fill_rrect(bx + bw + Z(4), dy[i], Z(26), Z(43), Z(5), WARN);
    }
    if (port) fill_circle(bx + Z(5), top + bh - Z(40), Z(7), WARN); /* rear left */
    char t[200] = "";
    const char *parts[8];
    int np = 0;
    if (fl || any_door) parts[np++] = any_door ? T("A door") : T("Driver's door");
    if (rl) parts[np++] = T("Rear left door");
    if (fr) parts[np++] = T("Passenger door");
    if (rr) parts[np++] = T("Rear right door");
    if (frunk) parts[np++] = T("Frunk");
    if (trunk) parts[np++] = T("Trunk");
    if (win) parts[np++] = T("Windows");
    if (port) parts[np++] = T("Charge port");
    for (int i = 0; i < np; i++) { size_t l = strlen(t); snprintf(t + l, sizeof t - l, "%s%s", i ? ", " : "", parts[i]); }
    float ty = y + h - Z(44);
    if (np) {
        char m[240];
        snprintf(m, sizeof m, T("Open: %s"), t);
        text_draw_clip(F_SEMI, Z(22), cx, ty, w - Z(40), WARN, AL_CENTER, m);
    } else if (tm_has(tm_get(c, "doors_open"))) text_draw(F_REG, Z(22), cx, ty, C_TEXT3, AL_CENTER, T("Everything is closed"));
}

static const char *compass(double deg) {
    static const char *const d[8] = {N_("compass|N"), N_("compass|NE"), N_("compass|E"), N_("compass|SE"),
                                     N_("compass|S"), N_("compass|SW"), N_("compass|W"), N_("compass|NW")};
    int i = (int)lround(fmod(fmod(deg, 360) + 360, 360) / 45) % 8;
    return tm_ctx(T(d[i]));
}

static void card_where(float x, float y, float w, float h, const TmCar *c) {
    char b1[48], b3[160];
    card(x, y, w, h, T("WHERE"));
    float ly = y + Z(66);
    const char *geo = tm_get(c, "geofence");
    text_draw_clip(F_SEMI, Z(32), x + Z(30), ly, w - Z(60), C_TEXT, AL_LEFT, tm_has(geo) ? geo : T("No saved place"));
    ly += Z(46);
    const char *lat = tm_get(c, "latitude"), *lon = tm_get(c, "longitude");
    if (tm_has(lat) && tm_has(lon)) {
        snprintf(b3, sizeof b3, "%.5f, %.5f", atof(lat), atof(lon));
        text_draw(F_REG, Z(22), x + Z(30), ly, C_TEXT3, AL_LEFT, b3);
    }
    ly += Z(44);
    if (!strcmp(tm_get(c, "state"), "driving")) {
        ly = line(x, ly, w, T("Speed"), tm_num_unit(b1, sizeof b1, tm_get(c, "speed"), 0, "km/h"), C_TEXT);
        ly = line(x, ly, w, tm_ctx(T("driving|Power")), tm_num_unit(b1, sizeof b1, tm_get(c, "power"), 0, "kW"), C_TEXT);
    }
    if (tm_feat(TF_DRIVE)) {
        const char *g = tm_get(c, "shift_state");
        if (tm_has(g)) ly = line(x, ly, w, T("Gear"), g, C_TEXT);
        const char *hd = tm_get(c, "heading");
        if (tm_has(hd)) {
            snprintf(b3, sizeof b3, "%s (%d°)", compass(atof(hd)), atoi(hd));
            ly = line(x, ly, w, T("Direction"), b3, C_TEXT);
        }
        const char *el = tm_get(c, "elevation");
        if (tm_has(el)) line(x, ly, w, T("Altitude"), tm_num_unit(b1, sizeof b1, el, 0, "m"), C_TEXT);
    }
}

#define MAP_H 320

/* OpenStreetMap tiles around the car (zoom 15), only when the feature is on. The tiles keep their size: a smaller
   card just shows less of the map */
static void card_map(float x, float y, float w, float h, const TmCar *c) {
    double lat = atof(tm_get(c, "latitude")), lon = atof(tm_get(c, "longitude"));
    fill_rrect(x, y, w, h, Z(22), C_SURFACE);
    const int z = 15;
    double n = 1 << z, xt = (lon + 180) / 360 * n, lr = lat * M_PI / 180;
    double yt = (1 - asinh(tan(lr)) / M_PI) / 2 * n;
    float cx = x + w / 2, cy = y + h / 2;
    clip_push(x, y, w, h);
    int tx0 = (int)floor(xt - (w / 2) / 256.0), tx1 = (int)floor(xt + (w / 2) / 256.0);
    int ty0 = (int)floor(yt - (h / 2) / 256.0), ty1 = (int)floor(yt + (h / 2) / 256.0);
    bool all = true;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            if (ty < 0 || ty >= (int)n) continue;
            int wx = ((tx % (int)n) + (int)n) % (int)n;
            char url[128];
            snprintf(url, sizeof url, "https://tile.openstreetmap.org/%d/%d/%d.png", z, wx, ty);
            float px = cx + (float)((tx - xt) * 256), py = cy + (float)((ty - yt) * 256);
            all &= img_draw(img_get(url), px, py, 256, 256, 0, 1);
        }
    if (!all) draw_spinner(x + 40, y + 40, 14, C_TEXT2);
    fill_circle(cx, cy, 16, RGB(255, 255, 255));
    fill_circle(cx, cy, 11, RGB(214, 32, 40));
    fill_rrect(x + w - 236, y + h - 34, 226, 26, 8, RGBA(255, 255, 255, 200));
    text_draw(F_REG, 16, x + w - 123, y + h - 30, RGB(40, 40, 40), AL_CENTER, "© OpenStreetMap");
    clip_pop();
}

static void card_car(float x, float y, float w, float h, const TmCar *c) {
    char b1[48], b3[160];
    card(x, y, w, h, T("THE CAR"));
    float ly = y + Z(70);
    const char *model = tm_get(c, "model"), *trim = tm_get(c, "trim_badging");
    if (tm_has(model)) {
        if (tm_has(trim)) snprintf(b3, sizeof b3, "Model %s · %s", model, trim);
        else snprintf(b3, sizeof b3, "Model %s", model);
        ly = line(x, ly, w, T("Model"), b3, C_TEXT);
    }
    const char *col = tm_get(c, "exterior_color");
    if (tm_has(col)) { split_words(b3, sizeof b3, col); ly = line(x, ly, w, T("Colour"), b3, C_TEXT); }
    const char *wh = tm_get(c, "wheel_type");
    if (tm_has(wh)) { split_words(b3, sizeof b3, wh); ly = line(x, ly, w, T("Wheels"), b3, C_TEXT); }
    const char *sp = tm_get(c, "spoiler_type");
    if (tm_has(sp) && strcmp(sp, "None")) { split_words(b3, sizeof b3, sp); ly = line(x, ly, w, T("Spoiler"), b3, C_TEXT); }
    ly = line(x, ly, w, T("Odometer"), tm_num_unit(b1, sizeof b1, tm_get(c, "odometer"), 0, "km"), C_TEXT);
    const char *ver = tm_get(c, "version");
    ly = line(x, ly, w, T("Software"), tm_has(ver) ? ver : "-", C_TEXT);
    if (tm_true(tm_get(c, "update_available"))) {
        const char *uv = tm_get(c, "update_version");
        ly = line(x, ly, w, T("Update ready"), tm_has(uv) ? uv : T("Yes"), GOOD);
    }
    const char *hl = tm_get(c, "healthy");
    if (tm_has(hl) && !tm_true(hl)) line(x, ly, w, T("Health"), T("TeslaMate reports a problem"), WARN);
}

/* ---------- layout: cards in four columns, each placed where the columns are lowest ---------- */

#define COLS 4
#define GAP 24
#define TOP 170
#define BOTTOM 964
#define LEFT (SIDE_W + 48)
#define RIGHT (UI_W - 96)
#define MIN_SCALE 0.62f /* below this the text gets too small on a TV: scroll instead */

typedef void (*CardFn)(float, float, float, float, const TmCar *);
typedef struct { CardFn fn; int span; float h; } Card; /* h at the designed size (S = 1) */

static int cards_of(const TmCar *c, Card *out) {
    int n = 0;
    Route r;
    bool special;
    bool loc = tm_has(tm_get(c, "latitude")) && tm_has(tm_get(c, "longitude"));
    out[n++] = (Card){card_hero, 2, HERO_H};
    if (tm_feat(TF_ROUTE) && route_of(c, &r)) out[n++] = (Card){card_route, 2, ROUTE_H};
    if (tm_feat(TF_CHARGING)) out[n++] = (Card){card_charging, 1, lines_h(charging_lines(c))};
    if (tm_feat(TF_CLIMATE)) out[n++] = (Card){card_climate, 1, lines_h(3)};
    if (tm_feat(TF_TYRES)) out[n++] = (Card){card_tyres, 1, tyre_warning(c) ? 250 : 214};
    if (tm_feat(TF_SECURITY)) out[n++] = (Card){card_security, 1, lines_h(7 + (screen_text(c, &special) != NULL))};
    if (tm_feat(TF_TOPVIEW)) out[n++] = (Card){card_topview, 1, TOPVIEW_H};
    if (tm_feat(TF_WHERE)) out[n++] = (Card){card_where, 1, 66 + 46 + 44 + where_lines(c) * LH + 14};
    if (tm_feat(TF_MAP) && loc) out[n++] = (Card){card_map, 2, MAP_H};
    if (tm_feat(TF_CAR)) out[n++] = (Card){card_car, 1, lines_h(car_lines(c))};
    return n;
}

/* places the cards at scale S; draws them (when draw) and returns the height of the content */
static float layout(const TmCar *c, float scroll, bool draw) {
    Card cs[16];
    int n = cards_of(c, cs);
    float gap = Z(GAP), cw = (RIGHT - LEFT - (COLS - 1) * gap) / (float)COLS, col[COLS];
    for (int i = 0; i < COLS; i++) col[i] = TOP;
    float bottom = TOP;
    for (int i = 0; i < n; i++) {
        int best = 0;
        float by = 1e9f, h = Z(cs[i].h);
        for (int s = 0; s + cs[i].span <= COLS; s++) {
            float m = 0;
            for (int k = s; k < s + cs[i].span; k++) m = fmaxf(m, col[k]);
            if (m < by - 0.5f) { by = m; best = s; }
        }
        float x = LEFT + best * (cw + gap), w = cs[i].span * cw + (cs[i].span - 1) * gap;
        if (draw && by - scroll + h > 140 && by - scroll < BOTTOM + 20) cs[i].fn(x, by - scroll, w, h, c);
        for (int k = best; k < best + cs[i].span; k++) col[k] = by + h + gap;
        bottom = fmaxf(bottom, by + h);
    }
    return bottom - TOP;
}

/* the largest scale at which everything fits (or MIN_SCALE, and then the view scrolls) */
static float fit_scale(const TmCar *c) {
    float best = MIN_SCALE;
    for (float s = 1.0f; s >= MIN_SCALE; s -= 0.02f) {
        S = s;
        if (layout(c, 0, false) <= BOTTOM - TOP) { best = s; break; }
    }
    S = best;
    return best;
}

/* ---------- the side menu ---------- */

enum { P_MAIN, P_FEATS, P_HOME };

static TmCar vbuf[TM_MAX_CARS]; /* the screen's copy (main thread only) */

static void side_build(TView *v, const TmCar *cars, int n) {
    side_clear(&v->side);
    v->side.active = -1;
    if (v->page == P_MAIN) {
        for (int i = 0; i < n; i++) {
            int k = side_add(&v->side, tm_car_name(&cars[i]), ICON_POWER, false);
            if (i == v->car) v->side.active = k;
        }
        if (!n) side_add(&v->side, "Tesla", ICON_POWER, false);
        v->it_feats = side_add(&v->side, T("What is shown"), ICON_GEAR, true);
        v->it_home = side_add(&v->side, T("On the home screen"), ICON_TV, false);
        v->it_server = side_add(&v->side, T("Change server"), ICON_SEARCH, false);
    } else if (v->page == P_FEATS) {
        side_add(&v->side, T("Back"), ICON_BACK, false);
        for (int f = 0; f < TF_COUNT; f++) side_add(&v->side, tm_ctx(T(feats[f].label)), tm_feat(f) ? ICON_CHECK : NO_ICON, f == 0);
    } else {
        side_add(&v->side, T("Back"), ICON_BACK, false);
        for (int i = 0; i < n; i++)
            side_add(&v->side, tm_car_name(&cars[i]), home_has(cars, n, cars[i].id) ? ICON_CHECK : NO_ICON, i == 0);
        if (!n) side_add(&v->side, T("No cars yet"), NO_ICON, true);
    }
    snprintf(v->side.subtitle, sizeof v->side.subtitle, "%s",
             v->page == P_FEATS ? T("OK turns it on or off") : v->page == P_HOME ? T("Up to 3 cars at the top") : "");
}

void view_init(TView *v) {
    side_init(&v->side, "Tesla", ICON_POWER, RGB(214, 32, 40));
    v->page = P_MAIN;
    v->car = 0;
    v->scroll = v->scroll_to = 0;
    v->opened = app_time();
}

void view_update(TView *v, float dt) {
    int n = tm_cars(vbuf, TM_MAX_CARS);
    if (!v->side.open && v->side.t < 0.05f && v->page != P_MAIN) v->page = P_MAIN;
    int keep = v->side.sel;
    side_build(v, vbuf, n);
    v->side.sel = keep;
    side_update(&v->side, dt);
    v->scroll = anim_to(v->scroll, v->scroll_to, dt, 14);
}

static void draw_status(TmState st, bool known) {
    char t[300];
    const char *s = "";
    switch (st) {
    case TM_LIVE: s = T("Live from TeslaMate"); break;
    case TM_CONNECTING: s = T("Connecting to TeslaMate…"); break;
    case TM_AUTH: s = T("The server refused the username or password"); break;
    case TM_UNREACHABLE: s = known ? T("Can't reach the server - showing what was last known") : T("Can't reach the server"); break;
    default: break;
    }
    snprintf(t, sizeof t, "%s  ·  %s", s, cfg_get("tesla_host", ""));
    Color c = st == TM_LIVE ? RGB(64, 196, 110) : st == TM_CONNECTING ? C_TEXT3 : RGB(238, 150, 60);
    fill_circle(LEFT + 9, 131, 7, c);
    text_draw(F_REG, 24, LEFT + 26, 118, C_TEXT3, AL_LEFT, t);
}

void view_draw(TView *v) {
    int n = tm_cars(vbuf, TM_MAX_CARS);
    TmState st = tm_state();
    if (v->car >= n) v->car = n > 0 ? n - 1 : 0;
    const TmCar *c = n ? &vbuf[v->car] : NULL;
    text_draw(F_BOLD, 44, LEFT, 56, C_TEXT, AL_LEFT, c ? tm_car_name(c) : "Tesla");
    draw_topbar(NULL);
    draw_status(st, n > 0);
    if (c) {
        fit_scale(c);
        v->content_h = layout(c, 0, false);
        if (v->content_h <= BOTTOM - TOP) v->scroll = v->scroll_to = 0;
        clip_push(0, 150, UI_W, BOTTOM + 10 - 150);
        layout(c, v->scroll, true);
        clip_pop();
        float hx = LEFT;
        if (v->content_h > BOTTOM - TOP) { draw_hint(hx, 990, "▲ ▼", T("Scroll")); hx += 230; }
        draw_hint(hx, 990, "◀", T("Menu: car, what is shown, home screen"));
    } else {
        if (st == TM_AUTH)
            draw_empty(T("Login refused"), T("The MQTT server did not accept the username or password. Press Red to choose the server again."));
        else if (st == TM_UNREACHABLE)
            draw_empty(T("Can't reach the server"), T("Check that the machine running TeslaMate is on and that its MQTT port (1883) can be reached from the network. NotaTV keeps trying."));
        else if (st == TM_LIVE && app_time() - v->opened > 5)
            draw_empty(T("No data from TeslaMate"), T("Connected, but TeslaMate has sent nothing yet. Check that MQTT is turned on in TeslaMate (it is by default)."));
        else draw_spinner(UI_W / 2, 480, 30, C_TEXT2);
        draw_hint(LEFT, 990, "Red", T("Menu"));
    }
    if (!c) v->content_h = 0;
    side_draw(&v->side);
}

static void clamp_scroll(TView *v) {
    float max = v->content_h - (BOTTOM - TOP);
    if (v->scroll_to > max) v->scroll_to = max;
    if (v->scroll_to < 0) v->scroll_to = 0;
}

/* returns VIEW_SERVER when the user wants another server, VIEW_CLOSE on Back, else 0 */
int view_input(TView *v, const InputEvent *ev) {
    int n = tm_cars(vbuf, TM_MAX_CARS);
    if (v->side.open) {
        if (ev->a == A_BACK && v->page != P_MAIN) { v->page = P_MAIN; v->side.sel = 0; return 0; }
        int act;
        if (!side_input(&v->side, ev, &act)) return ev->a == A_HOME ? -1 : 0;
        if (act < 0) return 0;
        if (v->page == P_MAIN) {
            if (act == v->it_feats || act == v->it_home) {
                v->page = act == v->it_feats ? P_FEATS : P_HOME;
                side_build(v, vbuf, n);
                v->side.open = true;
                v->side.sel = 1;
            } else if (act == v->it_server) return VIEW_SERVER;
            else if (act < n) { v->car = act; v->scroll_to = 0; }
        } else {
            side_build(v, vbuf, n);
            v->side.open = true; /* stays open: turn several on or off */
            v->side.sel = act;
            if (act == 0) { v->page = P_MAIN; v->side.sel = 0; }
            else if (v->page == P_FEATS && act - 1 < TF_COUNT) feat_toggle(act - 1);
            else if (v->page == P_HOME && act - 1 < n) home_toggle(vbuf, n, vbuf[act - 1].id);
        }
        return 0;
    }
    switch (ev->a) {
    case A_UP: v->scroll_to -= 260; clamp_scroll(v); return 0;
    case A_DOWN: v->scroll_to += 260; clamp_scroll(v); return 0;
    case A_LEFT: case A_MENU:
        v->page = P_MAIN;
        side_build(v, vbuf, n);
        side_open(&v->side);
        return 0;
    case A_BACK: return VIEW_CLOSE;
    case A_HOME: return -1;
    default: return 0;
    }
}

/* ---------- the top of the home screen: up to 3 small cars ---------- */

static TmCar hbuf[TM_MAX_CARS];

void tm_home_overlay(void) {
    Screen *t = app_top();
    if (!t || !t->name || strcmp(t->name, "home")) return;
    int n = tm_cars(hbuf, TM_MAX_CARS);
    const TmCar *cs[TM_HOME_MAX];
    int m = tm_home_cars(hbuf, n, cs);
    if (!m) return;
    float a = tm_state() == TM_LIVE ? 1 : 0.6f;
    char l1[TM_HOME_MAX][64], l2[TM_HOME_MAX][120], b1[24], b2[32];
    float wd[TM_HOME_MAX], total = 0;
    for (int i = 0; i < m; i++) {
        snprintf(l1[i], sizeof l1[i], "%s %%  ·  %s", tm_num(b1, sizeof b1, tm_get(cs[i], "battery_level"), 0),
                 tm_num_unit(b2, sizeof b2, tm_range(cs[i]), 0, "km"));
        snprintf(l2[i], sizeof l2[i], "%s", tm_car_name(cs[i]));
        float w1 = text_width(F_SEMI, 20, l1[i]),
              w2 = text_width(F_REG, 16, l2[i]) + text_width(F_REG, 16, tm_state_text(cs[i])) + 24;
        float tw = w1 > w2 ? w1 : w2;
        if (tw > 230) tw = 230;
        wd[i] = 16 + 30 + 12 + tw + 18;
        total += wd[i] + (i ? 12 : 0);
    }
    /* in the top bar between the date and the network icon / key hint */
    float right = UI_W - 310 - 36 - text_width(F_REG, 22, T("Move app")) - 28, y = 50, h = 58;
    float x = app_overlay_place ? app_overlay_place(right, total) : right - total; /* not on top of Music's "Now playing" */
    for (int i = 0; i < m; i++) {
        const TmCar *c = cs[i];
        fill_rrect(x, y, wd[i], h, h / 2, RGBA(18, 20, 28, (uint8_t)(205 * a)));
        stroke_rrect(x, y, wd[i], h, h / 2, 1, RGBA(255, 255, 255, (uint8_t)(22 * a)));
        /* a small battery */
        float bx = x + 16, by = y + 15, pct = atof(tm_get(c, "battery_level"));
        bool chg = !strcmp(tm_get(c, "state"), "charging");
        stroke_rrect(bx, by, 26, 14, 3, 1.5f, color_alpha(C_TEXT2, a));
        fill_rect(bx + 26, by + 4, 3, 6, color_alpha(C_TEXT2, a));
        Color fc = pct <= 10 ? RGB(232, 72, 60) : pct <= 20 ? RGB(238, 170, 50) : chg ? RGB(64, 196, 110) : RGB(225, 228, 235);
        if (pct > 0) fill_rrect(bx + 2.5f, by + 2.5f, fmaxf(2, 21 * fminf(pct, 100) / 100), 9, 1.5f, color_alpha(fc, a));
        float tx = bx + 30 + 12;
        text_draw_clip(F_SEMI, 20, tx, y + 8, 230, color_alpha(C_TEXT, a), AL_LEFT, l1[i]);
        float nw = text_draw_clip(F_REG, 16, tx, y + 33, 150, color_alpha(C_TEXT3, a), AL_LEFT, l2[i]);
        fill_circle(tx + nw + 10, y + 42, 3.5f, color_alpha(tm_state_color(c), a));
        Color sc = tm_state_color(c);
        text_draw_clip(F_REG, 16, tx + nw + 19, y + 33, 230 - nw - 19, color_alpha(sc.r == C_TEXT3.r ? C_TEXT3 : sc, a), AL_LEFT,
                       tm_state_text(c));
        x += wd[i] + 12;
    }
}
