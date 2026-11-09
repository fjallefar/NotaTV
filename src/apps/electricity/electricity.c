/* NotaTV app package "electricity" - the Danish electricity spot price per hour, today and tomorrow (from about 13:00),
 * from Energi Data Service (Energinet's open data, no key or account): the price now, the cheapest hours and a
 * chart. With the grid company chosen the price is the full one: spot price + the grid company's hourly
 * tariff + Energinet's tariffs + electricity tax (all from Energi Data Service's DatahubPricelist), with VAT, plus the
 * supplier's surcharge if one is set (it cannot be fetched). Without a grid company it is the spot price with VAT.
 * Price area West (DK1) or East (DK2): guessed from the Weather app's town. Area, grid company and surcharge are
 * chosen with the chips at the top (also in Settings); the first time the app asks for the grid company.
 * Built as app.so (see src/apps/README.md). */
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
#define REFRESH_S (6 * 3600) /* and at 13:15, see next_refresh */
#define RETRY_S (5 * 60)     /* after a failed fetch (the service limits how often it may be asked) */
#define MAX_H 72

typedef struct {
    time_t t;   /* the hour's start (UTC) */
    float spot; /* the hour's average spot price, kr/kWh without VAT */
    float kr;   /* the price shown: kr/kWh with VAT, tariffs and surcharge (see el_recalc) */
} Hour;
typedef struct {
    bool ok;
    char area[4];
    Hour h[MAX_H];
    int n;
} Prices;
static Prices P;
static time_t next_fetch; /* wall clock, so it also holds across sleep; 0 = as soon as possible */
static bool fetching;
static int el_gen;

/* ---------- the grid company and the tariffs ---------- */
#define ENERGINET_GLN "5790000432752" /* Energinet: transmission grid tariff, system tariff and electricity tax */
static const char *const en_codes[] = {"40000", "41000", "EA-001", NULL};
/* the household tariff (C customers, hourly) of each grid company: GLN, the tariff's ChargeTypeCode, a discount
   code that is added too (Konstant). One GLN when a company has several with the same tariff (N1, Konstant) */
typedef struct { const char *gln, *code, *code2; } Grid;
static const char *const grid_o[] = {"", "5790001095451", "5790000705184", "5790000610099", "5790000836239",
    "5790001100520", "5790000392551", "5790001088309", "5790000681105", "5790001090166", "5790000682102",
    "5790000704842", "5790001090111", "5790001103460", "5790001089023", "5790000611003", "5790000395620",
    "5790000681358", "5790000610877", "5790000681327", "5790000705689", "5790000836727", "5790001095444",
    "5790000706419", "5790000392261", "5790000706686", "5790001088217", "5790000610976", "5790001089375", NULL};
static const char *const grid_l[] = {N_("Not chosen"), "Aal El-Net", "Cerius", "Dinel", "Elektrus",
    "Elnet Midt", "FLOW Elnet", "Forsyning Elnet", "Grindsted Elnet", "Hammel Elforsyning Net", "Ikast El Net",
    "Konstant Net", "L-Net", "Læsø Elnet", "Midtfyns Elforsyning", "N1", "NOE Net",
    "Netselskabet Elværk", "Nord Energi Net", "RAH Net", "Radius Elnet", "Ravdex", "Sunds Net",
    "Tarm Elværk Net", "TREFOR El-net", "TREFOR El-net Øst", "Veksel", "Vores Elnet", "Zeanet", NULL};
