/* NotaTV app package "weather" - the weather now, the next 24 hours and 7 days from Open-Meteo (open data, no key or
 * account). The town comes from the box's time zone until one is chosen (Yellow / Settings → Apps → Weather). The
 * temperature is also shown in the home screen's top bar, and the home screen shows the weather when the app has focus.
 * The icons are drawn here (no image files). Built as app.so (see src/apps/README.md). */
#include "tv.h"
#include "vendor/cJSON.h"
#include "json_util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WX_ACCENT RGB(64, 156, 255)
#define REFRESH_S (20 * 60)
#define MAX_HOURS 48
#define MAX_DAYS 7

/* ---------- data ---------- */
typedef struct { char t[20]; float temp; int code, rain; bool day; } WxHour;
typedef struct { char d[12]; int code; float tmax, tmin, rain; } WxDay;
typedef struct {
    bool ok;
    float temp, feels, wind;
    int code;
    bool day;
    char time[20];
    WxHour h[MAX_HOURS];
    int nh;
    WxDay d[MAX_DAYS];
    int nd;
} Wx;
static Wx W;
static double next_fetch, fetched_at;
static bool fetching;
static int wx_gen; /* increased when the town changes: an answer for the old town is dropped */

static double jarr(cJSON *o, const char *k, int i) { cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItem(o, k), i); return cJSON_IsNumber(v) ? v->valuedouble : 0; }
static const char *jarrs(cJSON *o, const char *k, int i) { cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItem(o, k), i); return cJSON_IsString(v) ? v->valuestring : ""; }

static const char *lang2(void) {
    const char *l = cfg_get("lang", "en");
    return strlen(l) >= 2 ? l : "en";
}

/* the WMO weather code as a short text */
static const char *wx_text(int c) {
    if (c == 0) return T("Clear");
    if (c == 1) return T("Mainly clear");
    if (c == 2) return T("Partly cloudy");
    if (c == 3) return T("Overcast");
    if (c == 45 || c == 48) return T("Fog");
    if (c >= 51 && c <= 57) return T("Drizzle");
    if (c == 61 || c == 80) return T("Light rain");
    if (c == 63 || c == 81) return T("Rain");
    if (c == 65 || c == 82) return T("Heavy rain");
    if (c == 66 || c == 67) return T("Freezing rain");
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return T("Snow");
    if (c >= 95) return T("Thunderstorm");
    return "";
}

/* ---------- fetching (background) ---------- */
typedef struct {
    int gen;
    double lat, lon;
    bool geo;          /* no town yet: find the time zone's town first */
    char geo_q[96], lang[8];
    bool geo_ok;
    double glat, glon;
    char gname[128];
    Wx out;
} FetchJob;

static cJSON *get_json(const char *url) {
    HttpBuf b;
    memset(&b, 0, sizeof b);
    cJSON *j = NULL;
    if (http_request("GET", url, NULL, NULL, &b, 15) && b.status == 200 && b.data) j = cJSON_Parse(b.data);
    http_free(&b);
    return j;
}

