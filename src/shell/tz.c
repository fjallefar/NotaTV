/* NotaTV shell - time zone: country and region are chosen manually (no online lookups)
 *
 * cfg: tz_country (e.g. "DK"), tz_zone (e.g. "Europe/Copenhagen"), tz_dst (1 = follow summer/winter time, 0 = fixed standard time)
 * - tvshell and everything it starts (mpv, scripts) get TZ in the environment.
 * - The rest of the system (e.g. `date` over SSH) follows /etc/localtime -> /data/notatv/localtime (symlink, set here).
 * Fixed standard time: the zone's POSIX rule without the DST part (last line of the TZif file, e.g. "CET-1CEST,M3..." -> "CET-1"). */
#include "tv.h"
#include "tzlist.h"
static bool tz_da(void) { return !strcmp(i18n_lang(), "da"); }
static const char *tz_cname(const TzCountry *c) { return tz_da() ? c->name_da : c->name_en; }
static const short *tz_order(void) { return tz_da() ? tz_order_da : tz_order_en; }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ZI "/usr/share/zoneinfo"

/* the POSIX rule from the TZif file's last line; std = only the standard time part. false if the file cannot be read */
static bool zone_rule(const char *zone, char *rule, size_t rn, char *std, size_t sn, bool *has_dst) {
    char path[300];
    snprintf(path, sizeof path, ZI "/%s", zone);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[8192];
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long off = sz > (long)sizeof buf - 1 ? sz - (long)sizeof buf + 1 : 0;
    fseek(f, off, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (n < 2 || buf[n - 1] != '\n') return false;
    buf[n - 1] = 0;
    /* the file is binary (NUL bytes): find the last newline from the end instead of strrchr */
    char *p = NULL;
    for (size_t i = n - 1; i-- > 0;) if (buf[i] == '\n') { p = buf + i + 1; break; }
    if (!p) return false;
    snprintf(rule, rn, "%s", p);
    /* name (<+01> or letters), then offset [+-]hh[:mm[:ss]] */
    const char *q = p;
    if (*q == '<') { q = strchr(q, '>'); if (!q) return false; q++; }
    else while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) q++;
    if (*q == '+' || *q == '-') q++;
    while ((*q >= '0' && *q <= '9') || *q == ':') q++;
    snprintf(std, sn, "%.*s", (int)(q - p), p);
    *has_dst = *q != 0;
    return *std != 0;
}

/* the standard time's offset from UTC in seconds (east positive), from the std part ("CET-1" -> +3600) */
static long std_offset(const char *std) {
    const char *q = std;
    if (*q == '<') { q = strchr(q, '>'); if (!q) return 0; q++; }
    else while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) q++;
    int sign = 1;
    if (*q == '+') q++;
    else if (*q == '-') { sign = -1; q++; }
    int h = 0, m = 0, s = 0;
    sscanf(q, "%d:%d:%d", &h, &m, &s);
    return -sign * (h * 3600L + m * 60 + s); /* POSIX: positiv = vest for Greenwich */
}

bool tz_zone_has_dst(const char *zone) {
    char rule[256], std[64];
    bool dst = false;
    return zone_rule(zone, rule, sizeof rule, std, sizeof std, &dst) && dst;
}

static const TzCountry *country_by_code(const char *code) {
    for (int i = 0; i < TZ_N_COUNTRIES; i++)
        if (!strcmp(tz_countries[i].code, code)) return &tz_countries[i];
    return NULL;
}