static const Grid grids[] = {
    {"5790001095451", "AAL-NT-05", NULL}, {"5790000705184", "30TR_C_ET", "30RE_C_ET"}, {"5790000610099", "TCL<100_02", NULL},
    {"5790000836239", "6000091", NULL}, {"5790001100520", "T3001", NULL}, {"5790000392551", "FE1 NT-01", NULL},
    {"5790001088309", "STR-NT-03", NULL}, {"5790000681105", "GEV-NT-01", NULL}, {"5790001090166", "C-Tarif", NULL},
    {"5790000682102", "IEV-NT-01", NULL}, {"5790000704842", "C_FBTNTR_B", "C_FBTNTR_R"}, {"5790001090111", "3000", NULL},
    {"5790001103460", "43100", NULL}, {"5790001089023", "TNT15000", NULL}, {"5790000611003", "T-C-F-F-TD", NULL},
    {"5790000395620", "30030", NULL}, {"5790000681358", "5NCFF", NULL}, {"5790000610877", "TAC", NULL},
    {"5790000681327", "RAH-C", NULL}, {"5790000705689", "DT_C_01", NULL}, {"5790000836727", "NT-C", NULL},
    {"5790001095444", "SEF-NT-05", NULL}, {"5790000706419", "TEV-NT-01T", NULL}, {"5790000392261", "C", NULL},
    {"5790000706686", "46", NULL}, {"5790001088217", "NT-01", NULL}, {"5790000610976", "TNT1009", NULL},
    {"5790001089375", "43110", NULL},
};
static const Grid *grid(void) {
    const char *g = cfg_get("el_grid", "");
    for (size_t i = 0; i < sizeof grids / sizeof grids[0]; i++)
        if (!strcmp(grids[i].gln, g)) return &grids[i];
    return NULL;
}
static const char *grid_name(void) {
    const char *g = cfg_get("el_grid", "");
    for (int i = 1; grid_o[i]; i++) if (!strcmp(grid_o[i], g)) return grid_l[i];
    return "";
}
/* the supplier's surcharge, kr/kWh with VAT (on the bill; it cannot be fetched) */
static const char *const fee_o[] = {"0", "0.05", "0.10", "0.15", "0.20", "0.25", "0.30", "0.40", "0.50", "0.75", "1.00", NULL};
static const char *const fee_l[] = {N_("None"), "5 øre", "10 øre", "15 øre", "20 øre", "25 øre", "30 øre", "40 øre",
    "50 øre", "75 øre", "1 kr", NULL};

/* one price list entry: a tariff's prices per local hour (kr/kWh without VAT) in [from, to) local time */
typedef struct { char code[16], from[20], to[20]; float p[24]; } Tariff;
#define MAX_TR 600
#define TR_REFRESH_S (6 * 3600)
static Tariff *TR;
static int n_tr;
static bool tr_ok, tr_fetching;
static char tr_key[16]; /* the GLN the tariffs (or the last try) are for */
static double tr_next;
static int tr_gen;

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
        h->spot = (float)(sum[i] / cnt[i] / 1000.0); /* DKK/MWh -> kr/kWh */
        h->kr = h->spot * 1.25f;
    }
    j->out.ok = j->out.n > 0;
    snprintf(j->out.area, sizeof j->out.area, "%s", j->area);
}
/* the tariffs valid at the local time t, summed (kr/kWh without VAT); each tariff code counts once */
static float tariff_at(time_t t) {
    if (!tr_ok || !TR) return 0;
    struct tm lt;
    localtime_r(&t, &lt);
    char ts[20];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &lt);
    const char *done[8];
    int nd = 0;
    float sum = 0;
    for (int i = 0; i < n_tr; i++) {
        Tariff *r = &TR[i];
        if (strcmp(r->from, ts) > 0 || (r->to[0] && strcmp(ts, r->to) >= 0)) continue;
        bool dup = false;
        for (int k = 0; k < nd; k++) if (!strcmp(done[k], r->code)) dup = true;
        if (dup || nd >= 8) continue;
        done[nd++] = r->code;
        sum += r->p[lt.tm_hour];
    }
    return sum;
}
static bool with_tariffs(void) { const Grid *g = grid(); return g && tr_ok && !strcmp(tr_key, g->gln); }
static float fee(void) { return (float)atof(cfg_get("el_fee", "0")); }
static float price_of(float spot, time_t t) {
    return (spot + (with_tariffs() ? tariff_at(t) : 0)) * 1.25f + fee();
}
/* the shown prices again: after new spot prices or tariffs, or a changed setting */
static void el_recalc(void) {
    for (int i = 0; i < P.n; i++) P.h[i].kr = price_of(P.h[i].spot, P.h[i].t);
}