static void fetch_work(void *arg) {
    FetchJob *j = arg;
    if (j->geo) {
        char *q = url_encode(j->geo_q);
        char *url = str_printf("https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=%s&format=json", q, j->lang);
        cJSON *g = get_json(url), *r = cJSON_GetArrayItem(cJSON_GetObjectItem(g, "results"), 0);
        if (r) {
            j->geo_ok = true;
            j->glat = json_num(r, "latitude");
            j->glon = json_num(r, "longitude");
            const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(r, "name"));
            snprintf(j->gname, sizeof j->gname, "%s", n ? n : j->geo_q);
            j->lat = j->glat;
            j->lon = j->glon;
        }
        cJSON_Delete(g);
        free(url);
        free(q);
        if (!j->geo_ok) return;
    }
    char *url = str_printf("https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                           "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
                           "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
                           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum"
                           "&timezone=auto&forecast_days=7&wind_speed_unit=ms",
                           j->lat, j->lon);
    cJSON *d = get_json(url);
    free(url);
    if (!d) return;
    Wx *w = &j->out;
    cJSON *cur = cJSON_GetObjectItem(d, "current");
    if (cur) {
        w->ok = true;
        w->temp = (float)json_num(cur, "temperature_2m");
        w->feels = (float)json_num(cur, "apparent_temperature");
        w->wind = (float)json_num(cur, "wind_speed_10m");
        w->code = (int)json_num(cur, "weather_code");
        w->day = json_num(cur, "is_day") != 0;
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cur, "time"));
        snprintf(w->time, sizeof w->time, "%s", t ? t : "");
    }
    /* the hours from the current one */
    cJSON *h = cJSON_GetObjectItem(d, "hourly");
    int n = cJSON_GetArraySize(cJSON_GetObjectItem(h, "time"));
    char now_h[20];
    snprintf(now_h, sizeof now_h, "%.13s", w->time); /* "2026-11-08T17" */
    for (int i = 0; i < n && w->nh < MAX_HOURS; i++) {
        const char *t = jarrs(h, "time", i);
        if (strncmp(t, now_h, 13) < 0) continue;
        WxHour *x = &w->h[w->nh++];
        snprintf(x->t, sizeof x->t, "%s", t);
        x->temp = (float)jarr(h, "temperature_2m", i);
        x->code = (int)jarr(h, "weather_code", i);
        x->rain = (int)jarr(h, "precipitation_probability", i);
        x->day = jarr(h, "is_day", i) != 0;
    }
    cJSON *dl = cJSON_GetObjectItem(d, "daily");
    n = cJSON_GetArraySize(cJSON_GetObjectItem(dl, "time"));
    for (int i = 0; i < n && w->nd < MAX_DAYS; i++) {
        WxDay *x = &w->d[w->nd++];
        snprintf(x->d, sizeof x->d, "%s", jarrs(dl, "time", i));
        x->code = (int)jarr(dl, "weather_code", i);
        x->tmax = (float)jarr(dl, "temperature_2m_max", i);
        x->tmin = (float)jarr(dl, "temperature_2m_min", i);
        x->rain = (float)jarr(dl, "precipitation_sum", i);
    }
    cJSON_Delete(d);
}

static void fetch_done(void *arg) {
    FetchJob *j = arg;
    fetching = false;
    if (j->gen == wx_gen) {
        if (j->geo_ok) {
            char b[48];
            snprintf(b, sizeof b, "%.4f", j->glat);
            cfg_set("wx_lat", b);
            snprintf(b, sizeof b, "%.4f", j->glon);
            cfg_set("wx_lon", b);
            cfg_set("wx_name", j->gname);
        }
        if (j->out.ok) {
            W = j->out;
            fetched_at = app_time();
            next_fetch = app_time() + REFRESH_S;
        } else {
            next_fetch = app_time() + 120; /* no connection: try again in 2 minutes */
        }
    }
    free(j);
}