/* "Europe/Copenhagen" -> "Copenhagen", "America/Argentina/Buenos_Aires" -> "Buenos Aires" */
static void zone_city(const char *zone, char *out, size_t n) {
    const char *c = strrchr(zone, '/');
    snprintf(out, n, "%s", c ? c + 1 : zone);
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

static char g_label[200];
const char *tz_label(void) {
    const char *zone = cfg_get("tz_zone", "");
    if (!*zone) return T("Not selected (UTC)");
    const TzCountry *c = country_by_code(cfg_get("tz_country", ""));
    char city[96];
    zone_city(zone, city, sizeof city);
    if (c && c->n == 1) snprintf(g_label, sizeof g_label, "%s", tz_cname(c));
    else snprintf(g_label, sizeof g_label, "%s%s%s", c ? tz_cname(c) : "", c ? " · " : "", city);
    return g_label;
}

/* setenv("TZ") while other threads run is safe here only because main() creates TZ before any thread starts
   (glibc then replaces the string in place and never moves environ) */
void tz_apply(void) {
    const char *zone = cfg_get("tz_zone", "");
    char rule[256], std[64], env[320], link[320];
    bool has_dst = false;
    if (!*zone || strstr(zone, "..") || !zone_rule(zone, rule, sizeof rule, std, sizeof std, &has_dst)) {
        if (*zone) SDL_Log("tz: kan ikke læse tidszonen '%s' - bruger UTC", zone);
        setenv("TZ", "UTC0", 1);
        tzset();
        snprintf(link, sizeof link, ZI "/UTC");
    } else if (has_dst && !cfg_int("tz_dst", 1)) {
        /* fixed standard time all year */
        setenv("TZ", std, 1);
        tzset();
        long off = std_offset(std);
        if (off % 3600 == 0) snprintf(link, sizeof link, ZI "/Etc/GMT%s%ld", off > 0 ? "-" : "+", labs(off) / 3600); /* Etc signs are reversed */
        else snprintf(link, sizeof link, ZI "/%s", zone); /* e.g. +05:30 with DST does not exist as an Etc zone */
        if (off == 0) snprintf(link, sizeof link, ZI "/Etc/GMT");
    } else {
        snprintf(env, sizeof env, ":" ZI "/%s", zone);
        setenv("TZ", env, 1);
        tzset();
        snprintf(link, sizeof link, ZI "/%s", zone);
    }
    /* the system's /etc/localtime points here */
    char lp[300], tmp[310], cur[320];
    snprintf(lp, sizeof lp, "%s/localtime", data_dir());
    ssize_t cl = readlink(lp, cur, sizeof cur - 1);
    if (cl >= 0) cur[cl] = 0;
    if (cl < 0 || strcmp(cur, link)) {
        snprintf(tmp, sizeof tmp, "%s.new", lp);
        unlink(tmp);
        if (symlink(link, tmp) == 0 && rename(tmp, lp) == 0) SDL_Log("tz: %s (%s)", zone[0] ? zone : "UTC", getenv("TZ"));
        else unlink(tmp);
    }
}

void tz_init(void) { tz_apply(); }

/* ---------- choice: country -> region ---------- */
typedef struct { void (*done)(void *ud, bool ok); void *ud; int country; } TzPick;

static void zone_picked(void *ud, int idx) {
    TzPick *p = ud;
    const TzCountry *c = &tz_countries[p->country];
    if (idx >= 0 && idx < c->n) {
        cfg_set("tz_country", c->code);
        cfg_set("tz_zone", c->z[idx].zone);
        tz_apply();
    }
    if (p->done) p->done(p->ud, idx >= 0);
    free(p);
}

static void open_zones(TzPick *p) {
    const TzCountry *c = &tz_countries[p->country];
    if (c->n == 1) { zone_picked(p, 0); return; }
    char **items = calloc(c->n, sizeof *items);
    int sel = 0;
    const char *cur = cfg_get("tz_zone", "");
    for (int i = 0; i < c->n; i++) {
        char city[96];
        zone_city(c->z[i].zone, city, sizeof city);
        items[i] = c->z[i].note[0] ? str_printf("%s  (%s)", city, c->z[i].note) : str_dup(city);
        if (!strcmp(cur, c->z[i].zone)) sel = i;
    }
    char t[160];
    snprintf(t, sizeof t, T("Region in %s"), tz_cname(c));
    choice_open(t, (const char *const *)items, c->n, sel, zone_picked, p);
    for (int i = 0; i < c->n; i++) free(items[i]);
    free(items);
}

static void country_picked(void *ud, int idx) {
    TzPick *p = ud;
    if (idx < 0) { if (p->done) p->done(p->ud, false); free(p); return; }
    p->country = tz_order()[idx];
    open_zones(p);
}

void tz_pick(void (*done)(void *ud, bool ok), void *ud) {
    TzPick *p = calloc(1, sizeof *p);
    p->done = done;
    p->ud = ud;
    const char *names[TZ_N_COUNTRIES];
    int sel = 0;
    const char *cur = cfg_get("tz_country", "DK");
    const short *ord = tz_order();
    for (int i = 0; i < TZ_N_COUNTRIES; i++) {
        names[i] = tz_cname(&tz_countries[ord[i]]);
        if (!strcmp(cur, tz_countries[ord[i]].code)) sel = i;
    }
    choice_open(T("Choose country"), names, TZ_N_COUNTRIES, sel, country_picked, p);
}

/* country name in the UI language from the ISO code (e.g. "DK" -> "Denmark"/"Danmark"); unknown = the code itself */
const char *tz_country_name(const char *code) {
    for (int i = 0; i < TZ_N_COUNTRIES; i++) if (!strcmp(tz_countries[i].code, code)) return tz_cname(&tz_countries[i]);
    return code;
}

/* Danish keyboard layout (on-screen keyboard and physical keyboards): when the UI is Danish, or the
 * country is Denmark, Greenland or the Faroe Islands - someone in Denmark with English menus most likely
 * still has a Danish keyboard. Read on every key, so a change of language/country applies at once. */
bool kb_layout_da(void) {
    const char *c = cfg_get("tz_country", "");
    return tz_da() || !strcmp(c, "DK") || !strcmp(c, "GL") || !strcmp(c, "FO");
}