typedef struct { int gen; char gln[16], code[16], code2[16]; Tariff *out; int n; bool ok; } TrJob;
static bool code_in(const char *c, const char *const *list) {
    for (; *list; list++) if (!strcmp(c, *list)) return true;
    return false;
}
static void tr_work(void *arg) {
    TrJob *j = arg;
    char filt[400];
    snprintf(filt, sizeof filt,
             "{\"GLN_Number\":[\"%s\",\"" ENERGINET_GLN "\"],\"ChargeType\":[\"D03\"],"
             "\"ChargeTypeCode\":[\"%s\"%s%s%s,\"40000\",\"41000\",\"EA-001\"]}",
             j->gln, j->code, j->code2[0] ? ",\"" : "", j->code2, j->code2[0] ? "\"" : "");
    char *ef = url_encode(filt);
    /* the entries that started within the last year or start in the next days (ValidFrom is the time column) */
    char *url = str_printf("https://api.energidataservice.dk/dataset/DatahubPricelist?start=now-P1Y&end=now%%2BP3D"
                           "&filter=%s&sort=ValidFrom%%20asc&limit=%d", ef, MAX_TR);
    free(ef);
    HttpBuf b;
    memset(&b, 0, sizeof b);
    cJSON *d = NULL;
    if (http_request("GET", url, NULL, NULL, &b, 20) && b.status == 200 && b.data) d = cJSON_Parse(b.data);
    http_free(&b);
    free(url);
    if (!d) return;
    j->out = calloc(MAX_TR, sizeof *j->out);
    cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(d, "records")) {
        const char *gln = cJSON_GetStringValue(cJSON_GetObjectItem(r, "GLN_Number")),
                   *code = cJSON_GetStringValue(cJSON_GetObjectItem(r, "ChargeTypeCode")),
                   *from = cJSON_GetStringValue(cJSON_GetObjectItem(r, "ValidFrom")),
                   *to = cJSON_GetStringValue(cJSON_GetObjectItem(r, "ValidTo"));
        if (!gln || !code || !from || j->n >= MAX_TR) continue;
        bool mine = !strcmp(gln, j->gln) && (!strcmp(code, j->code) || (j->code2[0] && !strcmp(code, j->code2)));
        bool en = !strcmp(gln, ENERGINET_GLN) && code_in(code, en_codes);
        if (!mine && !en) continue;
        cJSON *p1 = cJSON_GetObjectItem(r, "Price1");
        if (!cJSON_IsNumber(p1)) continue;
        Tariff *t = &j->out[j->n++];
        snprintf(t->code, sizeof t->code, "%s", code);
        snprintf(t->from, sizeof t->from, "%s", from);
        snprintf(t->to, sizeof t->to, "%s", to ? to : "");
        for (int h = 0; h < 24; h++) { /* one price for the whole day (P1D) when the hours are empty */
            char k[10];
            snprintf(k, sizeof k, "Price%d", h + 1);
            cJSON *ph = cJSON_GetObjectItem(r, k);
            t->p[h] = (float)(cJSON_IsNumber(ph) ? ph->valuedouble : p1->valuedouble);
        }
    }
    cJSON_Delete(d);
    /* newest first, so a code's latest entry wins where two overlap */
    for (int a = 0, z = j->n - 1; a < z; a++, z--) { Tariff x = j->out[a]; j->out[a] = j->out[z]; j->out[z] = x; }
    j->ok = j->n > 0;
}
static void tr_done(void *arg) {
    TrJob *j = arg;
    tr_fetching = false;
    if (j->gen == tr_gen) {
        snprintf(tr_key, sizeof tr_key, "%s", j->gln);
        if (j->ok) {
            free(TR);
            TR = j->out;
            j->out = NULL;
            n_tr = j->n;
            tr_ok = true;
            tr_next = app_time() + TR_REFRESH_S;
        } else {
            tr_next = app_time() + 75; /* the service limits requests per minute */
        }
        el_recalc();
    }
    free(j->out);
    free(j);
}
static void tr_tick(void) {
    const Grid *g = grid();
    if (!g || tr_fetching || (app_time() < tr_next && !strcmp(tr_key, g->gln))) return;
    TrJob *j = calloc(1, sizeof *j);
    j->gen = tr_gen;
    snprintf(j->gln, sizeof j->gln, "%s", g->gln);
    snprintf(j->code, sizeof j->code, "%s", g->code);
    snprintf(j->code2, sizeof j->code2, "%s", g->code2 ? g->code2 : "");
    tr_fetching = true;
    job_submit(tr_work, tr_done, j);
}
static void grid_changed(void) {
    tr_gen++;
    tr_fetching = false;
    tr_ok = false;
    tr_key[0] = 0;
    tr_next = 0;
    el_recalc();
    tr_tick();
}