/* the time zone's town: "Europe/Copenhagen" -> "Copenhagen" */
static void tz_town(char *out, size_t n) {
    const char *z = cfg_get("tz_zone", "");
    const char *s = strrchr(z, '/');
    snprintf(out, n, "%s", s ? s + 1 : "Copenhagen");
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

static void wx_fetch(void) {
    if (fetching) return;
    FetchJob *j = xcalloc(1, sizeof *j);
    j->gen = wx_gen;
    const char *la = cfg_get("wx_lat", ""), *lo = cfg_get("wx_lon", "");
    if (*la && *lo) {
        j->lat = atof(la);
        j->lon = atof(lo);
    } else {
        j->geo = true;
        tz_town(j->geo_q, sizeof j->geo_q);
    }
    snprintf(j->lang, sizeof j->lang, "%.2s", lang2());
    fetching = true;
    job_submit(fetch_work, fetch_done, j);
}

static void wx_tick(void) {
    if (app_time() >= next_fetch && !fetching) wx_fetch();
}

/* ---------- choosing the town ---------- */
typedef struct {
    char q[96], lang[8];
    int n;
    char name[8][128], label[8][200];
    double lat[8], lon[8];
} SearchJob;
static SearchJob *search_res;

static void search_work(void *arg) {
    SearchJob *s = arg;
    char *q = url_encode(s->q);
    char *url = str_printf("https://geocoding-api.open-meteo.com/v1/search?name=%s&count=8&language=%s&format=json", q, s->lang);
    cJSON *g = get_json(url), *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(g, "results")) {
        if (s->n >= 8) break;
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(r, "name")),
                   *a1 = cJSON_GetStringValue(cJSON_GetObjectItem(r, "admin1")),
                   *co = cJSON_GetStringValue(cJSON_GetObjectItem(r, "country"));
        if (!n) continue;
        snprintf(s->name[s->n], sizeof s->name[0], "%s", n);
        snprintf(s->label[s->n], sizeof s->label[0], "%s%s%s%s%s", n, a1 ? ", " : "", a1 ? a1 : "", co ? ", " : "", co ? co : "");
        s->lat[s->n] = json_num(r, "latitude");
        s->lon[s->n] = json_num(r, "longitude");
        s->n++;
    }
    cJSON_Delete(g);
    free(url);
    free(q);
}
static void town_chosen(void *ud, int idx) {
    SearchJob *s = ud;
    if (idx >= 0 && idx < s->n) {
        char b[48];
        snprintf(b, sizeof b, "%.4f", s->lat[idx]);
        cfg_set("wx_lat", b);
        snprintf(b, sizeof b, "%.4f", s->lon[idx]);
        cfg_set("wx_lon", b);
        cfg_set("wx_name", s->name[idx]);
        wx_gen++;
        fetching = false;
        memset(&W, 0, sizeof W);
        next_fetch = 0;
        wx_fetch();
    }
    if (search_res == s) search_res = NULL;
    free(s);
}
static void search_done(void *arg) {
    SearchJob *s = arg;
    if (!s->n) {
        app_toast(T("No town called \"%s\" was found"), s->q);
        free(s);
        return;
    }
    const char *items[8];
    for (int i = 0; i < s->n; i++) items[i] = s->label[i];
    search_res = s;
    choice_open(T("Choose the town"), items, s->n, 0, town_chosen, s);
}
static void town_typed(void *ud, const char *text) {
    (void)ud;
    if (!text) return;
    char *t = str_dup(text);
    str_trim(t);
    if (*t) {
        SearchJob *s = xcalloc(1, sizeof *s);
        snprintf(s->q, sizeof s->q, "%s", t);
        snprintf(s->lang, sizeof s->lang, "%.2s", lang2());
        job_submit(search_work, search_done, s);
    }
    free(t);
}
static void choose_town(void) { keyboard_open(T("Weather for which town?"), "", false, town_typed, NULL); }

/* ---------- drawing ---------- */
static const Color SUN = {255, 196, 54, 255}, CLOUD = {214, 222, 235, 255}, CLOUD_D = {150, 160, 178, 255},
                   RAIN_C = {96, 170, 255, 255}, MOON = {220, 226, 240, 255};

