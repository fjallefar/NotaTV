/* NotaTV app package "elpris" - the Danish electricity spot price per hour, today and tomorrow (from about 13:00),
 * from Energi Data Service (Energinet's open data, no key or account): the price now, the cheapest hours and a
 * chart. Shown as the spot price with VAT - the grid tariff, the electricity tax and the supplier's fee are not in it
 * (they depend on the grid company and the contract). Price area West (DK1) or East (DK2): guessed from the Weather
 * app's town, Yellow switches. Built as app.so (see src/apps/README.md). */
#include "tv.h"
#include "vendor/cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EL_GREEN RGB(52, 199, 89)
#define EL_YELLOW RGB(255, 196, 54)
#define EL_RED RGB(255, 84, 72)
#define REFRESH_S (15 * 60)
#define MAX_H 72

typedef struct { time_t t; float kr; } Hour; /* the hour's start (UTC) and its average price, kr/kWh with VAT */
typedef struct {
    bool ok;
    char area[4];
    Hour h[MAX_H];
    int n;
} Prices;
static Prices P;
static double next_fetch, fetched_at;
static bool fetching;
static int el_gen;

/* West (DK1) or East (DK2): chosen, or guessed from the Weather app's town (Jutland and Funen are West of the
   Great Belt, about 11° East) */
static const char *area(void) {
    const char *a = cfg_get("el_area", "");
    if (!strcmp(a, "DK1") || !strcmp(a, "DK2")) return a;
    const char *lon = cfg_get("wx_lon", "");
    return *lon && atof(lon) < 11.0 ? "DK1" : "DK2";
}
static const char *area_name(const char *a) { return !strcmp(a, "DK1") ? T("West Denmark (DK1)") : T("East Denmark (DK2)"); }

/* ---------- fetching ---------- */
typedef struct { int gen; char area[4], start[16], end[16]; Prices out; } Job;