/* the prices are remembered, so a restart or a refused fetch still shows them: first line the area, then "time spot" */
static void cache_path(char *p, size_t n) { snprintf(p, n, "%s/electricity-prices.txt", data_dir()); }
static void prices_save(void) {
    char p[512], tmp[520];
    cache_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%s\n", P.area);
    for (int i = 0; i < P.n; i++) fprintf(f, "%lld %.5f\n", (long long)P.h[i].t, P.h[i].spot);
    if (fclose(f) == 0) rename(tmp, p);
    else remove(tmp);
}
static void prices_load(void) {
    if (P.ok) return;
    char p[512], a[8] = "";
    cache_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    long long t;
    float spot;
    if (fscanf(f, "%7s", a) == 1 && !strcmp(a, area())) {
        Prices c;
        memset(&c, 0, sizeof c);
        while (c.n < MAX_H && fscanf(f, "%lld %f", &t, &spot) == 2) {
            c.h[c.n].t = (time_t)t;
            c.h[c.n].spot = spot;
            c.h[c.n].kr = spot * 1.25f;
            c.n++;
        }
        if (c.n) {
            c.ok = true;
            snprintf(c.area, sizeof c.area, "%s", a);
            P = c;
            el_recalc();
        }
    }
    fclose(f);
}
/* every 6 hours, and at 13:15 when tomorrow's prices are out */
static time_t next_refresh(time_t now) {
    struct tm t;
    localtime_r(&now, &t);
    t.tm_hour = 13;
    t.tm_min = 15;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t day = mktime(&t), six = now + REFRESH_S;
    if (day <= now) day += 86400;
    return day < six ? day : six;
}