static void draw_cloud(float cx, float cy, float s, Color c) {
    fill_circle(cx - s * 0.16f, cy + s * 0.04f, s * 0.17f, c);
    fill_circle(cx + s * 0.06f, cy - s * 0.06f, s * 0.23f, c);
    fill_circle(cx + s * 0.24f, cy + s * 0.07f, s * 0.14f, c);
    fill_rrect(cx - s * 0.33f, cy + s * 0.04f, s * 0.71f, s * 0.18f, s * 0.09f, c);
}
static void draw_sun(float cx, float cy, float s) {
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 3.14159f / 4;
        fill_circle(cx + cosf(a) * s * 0.36f, cy + sinf(a) * s * 0.36f, s * 0.045f, SUN);
    }
    fill_circle(cx, cy, s * 0.24f, SUN);
}
static void draw_moon(float cx, float cy, float s) {
    fill_circle(cx, cy, s * 0.25f, MOON);
    fill_circle(cx - s * 0.08f, cy - s * 0.05f, s * 0.05f, RGB(196, 204, 222));
    fill_circle(cx + s * 0.07f, cy + s * 0.08f, s * 0.035f, RGB(196, 204, 222));
}
/* the icon for a weather code, centered on (cx, cy), about s wide */
static void draw_wx_icon(int c, bool day, float cx, float cy, float s) {
    if (c == 0 || c == 1) {
        if (day) draw_sun(cx, cy, s);
        else draw_moon(cx, cy, s);
        if (c == 1) draw_cloud(cx + s * 0.18f, cy + s * 0.2f, s * 0.5f, CLOUD);
        return;
    }
    if (c == 2) {
        if (day) draw_sun(cx - s * 0.14f, cy - s * 0.12f, s * 0.75f);
        else draw_moon(cx - s * 0.14f, cy - s * 0.12f, s * 0.75f);
        draw_cloud(cx + s * 0.06f, cy + s * 0.08f, s * 0.85f, CLOUD);
        return;
    }
    if (c == 45 || c == 48) { /* fog */
        for (int i = 0; i < 3; i++) fill_rrect(cx - s * 0.36f + (i % 2) * s * 0.08f, cy - s * 0.2f + i * s * 0.17f, s * 0.64f, s * 0.08f, s * 0.04f, CLOUD);
        return;
    }
    bool rain = (c >= 51 && c <= 67) || (c >= 80 && c <= 82), snow = (c >= 71 && c <= 77) || c == 85 || c == 86, storm = c >= 95;
    float up = rain || snow || storm ? s * 0.14f : 0;
    draw_cloud(cx, cy - up, s, rain || storm ? CLOUD_D : CLOUD);
    if (rain || storm) {
        int drops = c == 51 || c == 61 || c == 80 || c == 56 ? 2 : 3;
        for (int i = 0; i < drops; i++) {
            float x = cx - s * 0.18f + i * s * (drops == 2 ? 0.3f : 0.18f);
            fill_rrect(x, cy + s * 0.22f, s * 0.05f, s * 0.16f, s * 0.025f, RAIN_C);
        }
    }
    if (snow)
        for (int i = 0; i < 3; i++) fill_circle(cx - s * 0.16f + i * s * 0.16f, cy + s * 0.3f + (i % 2) * s * 0.06f, s * 0.045f, RGB(240, 246, 255));
    if (storm) {
        fill_tri(cx + s * 0.02f, cy + s * 0.12f, cx - s * 0.1f, cy + s * 0.32f, cx + s * 0.02f, cy + s * 0.3f, SUN);
        fill_tri(cx + s * 0.02f, cy + s * 0.26f, cx + s * 0.1f, cy + s * 0.26f, cx - s * 0.04f, cy + s * 0.48f, SUN);
    }
}