static time_t iso_utc(const char *s) {
    struct tm tm = {0};
    if (!s || sscanf(s, "%d-%d-%dT%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min) != 5) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon--;
    return timegm(&tm);
}

static void fetch_work(void *arg) {
    Job *j = arg;
    char *url = str_printf("https://api.energidataservice.dk/dataset/DayAheadPrices?start=%s&end=%s"
                           "&filter=%%7B%%22PriceArea%%22:%%5B%%22%s%%22%%5D%%7D&sort=TimeUTC%%20asc&limit=500",
                           j->start, j->end, j->area);
    HttpBuf b;
    memset(&b, 0, sizeof b);
    cJSON *d = NULL;
    if (http_request("GET", url, NULL, NULL, &b, 20) && b.status == 200 && b.data) d = cJSON_Parse(b.data);
    http_free(&b);
    free(url);
    if (!d) return;
    /* the prices come per quarter (from October 2025) or per hour: averaged per hour */
    double sum[MAX_H] = {0};
    int cnt[MAX_H] = {0};
    time_t first = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(d, "records")) {
        cJSON *pr = cJSON_GetObjectItem(r, "DayAheadPriceDKK");
        time_t t = iso_utc(cJSON_GetStringValue(cJSON_GetObjectItem(r, "TimeUTC")));
        if (!cJSON_IsNumber(pr) || !t) continue;
        t -= t % 3600;
        if (!first) first = t;
        int i = (int)((t - first) / 3600);
        if (i < 0 || i >= MAX_H) continue;
        sum[i] += pr->valuedouble;
        cnt[i]++;
    }
    cJSON_Delete(d);
    for (int i = 0; i < MAX_H; i++) {
        if (!cnt[i]) continue;
        Hour *h = &j->out.h[j->out.n++];
        h->t = first + (time_t)i * 3600;
        h->kr = (float)(sum[i] / cnt[i] / 1000.0 * 1.25); /* DKK/MWh -> kr/kWh with 25 % VAT */
    }
    j->out.ok = j->out.n > 0;
    snprintf(j->out.area, sizeof j->out.area, "%s", j->area);
}
static void fetch_done(void *arg) {
    Job *j = arg;
    fetching = false;
    if (j->gen == el_gen) {
        if (j->out.ok) {
            P = j->out;
            fetched_at = app_time();
            next_fetch = app_time() + REFRESH_S;
        } else {
            next_fetch = app_time() + 120;
        }
    }
    free(j);
}
static void el_fetch(void) {
    if (fetching) return;
    Job *j = calloc(1, sizeof *j);
    j->gen = el_gen;
    snprintf(j->area, sizeof j->area, "%s", area());
    time_t now = time(NULL), tom = now + 2 * 86400;
    struct tm a, b;
    localtime_r(&now, &a);
    localtime_r(&tom, &b);
    strftime(j->start, sizeof j->start, "%Y-%m-%d", &a);
    strftime(j->end, sizeof j->end, "%Y-%m-%d", &b);
    fetching = true;
    job_submit(fetch_work, fetch_done, j);
}
static void el_tick(void) {
    if (app_time() >= next_fetch && !fetching) el_fetch();
}
static void switch_area(void) {
    cfg_set("el_area", !strcmp(area(), "DK1") ? "DK2" : "DK1");
    el_gen++;
    fetching = false;
    memset(&P, 0, sizeof P);
    next_fetch = 0;
    el_fetch();
    app_toast("%s", area_name(area()));
}

/* ---------- reading the prices ---------- */
static int now_idx(void) {
    time_t now = time(NULL);
    for (int i = 0; i < P.n; i++)
        if (P.h[i].t <= now && now < P.h[i].t + 3600) return i;
    return -1;
}
/* the hours of the day the hour i is in (local time): [*a, *b) */
static void day_range(int i, int *a, int *b) {
    struct tm x, y;
    localtime_r(&P.h[i].t, &x);
    *a = i;
    while (*a > 0) { localtime_r(&P.h[*a - 1].t, &y); if (y.tm_yday != x.tm_yday) break; (*a)--; }
    *b = i + 1;
    while (*b < P.n) { localtime_r(&P.h[*b].t, &y); if (y.tm_yday != x.tm_yday) break; (*b)++; }
}
/* the color of a price compared with the day's: green the cheapest third, red the dearest third */
static Color price_color(int i) {
    int a, b;
    day_range(i, &a, &b);
    int below = 0;
    for (int k = a; k < b; k++) if (P.h[k].kr < P.h[i].kr) below++;
    float r = b - a > 1 ? (float)below / (float)(b - a - 1) : 0.5f;
    return r < 0.34f ? EL_GREEN : r < 0.67f ? EL_YELLOW : EL_RED;
}
/* the cheapest run of len hours from now on: the first hour's index, -1 = none */
static int cheapest_run(int len, float *avg) {
    int from = now_idx();
    if (from < 0) from = 0;
    int best = -1;
    float bs = 1e9f;
    for (int i = from; i + len <= P.n; i++) {
        float s = 0;
        for (int k = 0; k < len; k++) s += P.h[i + k].kr;
        if (s < bs) { bs = s; best = i; }
    }
    if (avg) *avg = best >= 0 ? bs / len : 0;
    return best;
}
static void fmt_kr(float kr, char *b, size_t n) { /* "1,92" (Danish) or "1.92" */
    snprintf(b, n, "%.2f", kr);
    if (!strncmp(cfg_get("lang", "en"), "da", 2)) for (char *p = b; *p; p++) if (*p == '.') *p = ',';
}
static void fmt_hour(time_t t, char *b, size_t n) {
    struct tm x;
    localtime_r(&t, &x);
    snprintf(b, n, "%02d", x.tm_hour);
}
static const char *day_word(time_t t) {
    time_t now = time(NULL);
    struct tm a, b;
    localtime_r(&now, &a);
    localtime_r(&t, &b);
    return a.tm_yday == b.tm_yday ? T("today") : T("tomorrow");
}

/* ---------- drawing ---------- */
static void draw_chart(float x, float y, float w, float h, int from, int to, bool labels) {
    if (to <= from) return;
    float lo = 0, hi = 0.1f;
    for (int i = from; i < to; i++) { if (P.h[i].kr > hi) hi = P.h[i].kr; if (P.h[i].kr < lo) lo = P.h[i].kr; }
    float span = hi - lo, bw = w / (float)(to - from), zero = y + h * (hi / span);
    int ni = now_idx();
    for (int i = from; i < to; i++) {
        float v = P.h[i].kr, bx = x + (i - from) * bw, top = y + h * ((hi - fmaxf(v, 0)) / span), bot = y + h * ((hi - fminf(v, 0)) / span);
        Color c = price_color(i);
        if (i != ni) c = color_alpha(c, i < ni ? 0.35f : 0.8f);
        fill_rrect(bx + bw * 0.12f, top, bw * 0.76f, fmaxf(bot - top, 2), fminf(bw * 0.2f, 5), c);
        if (i == ni) stroke_rrect(bx + bw * 0.04f, top - 6, bw * 0.92f, fmaxf(bot - top, 2) + 12, 6, 2, C_TEXT);
        if (labels) {
            struct tm t;
            localtime_r(&P.h[i].t, &t);
            if (t.tm_hour % 3 == 0) {
                char hb[8];
                snprintf(hb, sizeof hb, "%02d", t.tm_hour);
                text_draw(F_REG, 18, bx + bw / 2, y + h + 12, C_TEXT3, AL_CENTER, hb);
            }
            if (t.tm_hour == 0 && i > from) fill_rect(bx, y - 10, 2, h + 20, RGBA(255, 255, 255, 60)); /* midnight */
        }
    }
    if (lo < 0) fill_rect(x, zero, w, 1, RGBA(255, 255, 255, 80));
}

/* the home screen when the app has focus */
static bool el_shelf(float x, float y) {
    int ni = now_idx();
    if (!P.ok || ni < 0) return false;
    char b[64], line[240];
    fmt_kr(P.h[ni].kr, b, sizeof b);
    Color c = price_color(ni);
    fill_circle(x + 30, y + 90, 22, c);
    text_draw(F_BOLD, 110, x + 76, y + 30, C_TEXT, AL_LEFT, b);
    float ux = x + 76 + text_width(F_BOLD, 110, b) + 22;
    text_draw(F_SEMI, 34, ux, y + 64, C_TEXT2, AL_LEFT, T("kr/kWh now"));
    float avg;
    int r = cheapest_run(3, &avg);
    if (r >= 0) {
        char a[8], e[8], k[16];
        fmt_hour(P.h[r].t, a, sizeof a);
        fmt_hour(P.h[r].t + 3 * 3600, e, sizeof e);
        fmt_kr(avg, k, sizeof k);
        snprintf(line, sizeof line, T("Cheapest 3 hours: %s %s-%s (%s kr)"), day_word(P.h[r].t), a, e, k);
        text_draw(F_REG, 28, x, y + 170, C_TEXT2, AL_LEFT, line);
    }
    int a, e;
    day_range(ni, &a, &e);
    if (e < P.n) e = P.n; /* tomorrow too, when it is there */
    draw_chart(UI_W - x - 760, y + 20, 760, 180, a, e, false);
    return true;
}

typedef struct { int dummy; } ElView;
static void el_enter(Screen *s) {
    (void)s;
    if (!P.ok || app_time() - fetched_at > 5 * 60 || strcmp(P.area, area())) { next_fetch = 0; el_tick(); }
}
static void el_update(Screen *s, float dt) { (void)s; (void)dt; el_tick(); }

static void el_draw(Screen *s) {
    (void)s;
    draw_background();
    draw_topbar(NULL);
    float x0 = 120;
    text_draw(F_BOLD, 52, x0, 130, C_TEXT, AL_LEFT, T("Electricity price"));
    text_draw(F_REG, 26, x0, 196, C_TEXT3, AL_LEFT, area_name(area()));
    int ni = now_idx();
    if (!P.ok || ni < 0) {
        if (fetching) draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT);
        else draw_empty(T("No prices right now"), T("The prices could not be fetched - check the network."));
        draw_keybar(x0, UI_H - 62, NULL, NULL, T("Switch area"), NULL, NULL);
        return;
    }
    char b[64], k[16], line[240];
    /* now */
    fmt_kr(P.h[ni].kr, b, sizeof b);
    Color c = price_color(ni);
    fill_circle(x0 + 34, 352, 26, c);
    text_draw(F_BOLD, 150, x0 + 84, 270, C_TEXT, AL_LEFT, b);
    text_draw(F_SEMI, 36, x0 + 94 + text_width(F_BOLD, 150, b), 318, C_TEXT2, AL_LEFT, T("kr/kWh now"));
    /* the cheapest hours and the day's numbers, on the right */
    float rx = 1080, ry = 250;
    float avg;
    for (int len = 1; len <= 3; len += 2) {
        int r = cheapest_run(len, &avg);
        if (r < 0) continue;
        char a[8], e[8];
        fmt_hour(P.h[r].t, a, sizeof a);
        fmt_hour(P.h[r].t + len * 3600, e, sizeof e);
        fmt_kr(avg, k, sizeof k);
        snprintf(line, sizeof line, len == 1 ? T("Cheapest hour: %s %s-%s  ·  %s kr") : T("Cheapest 3 hours: %s %s-%s  ·  %s kr"),
                 day_word(P.h[r].t), a, e, k);
        text_draw(F_SEMI, 28, rx, ry, EL_GREEN, AL_LEFT, line);
        ry += 50;
    }
    int a, e;
    day_range(ni, &a, &e);
    float lo = 1e9f, hi = -1e9f, sum = 0;
    for (int i = a; i < e; i++) { lo = fminf(lo, P.h[i].kr); hi = fmaxf(hi, P.h[i].kr); sum += P.h[i].kr; }
    char l1[16], l2[16], l3[16];
    fmt_kr(lo, l1, sizeof l1);
    fmt_kr(hi, l2, sizeof l2);
    fmt_kr(sum / (e - a), l3, sizeof l3);
    snprintf(line, sizeof line, T("Today: lowest %s  ·  highest %s  ·  average %s kr"), l1, l2, l3);
    text_draw(F_REG, 26, rx, ry + 10, C_TEXT2, AL_LEFT, line);

    /* the chart: today and tomorrow when it is there */
    float cy = 540, chh = 330;
    draw_panel(x0 - 30, cy - 40, UI_W - 2 * x0 + 60, chh + 110, 28, 1);
    draw_chart(x0, cy, UI_W - 2 * x0, chh, a, P.n, true);
    if (e >= P.n) text_draw(F_REG, 22, UI_W - x0, cy - 30, C_TEXT3, AL_RIGHT, T("Tomorrow's prices come at about 13:00"));
    text_draw(F_REG, 20, x0, UI_H - 108, C_TEXT3, AL_LEFT,
              T("Spot price with VAT - without the grid tariff, electricity tax and your supplier's fee. Source: Energi Data Service"));
    draw_keybar(x0, UI_H - 62, NULL, NULL, T("Switch area"), NULL, NULL);
}
static bool el_input(Screen *s, const InputEvent *ev) {
    (void)s;
    if ((ev->a == A_YELLOW || ev->a == A_OK) && !ev->repeat) { switch_area(); return true; }
    return false;
}
static void el_destroy(Screen *s) {
    free(s->st);
    free(s);
}
static Screen *elpris_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "elpris";
    s->st = calloc(1, sizeof(ElView));
    s->enter = el_enter;
    s->update = el_update;
    s->draw = el_draw;
    s->input = el_input;
    s->destroy = el_destroy;
    return s;
}

/* ---------- the app ---------- */
static void elpris_forget(void) {
    el_gen++;
    fetching = false;
    memset(&P, 0, sizeof P);
    next_fetch = 0;
    cfg_set("el_area", "");
}
static const char *area_value(void) { return area_name(area()); }
static const Item el_items[] = {
    {N_("Price area"), IT_ACTION, .action = switch_area, .value = area_value,
     .help = N_("West: Jutland and Funen. East: Zealand, the islands and Bornholm")},
};
const NotaApp nota_app = {
    NOTA_API, "elpris", "Electricity price", N_("The electricity price hour by hour, today and tomorrow"), APP_VERSION, 78, 0,
    ICON_STAR, {34, 197, 94, 255}, {15, 95, 51, 255}, {255, 255, 255, 255},
    .open = elpris_screen, .tick = el_tick, .forget = elpris_forget,
    .settings = el_items, .n_settings = sizeof el_items / sizeof el_items[0],
    .draw_shelf = el_shelf,
};