static void fetch_done(void *arg) {
    Job *j = arg;
    fetching = false;
    if (j->gen == el_gen) {
        if (j->out.ok) {
            P = j->out;
            el_recalc();
            prices_save();
            next_fetch = next_refresh(time(NULL));
        } else {
            next_fetch = time(NULL) + RETRY_S;
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
    time_t now = time(NULL);
    if (now < 1600000000) return; /* the clock is not set yet */
    if (now >= next_fetch && !fetching) el_fetch();
    else if (!fetching) tr_tick(); /* not at the same moment as the spot prices (rate limit) */
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
/* the price right now: the hour's (the average of its quarters, as the electricity companies' apps show it) */
static float now_kr(int ni) { return P.h[ni].kr; }
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
/* mark = the hour with the frame; labels = the big chart: hours, a line per krone and the marked hour's price */
static void draw_chart(float x, float y, float w, float h, int from, int to, bool labels, int mark) {
    if (to <= from) return;
    float lo = 0, hi = 0.1f;
    for (int i = from; i < to; i++) { if (P.h[i].kr > hi) hi = P.h[i].kr; if (P.h[i].kr < lo) lo = P.h[i].kr; }
    float span = hi - lo, bw = w / (float)(to - from), zero = y + h * (hi / span);
    int ni = now_idx();
    if (labels) /* behind the bars */
        for (int k = (int)ceilf(lo); k <= (int)floorf(hi); k++) {
            float ky = y + h * ((hi - k) / span);
            char kb[16];
            snprintf(kb, sizeof kb, "%d kr", k);
            if (k) fill_rect(x, ky, w, 1, RGBA(255, 255, 255, 30));
            text_draw(F_REG, 20, x - 12, ky - 12, C_TEXT3, AL_RIGHT, kb);
        }
    for (int i = from; i < to; i++) {
        float v = P.h[i].kr, bx = x + (i - from) * bw, top = y + h * ((hi - fmaxf(v, 0)) / span), bot = y + h * ((hi - fminf(v, 0)) / span);
        Color c = price_color(i);
        if (i != ni && i != mark) c = color_alpha(c, i < ni ? 0.35f : 0.8f);
        fill_rrect(bx + bw * 0.12f, top, bw * 0.76f, fmaxf(bot - top, 2), fminf(bw * 0.2f, 5), c);
        if (i == mark) {
            stroke_rrect(bx + bw * 0.04f, top - 6, bw * 0.92f, fmaxf(bot - top, 2) + 12, 6, 2, C_TEXT);
            if (labels) {
                char pb[16];
                fmt_kr(v, pb, sizeof pb);
                text_draw(F_BOLD, 24, bx + bw / 2, top - 38, C_TEXT, AL_CENTER, pb);
            }
        }
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
    fmt_kr(now_kr(ni), b, sizeof b);
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
    draw_chart(UI_W - x - 760, y + 20, 760, 180, a, e, false, ni);
    return true;
}

/* the top bar of the home screen: the price right now, with the hour's color */
static void el_overlay(void) {
    int ni = now_idx();
    if (!P.ok || ni < 0 || !cfg_int("el_topbar", 1)) return;
    Screen *t = app_top();
    if (!t || !t->name || strcmp(t->name, "home")) return;
    char n[32], b[48];
    fmt_kr(now_kr(ni), n, sizeof n);
    snprintf(b, sizeof b, T("%s kr"), n);
    float tw = text_width(F_SEMI, 24, b), w = 20 + 16 + 12 + tw + 22, h = 58, y = 50;
    float right = UI_W - 310 - 36 - text_width(F_REG, 22, T("Move app")) - 28;
    float x = app_overlay_place ? app_overlay_place(right, w) : right - w;
    fill_rrect(x, y, w, h, h / 2, RGBA(18, 20, 28, 205));
    stroke_rrect(x, y, w, h, h / 2, 1, RGBA(255, 255, 255, 22));
    fill_circle(x + 20 + 8, y + h / 2, 8, price_color(ni));
    text_draw(F_SEMI, 24, x + 20 + 16 + 12, y + 14, C_TEXT, AL_LEFT, b);
}

/* ---------- the choices at the top (chips): area, grid company, surcharge - chosen right here, not in Settings ---------- */
typedef struct {
    int sel;       /* the chip in focus */
    bool on_chips; /* the focus: the chips at the top (↑), otherwise the chart */
    time_t sel_t;  /* the hour chosen in the chart; 0 = now (follows the clock) */
} ElView;
enum { CH_AREA, CH_GRID, CH_FEE, CH_TOP, N_CH };
static int opt_at(const char *const *o, const char *key, const char *def) {
    const char *v = cfg_get(key, def);
    for (int i = 0; o[i]; i++) if (!strcmp(o[i], v)) return i;
    return 0;
}
static void area_picked(void *ud, int idx) {
    (void)ud;
    if (idx < 0 || !strcmp(area(), idx == 0 ? "DK1" : "DK2")) return;
    switch_area();
}
static void grid_picked(void *ud, int idx) {
    (void)ud;
    if (idx < 0) return;
    cfg_set("el_grid", grid_o[idx]);
    grid_changed();
}
static void fee_picked(void *ud, int idx) {
    (void)ud;
    if (idx < 0) return;
    cfg_set("el_fee", fee_o[idx]);
    el_recalc();
}
/* translated copies of a label list for choice_open; [0] can be replaced */
static int labels_tr(const char *const *l, const char **out, int max, const char *first) {
    int n = 0;
    for (; l[n] && n < max; n++) out[n] = T(l[n]);
    if (first && n) out[0] = first;
    return n;
}
static void pick_grid(const char *title) {
    const char *it[40];
    int n = labels_tr(grid_l, it, 40, T("I don't know - show the spot price"));
    choice_open(title, it, n, opt_at(grid_o, "el_grid", ""), grid_picked, NULL);
}
static void chip_open(int c) {
    if (c == CH_TOP) {
        cfg_set_int("el_topbar", !cfg_int("el_topbar", 1));
    } else if (c == CH_AREA) {
        const char *it[] = {T("West Denmark (DK1)"), T("East Denmark (DK2)")};
        choice_open(T("Price area\nWest: Jutland and Funen. East: Zealand, the islands and Bornholm"), it, 2, !strcmp(area(), "DK1") ? 0 : 1, area_picked, NULL);
    } else if (c == CH_GRID) {
        pick_grid(T("Grid company\nThe company that owns the cables to your home - it is on your electricity bill"));
    } else {
        const char *it[20];
        int n = labels_tr(fee_l, it, 20, NULL);
        choice_open(T("Supplier's surcharge\nWhat your electricity company adds per kWh, with VAT - on your bill or in your contract"), it, n, opt_at(fee_o, "el_fee", "0"), fee_picked, NULL);
    }
}
static void el_enter(Screen *s) {
    (void)s;
    if (P.ok && strcmp(P.area, area())) { next_fetch = 0; el_tick(); } /* opening the app does not fetch: see next_refresh */
    /* the first time: ask the one thing that cannot be guessed (the area is guessed from the Weather town) */
    if (draw_chips && !cfg_int("el_asked", 0)) {
        cfg_set_int("el_asked", 1);
        if (!grid()) pick_grid(T("Who owns the cables to your home?\nIt is on your electricity bill - it gives the full price. You can change it at the top"));
    }
}
static void el_update(Screen *s, float dt) { (void)s; (void)dt; el_tick(); }

/* the hour shown in the chart and the big number: the chosen one while it still exists, otherwise now */
static int sel_idx(const ElView *v, int ni) {
    int a, e;
    day_range(ni, &a, &e);
    if (v->sel_t) for (int i = a; i < P.n; i++) if (P.h[i].t == v->sel_t) return i;
    return ni;
}
static void el_draw(Screen *s) {
    draw_background();
    draw_topbar(NULL);
    float x0 = 120;
    text_draw(F_BOLD, 52, x0, 130, C_TEXT, AL_LEFT, T("Electricity price"));
    ElView *v = s->st;
    if (draw_chips) {
        char g[96], f[64], tb[96];
        const char *gn = grid_name();
        if (*gn) snprintf(g, sizeof g, T("Grid company: %s"), gn);
        else snprintf(g, sizeof g, "%s", T("Choose grid company"));
        snprintf(f, sizeof f, T("Surcharge: %s"), T(fee_l[opt_at(fee_o, "el_fee", "0")]));
        snprintf(tb, sizeof tb, "%s: %s", T("Price on the home screen"), cfg_int("el_topbar", 1) ? T("On") : T("Off"));
        const char *chips[N_CH] = {area_name(area()), g, f, tb};
        draw_chips(x0, 196, chips, N_CH, v->on_chips ? v->sel : -1);
    } else {
        text_draw(F_REG, 26, x0, 196, C_TEXT3, AL_LEFT, area_name(area()));
    }
    int ni = now_idx();
    if (!P.ok || ni < 0) {
        if (fetching) draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT);
        else draw_empty(T("No prices right now"), T("The prices could not be fetched - check the network."));
        if (!draw_chips) draw_keybar(x0, UI_H - 62, NULL, NULL, T("Switch area"), NULL, NULL);
        return;
    }
    char b[64], k[16], line[400];
    /* now, or the hour chosen in the chart */
    int si = sel_idx(v, ni);
    fmt_kr(now_kr(si), b, sizeof b);
    Color c = price_color(si);
    fill_circle(x0 + 34, 352, 26, c);
    text_draw(F_BOLD, 150, x0 + 84, 270, C_TEXT, AL_LEFT, b);
    if (si == ni) snprintf(line, sizeof line, "%s", T("kr/kWh now"));
    else {
        char h1[8], h2[8];
        fmt_hour(P.h[si].t, h1, sizeof h1);
        fmt_hour(P.h[si].t + 3600, h2, sizeof h2);
        snprintf(line, sizeof line, T("kr/kWh  ·  %s %s-%s"), day_word(P.h[si].t), h1, h2);
    }
    text_draw(F_SEMI, 36, x0 + 94 + text_width(F_BOLD, 150, b), 318, C_TEXT2, AL_LEFT, line);
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
    float cy = 560, chh = 310, cleft = 56;
    draw_panel(x0 - 30, cy - 60, UI_W - 2 * x0 + 60, chh + 130, 28, 1);
    draw_chart(x0 + cleft, cy, UI_W - 2 * x0 - cleft, chh, a, P.n, true, si);
    text_draw(F_REG, 22, x0, cy - 54, C_TEXT3, AL_LEFT, T("Use the arrows to see the price hour by hour"));
    if (e >= P.n) text_draw(F_REG, 22, UI_W - x0, cy - 54, C_TEXT3, AL_RIGHT, T("Tomorrow's prices come at about 13:00"));
    if (with_tariffs()) {
        snprintf(line, sizeof line,
                 fee() > 0 ? T("Full price with VAT: spot price, grid tariff from %s, Energinet's tariffs, electricity tax and your supplier's surcharge")
                 : draw_chips ? T("Price with VAT: spot price, grid tariff from %s, Energinet's tariffs and electricity tax - add your supplier's surcharge at the top")
                              : T("Price with VAT: spot price, grid tariff from %s, Energinet's tariffs and electricity tax - your supplier's surcharge can be added in Settings"),
                 grid_name());
        text_draw(F_REG, 22, x0, UI_H - 70, C_TEXT3, AL_LEFT, line);
    } else {
        text_draw(F_REG, 22, x0, UI_H - 70, C_TEXT3, AL_LEFT,
                  grid() ? T("Spot price with VAT - the grid tariff is being fetched")
                  : draw_chips ? T("Spot price with VAT only - choose your grid company at the top to see the full price")
                               : T("Spot price with VAT only - choose your grid company in Settings to see the full price"));
    }
    if (!draw_chips) draw_keybar(x0, UI_H - 62, NULL, NULL, T("Switch area"), NULL, NULL);
}
static bool el_input(Screen *s, const InputEvent *ev) {
    ElView *v = s->st;
    if (ev->a == A_YELLOW && !ev->repeat) { switch_area(); return true; } /* shortcut */
    int ni = now_idx();
    if (!v->on_chips && (ev->a == A_LEFT || ev->a == A_RIGHT)) { /* the chart: one hour at a time */
        if (!P.ok || ni < 0) return true;
        int a, e, si = sel_idx(v, ni) + (ev->a == A_LEFT ? -1 : 1);
        day_range(ni, &a, &e);
        if (si >= a && si < P.n) v->sel_t = si == ni ? 0 : P.h[si].t;
        return true;
    }
    if (!draw_chips) {
        if (ev->a == A_OK && !ev->repeat) { switch_area(); return true; }
        return false;
    }
    if (ev->a == A_UP) { v->on_chips = true; return true; }
    if (ev->a == A_DOWN) { v->on_chips = false; return true; }
    if (!v->on_chips) {
        if (ev->a == A_OK && !ev->repeat) { v->sel_t = 0; return true; } /* back to now */
        return false;
    }
    if (ev->a == A_OK && !ev->repeat) { chip_open(v->sel); return true; }
    if (ev->a == A_LEFT || ev->a == A_RIGHT) { buttons_nav(&v->sel, N_CH, ev); return true; }
    return false;
}
static void el_destroy(Screen *s) {
    free(s->st);
    free(s);
}
static Screen *electricity_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "electricity";
    s->st = calloc(1, sizeof(ElView));
    s->enter = el_enter;
    s->update = el_update;
    s->draw = el_draw;
    s->input = el_input;
    s->destroy = el_destroy;
    return s;
}

/* ---------- the app ---------- */
static void electricity_forget(void) {
    el_gen++;
    fetching = false;
    memset(&P, 0, sizeof P);
    next_fetch = 0;
    { char p[512]; cache_path(p, sizeof p); remove(p); }
    cfg_set("el_area", "");
    tr_gen++;
    tr_fetching = tr_ok = false;
    tr_key[0] = 0;
    tr_next = 0;
    free(TR);
    TR = NULL;
    n_tr = 0;
    cfg_set("el_grid", "");
    cfg_set("el_fee", "");
    cfg_set("el_asked", "");
}
static const char *area_value(void) { return area_name(area()); }
static const char *const el_onoff_v[] = {"0", "1", NULL}, *const el_onoff_l[] = {N_("Off"), N_("On"), NULL};
static void el_init(void) {
    prices_load();
    if (app_set_overlay) app_set_overlay("electricity", el_overlay);
}
static const Item el_items[] = {
    {N_("Price area"), IT_ACTION, .action = switch_area, .value = area_value,
     .help = N_("West: Jutland and Funen. East: Zealand, the islands and Bornholm")},
    {N_("Grid company"), IT_CYCLE, "el_grid", "", grid_o, grid_l, .changed = grid_changed,
     .help = N_("The company that owns the cables to your home - it is on your electricity bill. Gives the full price")},
    {N_("Supplier's surcharge"), IT_CYCLE, "el_fee", "0", fee_o, fee_l, .changed = el_recalc,
     .help = N_("What your electricity company adds per kWh, with VAT - it is on your bill or in your contract")},
    {N_("Price on the home screen"), IT_CYCLE, "el_topbar", "1", el_onoff_v, el_onoff_l,
     .help = N_("The price right now at the top of the home screen")},
};
const NotaApp nota_app = {
    NOTA_API, "electricity", "Electricity price", N_("The electricity price hour by hour, today and tomorrow"), APP_VERSION, 78, 0,
    ICON_STAR, {34, 197, 94, 255}, {15, 95, 51, 255}, {255, 255, 255, 255},
    .open = electricity_screen, .init = el_init, .tick = el_tick, .forget = electricity_forget,
    .settings = el_items, .n_settings = sizeof el_items / sizeof el_items[0],
    .draw_shelf = el_shelf,
};