static void fmt_temp(float t, char *b, size_t n) {
    int v = (int)lroundf(t);
    snprintf(b, n, "%d°", v == 0 ? 0 : v); /* never "-0°" */
}
static const char *day_name(const char *iso, int idx) {
    if (idx == 0) return T("Today");
    static const char *names[] = {N_("Sun"), N_("Mon"), N_("Tue"), N_("Wed"), N_("Thu"), N_("Fri"), N_("Sat")};
    struct tm tm = {0};
    if (sscanf(iso, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return "";
    tm.tm_year -= 1900;
    tm.tm_mon--;
    tm.tm_hour = 12;
    time_t t = timegm(&tm);
    struct tm g;
    gmtime_r(&t, &g);
    return T(names[g.tm_wday]);
}
static const char *town(void) {
    const char *n = cfg_get("wx_name", "");
    return *n ? n : "";
}

/* the top bar of the home screen: icon + temperature */
static void wx_overlay(void) {
    if (!W.ok || !cfg_int("wx_topbar", 1)) return;
    Screen *t = app_top();
    if (!t || !t->name || strcmp(t->name, "home")) return;
    char tb[16];
    fmt_temp(W.temp, tb, sizeof tb);
    float tw = text_width(F_SEMI, 24, tb), w = 14 + 40 + 6 + tw + 20, h = 58, y = 50;
    float right = UI_W - 310 - 36 - text_width(F_REG, 22, T("Move app")) - 28;
    float x = app_overlay_place ? app_overlay_place(right, w) : right - w;
    fill_rrect(x, y, w, h, h / 2, RGBA(18, 20, 28, 205));
    stroke_rrect(x, y, w, h, h / 2, 1, RGBA(255, 255, 255, 22));
    draw_wx_icon(W.code, W.day, x + 14 + 20, y + h / 2, 40);
    text_draw(F_SEMI, 24, x + 14 + 40 + 6, y + 14, C_TEXT, AL_LEFT, tb);
}

/* the home screen when the app has focus: the weather now and the next hours */
static bool wx_shelf(float x, float y) {
    if (!W.ok) return false;
    char tb[16];
    fmt_temp(W.temp, tb, sizeof tb);
    draw_wx_icon(W.code, W.day, x + 80, y + 105, 170);
    float tx = x + 190;
    text_draw(F_BOLD, 110, tx, y + 30, C_TEXT, AL_LEFT, tb);
    float dx = tx + text_width(F_BOLD, 110, tb) + 40;
    text_draw(F_SEMI, 40, dx, y + 52, C_TEXT, AL_LEFT, wx_text(W.code));
    char line[200], f[16];
    fmt_temp(W.feels, f, sizeof f);
    snprintf(line, sizeof line, T("%s  ·  feels like %s  ·  wind %.0f m/s"), town(), f, W.wind);
    text_draw(F_REG, 28, dx, y + 108, C_TEXT2, AL_LEFT, line);
    /* the next hours, to the right */
    float hx = UI_W - x - 6 * 130, hy = y + 40;
    for (int i = 1; i <= 6 && i < W.nh; i++) {
        const WxHour *h = &W.h[i];
        float cx = hx + (i - 1) * 130 + 65;
        text_draw(F_REG, 22, cx, hy, C_TEXT3, AL_CENTER, h->t + 11);
        draw_wx_icon(h->code, h->day, cx, hy + 50, 44);
        fmt_temp(h->temp, f, sizeof f);
        text_draw(F_SEMI, 24, cx, hy + 78, C_TEXT, AL_CENTER, f);
    }
    return true;
}

/* ---------- the app's screen ---------- */
typedef struct { int sel; } WxView;
/* the choices at the top (chips): town and the home screen temperature, changed right here */
enum { WC_TOWN, WC_TOPBAR, N_WC };
static void wx_chips(WxView *v, float x, float y) {
    char tb[96];
    snprintf(tb, sizeof tb, "%s: %s", T("Temperature on the home screen"), cfg_int("wx_topbar", 1) ? T("On") : T("Off"));
    const char *chips[N_WC] = {T("Change town"), tb};
    draw_chips(x, y, chips, N_WC, v->sel);
}

static void wx_enter(Screen *s) {
    (void)s;
    if (!W.ok || app_time() - fetched_at > 5 * 60) { next_fetch = 0; wx_tick(); }
}
static void wx_update(Screen *s, float dt) { (void)s; (void)dt; wx_tick(); }

static void wx_draw(Screen *s) {
    WxView *v = s->st;
    draw_background();
    draw_topbar(NULL);
    float x0 = 120;
    if (!W.ok) {
        if (fetching) draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT);
        else draw_empty(T("No weather right now"), draw_chips ? T("The weather could not be fetched - check the network, or choose another town at the top.")
                                                              : T("The weather could not be fetched - check the network. Yellow chooses another town."));
        if (draw_chips) {
            text_draw(F_BOLD, 52, x0, 130, C_TEXT, AL_LEFT, *town() ? town() : T("Weather"));
            wx_chips(v, x0, 196);
        } else draw_keybar(x0, UI_H - 62, NULL, NULL, T("Change town"), NULL, NULL);
        return;
    }
    char b[200], f[16];
    /* now */
    text_draw(F_BOLD, 52, x0, 130, C_TEXT, AL_LEFT, *town() ? town() : T("Weather"));
    if (draw_chips) wx_chips(v, x0, 196);
    draw_wx_icon(W.code, W.day, x0 + 120, 360, 230);
    fmt_temp(W.temp, b, sizeof b);
    text_draw(F_BOLD, 150, x0 + 270, 250, C_TEXT, AL_LEFT, b);
    text_draw(F_SEMI, 40, x0 + 280, 430, C_TEXT, AL_LEFT, wx_text(W.code));
    fmt_temp(W.feels, f, sizeof f);
    snprintf(b, sizeof b, T("Feels like %s  ·  wind %.0f m/s"), f, W.wind);
    text_draw(F_REG, 28, x0 + 280, 486, C_TEXT2, AL_LEFT, b);

    /* 7 days, on the right */
    float dx = 1100, dy = 140, dw = 720, rh = 62;
    draw_panel(dx - 30, dy - 20, dw + 60, rh * W.nd + 40, 28, 1);
    float lo = 99, hi = -99;
    for (int i = 0; i < W.nd; i++) { if (W.d[i].tmin < lo) lo = W.d[i].tmin; if (W.d[i].tmax > hi) hi = W.d[i].tmax; }
    if (hi - lo < 1) hi = lo + 1;
    for (int i = 0; i < W.nd; i++) {
        const WxDay *d = &W.d[i];
        float y = dy + i * rh;
        text_draw(F_SEMI, 26, dx, y + 12, C_TEXT, AL_LEFT, day_name(d->d, i));
        draw_wx_icon(d->code, true, dx + 170, y + 30, 46);
        if (d->rain >= 0.1f) {
            snprintf(b, sizeof b, "%.1f mm", d->rain);
            text_draw(F_REG, 20, dx + 210, y + 17, RAIN_C, AL_LEFT, b);
        }
        fmt_temp(d->tmin, f, sizeof f);
        text_draw(F_REG, 24, dx + 380, y + 14, C_TEXT3, AL_RIGHT, f);
        float bx = dx + 400, bw = 230, a = (d->tmin - lo) / (hi - lo), e = (d->tmax - lo) / (hi - lo);
        fill_rrect(bx, y + 26, bw, 8, 4, RGBA(255, 255, 255, 30));
        fill_rrect(bx + bw * a, y + 26, fmaxf(10, bw * (e - a)), 8, 4, WX_ACCENT);
        fmt_temp(d->tmax, f, sizeof f);
        text_draw(F_SEMI, 24, dx + 650, y + 14, C_TEXT, AL_LEFT, f);
    }

    /* the next 24 hours, every second hour */
    float hy = 640, hx = x0;
    text_draw(F_BOLD, 32, hx, hy - 60, C_TEXT, AL_LEFT, T("Next 24 hours"));
    draw_panel(hx - 30, hy - 10, UI_W - 2 * x0 + 60, 250, 28, 1);
    int cols = 12;
    float cw = (UI_W - 2 * x0) / cols;
    for (int c = 0; c < cols; c++) {
        int i = c * 2;
        if (i >= W.nh) break;
        const WxHour *h = &W.h[i];
        float cx = hx + c * cw + cw / 2;
        text_draw(F_REG, 22, cx, hy + 14, C_TEXT3, AL_CENTER, i == 0 ? T("Now") : h->t + 11);
        draw_wx_icon(h->code, h->day, cx, hy + 90, 64);
        fmt_temp(h->temp, f, sizeof f);
        text_draw(F_SEMI, 28, cx, hy + 140, C_TEXT, AL_CENTER, f);
        if (h->rain > 0) {
            snprintf(b, sizeof b, "%d %%", h->rain);
            text_draw(F_REG, 20, cx, hy + 184, RAIN_C, AL_CENTER, b);
        }
    }
    if (fetched_at > 0) {
        int mins = (int)((app_time() - fetched_at) / 60);
        if (mins < 1) snprintf(b, sizeof b, "%s", T("Open-Meteo · updated just now"));
        else snprintf(b, sizeof b, T("Open-Meteo · updated %d min ago"), mins);
        text_draw(F_REG, 20, UI_W - x0, UI_H - 50, C_TEXT3, AL_RIGHT, b);
    }
    if (!draw_chips) draw_keybar(x0, UI_H - 62, NULL, NULL, T("Change town"), NULL, NULL);
}

static bool wx_input(Screen *s, const InputEvent *ev) {
    WxView *v = s->st;
    if (ev->a == A_YELLOW && !ev->repeat) { choose_town(); return true; } /* shortcut */
    if (ev->a == A_OK && !ev->repeat) {
        if (!draw_chips || v->sel == WC_TOWN) choose_town();
        else { cfg_set_int("wx_topbar", !cfg_int("wx_topbar", 1)); }
        return true;
    }
    if (draw_chips && (ev->a == A_LEFT || ev->a == A_RIGHT)) { buttons_nav(&v->sel, N_WC, ev); return true; }
    return false;
}
static void wx_destroy(Screen *s) {
    free(s->st);
    free(s);
}
static Screen *weather_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "weather";
    s->st = xcalloc(1, sizeof(WxView));
    s->enter = wx_enter;
    s->update = wx_update;
    s->draw = wx_draw;
    s->input = wx_input;
    s->destroy = wx_destroy;
    return s;
}

/* ---------- the app ---------- */
static void weather_init(void) {
    if (app_set_overlay) app_set_overlay("weather", wx_overlay);
}
/* uninstall: the chosen town and the weather go (nothing else is saved) */
static void weather_forget(void) {
    wx_gen++;
    fetching = false;
    memset(&W, 0, sizeof W);
    next_fetch = 0;
    cfg_set("wx_lat", "");
    cfg_set("wx_lon", "");
    cfg_set("wx_name", "");
}
static const char *town_value(void) { return *town() ? town() : T("From the time zone"); }
static const char *const onoff_v[] = {"0", "1", NULL}, *const onoff_l[] = {N_("Off"), N_("On"), NULL};
static const Item wx_items[] = {
    {N_("Town"), IT_ACTION, .action = choose_town, .value = town_value, .help = N_("The weather is for this town")},
    {N_("Temperature on the home screen"), IT_CYCLE, "wx_topbar", "1", onoff_v, onoff_l,
     .help = N_("The temperature and an icon at the top of the home screen")},
};
const NotaApp nota_app = {
    NOTA_API, "weather", "Weather", N_("The weather now, the next hours and 7 days"), APP_VERSION, 77, 0,
    ICON_STAR, {64, 156, 255, 255}, {28, 84, 170, 255}, {255, 255, 255, 255},
    .open = weather_screen, .init = weather_init, .tick = wx_tick, .forget = weather_forget,
    .settings = wx_items, .n_settings = sizeof wx_items / sizeof wx_items[0],
    .draw_shelf = wx_shelf,
};
