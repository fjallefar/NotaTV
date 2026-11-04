/* NotaTV app package "iptv" - Live TV (IPTV): M3U/Xtream, groups, favorites, EPG (XMLTV) */
#include "iptv.h"
#include "vendor/cJSON.h"
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define IPTV_UA "VLC/3.0.21 LibVLC/3.0.21"

typedef struct { time_t start, stop; char *title; } Prog;
typedef struct {
    char *name, *url, *logo, *group, *tvgid;
    char *disp;       /* nice name for the screen: "DNK: 6`EREN [H265]" -> "6'EREN" */
    int num;
    int sid;          /* Xtream stream id from /live/<user>/<pass>/<id>.ts (0 = none) */
    double epg_t;     /* last fetched via get_short_epg */
    bool epg_busy;
    Prog *progs;
    int n_progs, cap_progs;
} Channel;

typedef struct {
    Channel *ch;
    int n_ch;
    char **groups;
    int n_groups;
    char *epg_url;
    bool loading, epg_loading;
    bool short_epg;   /* Xtream: program data is fetched per channel when needed (no big XMLTV file) */
    int sepg_busy;
    int sepg_ok, sepg_fail; /* get_short_epg replies with data / failed (the provider's guide may be down) */
    char epg_msg[160];
    int *gcount;      /* number of channels per group */
    char **countries; /* country codes from group names ("DK: SPORT" -> "DK") */
    int n_countries, *ccount;
    bool *cexp;       /* country expanded */
    int *gcountry;    /* group -> country (-1 = none / no headings) */
    int *ui, n_ui;    /* left column after Favorites/All: >= 0 group, < 0 country -(i+1) */
    int *vis, n_vis;  /* visible entries in the ui (groups in collapsed countries are hidden) */
    char status[256];
    int gen; /* counted up on every new load */
} IptvData;

static IptvData D;
static char **favs;
static int n_favs;

/* the providers' channel names are full of technical tags - remove the country code prefix, [..] tags and
   quality suffixes for display (search, EPG matching etc. still use the raw name) */
static char *clean_name(const char *in) {
    const char *p = in;
    int k = 0;
    while (k < 6 && ((p[k] >= 'A' && p[k] <= 'Z') || (p[k] >= '0' && p[k] <= '9'))) k++;
    if (k >= 2 && (p[k] == ':' || p[k] == '|')) { p += k + 1; }
    else if (k >= 2 && p[k] == ' ' && (p[k + 1] == '|' || p[k + 1] == '-') && p[k + 2] == ' ') p += k + 3;
    char *out = malloc(strlen(p) + 1), *w = out;
    int depth = 0;
    for (; *p; p++) {
        if (*p == '[') { depth++; continue; }
        if (*p == ']') { if (depth) depth--; continue; }
        if (depth) continue;
        if (*p == '`') { *w++ = '\''; continue; }
        if (*p == ' ' && (w == out || w[-1] == ' ')) continue; /* double/leading spaces */
        *w++ = *p;
    }
    while (w > out && w[-1] == ' ') w--;
    *w = 0;
    static const char *suf[] = {" FHD", " UHD", " HD", " SD", " 4K", " H265", " HEVC", " RAW", " 1080P", " 720P", NULL};
    for (int again = 1; again;) {
        again = 0;
        size_t n = strlen(out);
        for (int i = 0; suf[i]; i++) {
            size_t l = strlen(suf[i]);
            if (n > l + 1 && !strcasecmp(out + n - l, suf[i])) { out[n - l] = 0; again = 1; break; }
        }
    }
    if (!*out) { free(out); return strdup(in); }
    return out;
}
#define DNAME(c) ((c)->disp ? (c)->disp : (c)->name)

/* ---------- favorites / recent ---------- */
static void favs_load(void) {
    for (int i = 0; i < n_favs; i++) free(favs[i]);
    free(favs);
    favs = NULL;
    n_favs = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/iptv-fav.txt", data_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        str_trim(line);
        if (!line[0]) continue;
        favs = realloc(favs, sizeof(char *) * (n_favs + 1));
        favs[n_favs++] = strdup(line);
    }
    fclose(f);
}

static void favs_save(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/iptv-fav.txt", data_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n_favs; i++) fprintf(f, "%s\n", favs[i]);
    fclose(f);
}

static bool is_fav(const char *url) {
    for (int i = 0; i < n_favs; i++)
        if (!strcmp(favs[i], url)) return true;
    return false;
}

static bool toggle_fav(const char *url) {
    for (int i = 0; i < n_favs; i++)
        if (!strcmp(favs[i], url)) {
            free(favs[i]);
            memmove(favs + i, favs + i + 1, sizeof(char *) * (n_favs - i - 1));
            n_favs--;
            favs_save();
            return false;
        }
    favs = realloc(favs, sizeof(char *) * (n_favs + 1));
    favs[n_favs++] = strdup(url);
    favs_save();
    return true;
}

/* channel logos: cleaned (no white box needed) on a NotaTV that can, otherwise as they come on a light card */
static Image *chan_logo(const char *url) { return img_get_logo ? img_get_logo(url) : img_get(url); }
static bool logo_card(void) { return !img_get_logo; }

typedef struct { char *name, *url, *logo; } Recent;
static Recent recents[12];
static int n_recent;
static HomeItem recent_items[12];

static void recent_load(void) {
    for (int i = 0; i < n_recent; i++) { free(recents[i].name); free(recents[i].url); free(recents[i].logo); }
    n_recent = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/iptv-recent.txt", data_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[8192];
    while (n_recent < 8 && fgets(line, sizeof line, f)) {
        str_trim(line);
        char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "\t");
        if (!a || !b) continue;
        recents[n_recent].name = strdup(a);
        recents[n_recent].url = strdup(b);
        recents[n_recent].logo = c && strcmp(c, "-") ? strdup(c) : NULL;
        n_recent++;
    }
    fclose(f);
}

static void recent_add(const Channel *c) {
    recent_load();
    char path[512];
    snprintf(path, sizeof path, "%s/iptv-recent.txt", data_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\t%s\t%s\n", DNAME(c), c->url, c->logo && *c->logo ? c->logo : "-");
    int w = 1;
    for (int i = 0; i < n_recent && w < 8; i++) {
        if (!strcmp(recents[i].url, c->url)) continue;
        fprintf(f, "%s\t%s\t%s\n", recents[i].name, recents[i].url, recents[i].logo ? recents[i].logo : "-");
        w++;
    }
    fclose(f);
    recent_load();
}

static char *recent_clean[12], *recent_clean_src[12];
int iptv_recent(HomeItem **items) {
    recent_load();
    for (int i = 0; i < n_recent; i++) {
        recent_items[i] = (HomeItem){recents[i].name, "Live TV", recents[i].logo, NULL, 0};
        if (!recent_clean[i] || strcmp(recent_clean_src[i] ? recent_clean_src[i] : "", recents[i].name)) {
            free(recent_clean[i]); free(recent_clean_src[i]);
            recent_clean[i] = clean_name(recents[i].name); /* older lines have the raw name */
            recent_clean_src[i] = strdup(recents[i].name);
        }
        recent_items[i].title = recent_clean[i];
        static char keys[12][16];
        snprintf(keys[i], sizeof keys[i], "iptv:%d", i);
        recent_items[i].key = keys[i];
    }
    *items = recent_items;
    return n_recent;
}

/* ---------- M3U ---------- */
static char *attr(const char *line, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "%s=\"", key);
    const char *p = strstr(line, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return NULL;
    return strndup(p, e - p);
}

typedef struct {
    Channel *ch;
    int n, cap;
    char **groups;
    int n_groups, cap_groups;
    int *ghash, ghash_cap; /* group name -> index + 1 (open addressing; 0 = empty) */
    char *epg;
    char err[200];
} ParseOut;

static int group_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static unsigned long str_hash(const char *s) {
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}
/* (a hash, not a search through all groups for every channel: 20,000 channels x 1,000 groups was 20 million
   string compares on every load) */
static int group_index(ParseOut *o, const char *g) {
    if (o->ghash_cap && o->n_groups * 2 < o->ghash_cap) {
        for (unsigned long j = str_hash(g) & (o->ghash_cap - 1); o->ghash[j]; j = (j + 1) & (o->ghash_cap - 1))
            if (!strcmp(o->groups[o->ghash[j] - 1], g)) return o->ghash[j] - 1;
    } else { /* (re)build the hash twice as big, then look again */
        int ncap = o->ghash_cap ? o->ghash_cap * 2 : 256;
        int *nh = calloc((size_t)ncap, sizeof(int));
        if (nh) {
            for (int i = 0; i < o->n_groups; i++) {
                unsigned long j = str_hash(o->groups[i]) & (ncap - 1);
                while (nh[j]) j = (j + 1) & (ncap - 1);
                nh[j] = i + 1;
            }
            free(o->ghash);
            o->ghash = nh;
            o->ghash_cap = ncap;
            return group_index(o, g);
        }
        for (int i = 0; i < o->n_groups; i++) if (!strcmp(o->groups[i], g)) return i;
    }
    if (o->n_groups == o->cap_groups) {
        o->cap_groups = o->cap_groups ? o->cap_groups * 2 : 64;
        o->groups = realloc(o->groups, sizeof(char *) * (size_t)o->cap_groups);
    }
    o->groups[o->n_groups] = strdup(g);
    if (o->ghash_cap) {
        unsigned long j = str_hash(g) & (o->ghash_cap - 1);
        while (o->ghash[j]) j = (j + 1) & (o->ghash_cap - 1);
        o->ghash[j] = o->n_groups + 1;
    }
    return o->n_groups++;
}

static void parse_m3u(const char *path, ParseOut *o) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(o->err, sizeof o->err, T("Could not read the playlist")); return; }
    size_t cap = 1 << 16;
    char *line = malloc(cap);
    char *pending = NULL, *grp = NULL;
    bool first = true;
    while (fgets(line, (int)cap, f)) {
        str_trim(line);
        if (first) {
            first = false;
            if (strncmp(line, "#EXTM3U", 7) == 0) {
                char *u = attr(line, "url-tvg");
                if (!u) u = attr(line, "x-tvg-url");
                if (u) { char *c = strchr(u, ','); if (c) *c = 0; o->epg = u; }
                continue;
            }
        }
        if (!line[0]) continue;
        if (strncmp(line, "#EXTINF", 7) == 0) { free(pending); pending = strdup(line); continue; }
        if (strncmp(line, "#EXTGRP:", 8) == 0) { free(grp); grp = strdup(line + 8); continue; }
        if (line[0] == '#') continue;
        /* URL line. Xtream puts movies and series in the same playlist - we only show live channels */
        if (strstr(line, "/movie/") || strstr(line, "/series/")) { free(pending); pending = NULL; continue; }
        if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 256; o->ch = realloc(o->ch, sizeof(Channel) * o->cap); }
        Channel *c = &o->ch[o->n];
        memset(c, 0, sizeof *c);
        c->url = strdup(line);
        const char *lv = strstr(line, "/live/");
        if (lv) { const char *sl = strrchr(lv, '/'); c->sid = sl ? atoi(sl + 1) : 0; }
        if (pending) {
            /* the name comes after the last comma that is not inside an attribute */
            const char *p = pending;
            bool inq = false;
            const char *comma = NULL;
            for (; *p; p++) {
                if (*p == '"') inq = !inq;
                else if (*p == ',' && !inq) comma = p;
            }
            c->name = strdup(comma ? comma + 1 : T("Channel"));
            str_trim(c->name);
            c->logo = attr(pending, "tvg-logo");
            c->tvgid = attr(pending, "tvg-id");
            c->group = attr(pending, "group-title");
            char *chno = attr(pending, "tvg-chno");
            if (chno) { c->num = atoi(chno); free(chno); }
            if (!c->name[0]) { free(c->name); c->name = attr(pending, "tvg-name"); }
        }
        if (!c->name) c->name = strdup(T("Channel"));
        c->disp = clean_name(c->name);
        if (!c->group || !c->group[0]) { free(c->group); c->group = strdup(grp ? grp : T("Other channels")); }
        group_index(o, c->group);
        o->n++;
        free(pending);
        pending = NULL;
    }
    free(line);
    free(pending);
    free(grp);
    fclose(f);
    for (int i = 0; i < o->n; i++)
        if (!o->ch[i].num) o->ch[i].num = i + 1;
    qsort(o->groups, o->n_groups, sizeof(char *), group_cmp);
    free(o->ghash);
    o->ghash = NULL;
    o->ghash_cap = 0;
    if (o->n == 0) snprintf(o->err, sizeof o->err, T("No channels found in the playlist"));
}

static char **gsort_names;
static int gidx_cmp(const void *a, const void *b) { return strcmp(gsort_names[*(const int *)a], gsort_names[*(const int *)b]); }

static void free_channels(Channel *ch, int n) {
    for (int i = 0; i < n; i++) {
        free(ch[i].name); free(ch[i].disp); free(ch[i].url); free(ch[i].logo); free(ch[i].group); free(ch[i].tvgid);
        for (int k = 0; k < ch[i].n_progs; k++) free(ch[i].progs[k].title);
        free(ch[i].progs);
    }
    free(ch);
}

/* "host:8080/", "http://host:8080/get.php?..." -> "http://host:8080" */
static char *xtream_base(const char *srv) {
    char *t = str_dup(srv);
    str_trim(t);
    char *q = strchr(t, '?');
    if (q) *q = 0;
    const char *known[] = {"/get.php", "/player_api.php", "/xmltv.php", "/c/", NULL};
    for (int i = 0; known[i]; i++) {
        char *p = strstr(t, known[i]);
        if (p) *p = 0;
    }
    size_t n = strlen(t);
    while (n && t[n - 1] == '/') t[--n] = 0;
    char *r = strstr(t, "://") ? str_dup(t) : str_printf("http://%s", t);
    free(t);
    return r;
}

static char *xtream_url_for(const char *srv, const char *us, const char *pw, const char *script, const char *extra) {
    if (!*srv || !*us) return NULL;
    char *base = xtream_base(srv), *eu = url_encode(us), *ep = url_encode(pw);
    char *r = str_printf("%s/%s?username=%s&password=%s%s", base, script, eu, ep, extra ? extra : "");
    free(base); free(eu); free(ep);
    return r;
}
static char *xtream_url(const char *script, const char *extra) {
    return xtream_url_for(cfg_get("xtream_server", ""), cfg_get("xtream_user", ""), cfg_get("xtream_pass", ""), script, extra);
}

static char *playlist_url(void) {
    const char *u = cfg_get("iptv_url", "");
    if (*u) return str_dup(u);
    return xtream_url("get.php", "&type=m3u_plus&output=ts");
}

/* is there a playlist? (cheap - called every frame; playlist_url() builds and URL-encodes the whole address) */
static bool iptv_configured(void) {
    return *cfg_get("iptv_url", "") || (*cfg_get("xtream_server", "") && *cfg_get("xtream_user", ""));
}

static char *epg_url_cfg(void) {
    const char *u = cfg_get("iptv_epg", "");
    if (*u) return str_dup(u);
    const char *pl = cfg_get("iptv_url", "");
    if (!*pl) return xtream_url("xmltv.php", NULL);
    /* M3U link from an Xtream provider (…/get.php?username=…&password=…): the guide is at xmltv.php */
    const char *g = strstr(pl, "/get.php?");
    if (g) {
        const char *us = strstr(g, "username="), *pw = strstr(g, "password=");
        if (us && pw) {
            int lu = (int)strcspn(us + 9, "&"), lp = (int)strcspn(pw + 9, "&");
            return str_printf("%.*s/xmltv.php?username=%.*s&password=%.*s", (int)(g - pl), pl, lu, us + 9, lp, pw + 9);
        }
    }
    return NULL;
}

typedef struct { char *url; bool force; ParseOut out; int epoch; } LoadJob;
static int iptv_epoch; /* raised when the account is signed out: a playlist that is still loading is thrown away */

static void load_work(void *arg) {
    LoadJob *j = arg;
    char path[512];
    snprintf(path, sizeof path, "%s/iptv.m3u", data_dir());
    struct stat st;
    bool have = stat(path, &st) == 0 && st.st_size > 0;
    char urlf[512];
    snprintf(urlf, sizeof urlf, "%s/iptv.url", data_dir());
    /* fetch again if the URL changed, forced or older than 12 hours */
    char old[4096] = "";
    FILE *uf = fopen(urlf, "r");
    if (uf) { if (!fgets(old, sizeof old, uf)) old[0] = 0; fclose(uf); str_trim(old); }
    bool stale = !have || j->force || strcmp(old, j->url) || time(NULL) - st.st_mtime > 12 * 3600;
    if (stale) {
        if (http_download_ua(j->url, path, 180, IPTV_UA)) {
            uf = fopen(urlf, "w");
            if (uf) { fprintf(uf, "%s\n", j->url); fclose(uf); }
        } else if (!have || strcmp(old, j->url)) {
            snprintf(j->out.err, sizeof j->out.err, T("Could not download the playlist. Check the URL and network."));
            return;
        }
    }
    parse_m3u(path, &j->out);
}

static void epg_start(void);

/* country code in front of the group name: "DK: TV2 PLAY" -> 2 (the length of "DK"), otherwise 0 */
static int country_len(const char *g) {
    int n = 0;
    while (g[n] >= 'A' && g[n] <= 'Z') n++;
    return n >= 2 && n <= 6 && g[n] == ':' ? n : 0;
}

static const char *country_name(const char *code) {
    static const char *m[][2] = {
        {"DK", N_("Denmark")}, {"SE", N_("Sweden")}, {"NO", N_("Norway")}, {"FI", N_("Finland")}, {"UK", N_("United Kingdom")},
        {"EN", N_("English")}, {"US", N_("United States")}, {"DE", N_("Germany")}, {"NL", N_("Netherlands")}, {"FR", N_("France")},
        {"IT", N_("Italy")}, {"ES", N_("Spain")}, {"PT", "Portugal"}, {"PL", N_("Poland")}, {"TR", N_("Turkey")},
        {"RU", N_("Russia")}, {"ARA", N_("Arabic")}, {"AR", N_("Arabic")}, {"IN", N_("India")}, {"GLOBAL", N_("Global")},
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!strcmp(code, m[i][0])) return T(m[i][1]);
    return code;
}

/* order: countries in cfg iptv_countries (default DK) first, then the rest alphabetically, groups without a country last */
static int country_rank(const char *code) {
    if (!code) return 1 << 20;
    char *pref = str_dup(cfg_get("iptv_countries", "DK")), *save = NULL;
    int r = 1 << 10, i = 0;
    for (char *t = strtok_r(pref, ", ", &save); t; t = strtok_r(NULL, ", ", &save), i++)
        if (!strcasecmp(t, code)) { r = i; break; }
    free(pref);
    return r;
}

static int group_sort_cmp(const void *a, const void *b) {
    const char *ga = *(char *const *)a, *gb = *(char *const *)b;
    int la = country_len(ga), lb = country_len(gb);
    char ca[8] = "", cb[8] = "";
    memcpy(ca, ga, la); memcpy(cb, gb, lb);
    int ra = country_rank(la ? ca : NULL), rb = country_rank(lb ? cb : NULL);
    if (ra != rb) return ra < rb ? -1 : 1;
    int c = strcasecmp(country_name(ca), country_name(cb));
    return c ? c : strcasecmp(ga + la, gb + lb);
}

static int country_index(const char *code) {
    for (int i = 0; i < D.n_countries; i++)
        if (!strcmp(D.countries[i], code)) return i;
    return -1;
}

static bool in_country(const Channel *c, int ci) {
    const char *code = D.countries[ci];
    size_t n = strlen(code);
    return !strncmp(c->group, code, n) && c->group[n] == ':';
}

static void build_countries(void) {
    for (int i = 0; i < D.n_countries; i++) free(D.countries[i]);
    free(D.countries); free(D.ccount); free(D.cexp); free(D.gcountry); free(D.ui); free(D.vis);
    D.countries = NULL; D.n_countries = 0;
    qsort(D.groups, D.n_groups, sizeof(char *), group_sort_cmp);
    D.ui = malloc(sizeof(int) * (D.n_groups * 2 + 1));
    D.n_ui = 0;
    for (int g = 0; g < D.n_groups; g++) {
        int l = country_len(D.groups[g]);
        if (l) {
            char code[8] = "";
            memcpy(code, D.groups[g], l);
            if (country_index(code) < 0) {
                D.countries = realloc(D.countries, sizeof(char *) * (D.n_countries + 1));
                D.countries[D.n_countries] = str_dup(code);
                D.ui[D.n_ui++] = -(++D.n_countries);
            }
        }
        D.ui[D.n_ui++] = g;
    }
    /* only one country (or none): no headings */
    if (D.n_countries < 2) {
        D.n_ui = 0;
        for (int g = 0; g < D.n_groups; g++) D.ui[D.n_ui++] = g;
    }
    D.ccount = calloc(D.n_countries + 1, sizeof(int));
    D.cexp = calloc(D.n_countries + 1, sizeof(bool));
    D.gcountry = malloc(sizeof(int) * (D.n_groups + 1));
    for (int g = 0; g < D.n_groups; g++) D.gcountry[g] = -1;
    for (int ci = 0; D.n_countries >= 2 && ci < D.n_countries; ci++) {
        for (int c = 0; c < D.n_ch; c++) if (in_country(&D.ch[c], ci)) D.ccount[ci]++;
        size_t n = strlen(D.countries[ci]);
        for (int g = 0; g < D.n_groups; g++)
            if (!strncmp(D.groups[g], D.countries[ci], n) && D.groups[g][n] == ':') D.gcountry[g] = ci;
    }
    D.vis = malloc(sizeof(int) * (D.n_ui + 1));
}

static void rebuild_vis(void) {
    D.n_vis = 0;
    for (int i = 0; i < D.n_ui; i++) {
        int g = D.ui[i];
        if (g >= 0 && D.gcountry[g] >= 0 && !D.cexp[D.gcountry[g]]) continue;
        D.vis[D.n_vis++] = i;
    }
}

static void iptv_after_load(void);
static void load_done(void *arg) {
    LoadJob *j = arg;
    D.loading = false;
    if (j->epoch != iptv_epoch) { /* signed out meanwhile */
        free_channels(j->out.ch, j->out.n);
        for (int i = 0; i < j->out.n_groups; i++) free(j->out.groups[i]);
        free(j->out.groups);
        free(j->out.epg);
        free(j->url);
        free(j);
        return;
    }
    if (j->out.err[0] && j->out.n == 0) {
        snprintf(D.status, sizeof D.status, "%s", j->out.err);
    } else {
        free_channels(D.ch, D.n_ch);
        for (int i = 0; i < D.n_groups; i++) free(D.groups[i]);
        free(D.groups);
        D.ch = j->out.ch;
        D.n_ch = j->out.n;
        D.groups = j->out.groups;
        D.n_groups = j->out.n_groups;
        free(D.epg_url);
        char *cfg_epg = epg_url_cfg();
        D.epg_url = cfg_epg ? cfg_epg : j->out.epg;
        if (cfg_epg) free(j->out.epg);
        D.status[0] = 0;
        D.gen++;
        build_countries();
        rebuild_vis();
        iptv_after_load();
        int with_sid = 0;
        for (int i = 0; i < D.n_ch; i++) with_sid += D.ch[i].sid > 0;
        D.short_epg = !*cfg_get("iptv_epg", "") && with_sid > D.n_ch / 2;
        D.sepg_ok = D.sepg_fail = 0;
        free(D.gcount);
        D.gcount = calloc(D.n_groups + 1, sizeof(int));
        { /* channels per group: one sorted copy of the group names and a binary search per channel */
            int *ord = malloc(sizeof(int) * (size_t)(D.n_groups + 1));
            for (int g = 0; g < D.n_groups; g++) ord[g] = g;
            gsort_names = D.groups;
            qsort(ord, (size_t)D.n_groups, sizeof(int), gidx_cmp);
            for (int c = 0; c < D.n_ch; c++) {
                int lo = 0, hi = D.n_groups - 1;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2, r = strcmp(D.ch[c].group, D.groups[ord[mid]]);
                    if (!r) { D.gcount[ord[mid]]++; break; }
                    if (r < 0) hi = mid - 1; else lo = mid + 1;
                }
            }
            free(ord);
        }
        epg_start();
    }
    free(j->url);
    free(j);
}

static void iptv_load(bool force) {
    char *url = playlist_url();
    if (!url) return;
    if (D.loading) { free(url); return; }
    D.loading = true;
    snprintf(D.status, sizeof D.status, T("Loading channels…"));
    LoadJob *j = calloc(1, sizeof *j);
    j->url = url;
    j->force = force;
    j->epoch = iptv_epoch;
    job_submit_q(JQ_SLOW, load_work, load_done, j);
}

/* ---------- EPG (XMLTV) ---------- */
/* limits: a big XMLTV guide must not exhaust memory on the Pi */
#define EPG_MAX_PER_CH 300        /* programmes kept per channel (the ones nearest to now) */
#define EPG_MAX_TOTAL 200000      /* total programmes: parsing stops adding at this point */
#define EPG_PAST (3 * 3600)       /* drop programmes that ended more than 3 h ago */
#define EPG_AHEAD (24 * 3600)     /* ... or start more than 24 h ahead (as before) */

typedef struct { char *id; int idx; } IdMap;
static int idmap_cmp(const void *a, const void *b) { return strcmp(((const IdMap *)a)->id, ((const IdMap *)b)->id); }

typedef struct {
    char *url;
    int gen;
    Channel *ch; /* copy of pointers to channel ids (read only) */
    int n_ch;
    IdMap *map;
    int n_map, cap_map;
    int n_sorted;     /* map[0..n_sorted) is sorted; what <channel> elements add later is merged in when needed */
    char **names;     /* normalized channel names (name matching when the tvg-id does not fit) */
    char **tvgids;
    int *by_id, n_by_id; /* channel indexes sorted by tvg-id / by normalized name, for binary search */
    int *by_name;
    bool *exact;      /* the channel's tvg-id exists in the guide */
    /* results per channel */
    Prog **progs;
    int *n_progs, *cap_progs; /* capacity: arrays are doubled (before: realloc for every program) */
    bool ok, downloaded;
    int total;
    unsigned long names_hash; /* the channel list: the cache only fits the same list */
} EpgJob;

static char *xml_attr(const char *tag, const char *end, const char *key);
static void xml_unescape(char *s);

/* "DK | TV 2 HD" -> "tv2", "TV2.dk" -> "tv2dk" */
static char *norm_name(const char *in) {
    const char *p = in;
    /* country code prefix: up to 4 letters followed by | : - */
    int k = 0;
    while (k < 4 && isalpha((unsigned char)p[k])) k++;
    const char *q = p + k;
    while (*q == ' ') q++;
    if (k > 0 && (*q == '|' || *q == ':' || *q == '-')) { p = q + 1; }
    char *out = malloc(strlen(p) + 1), *w = out;
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x80) *w++ = (char)c;                 /* keep e.g. Arabic/æøå */
        else if (isalnum(c)) *w++ = (char)tolower(c);
    }
    *w = 0;
    static const char *suf[] = {"fhd", "uhd", "hd", "sd", "4k", "hevc", "h265", "raw", NULL};
    for (int again = 1; again;) {
        again = 0;
        size_t n = strlen(out);
        for (int i = 0; suf[i]; i++) {
            size_t l = strlen(suf[i]);
            if (n > l + 1 && !strcmp(out + n - l, suf[i])) { out[n - l] = 0; again = 1; break; }
        }
    }
    return out;
}

static void map_add(EpgJob *j, const char *id, int idx) {
    if (j->n_map == j->cap_map) { j->cap_map = j->cap_map ? j->cap_map * 2 : 64; j->map = realloc(j->map, sizeof(IdMap) * j->cap_map); }
    j->map[j->n_map++] = (IdMap){strdup(id), idx};
}

/* the entries added since the last sort are sorted and merged into the sorted part (not the whole map again) */
static void map_sort(EpgJob *j) {
    if (j->n_sorted >= j->n_map) return;
    int a = j->n_sorted, n = j->n_map;
    qsort(j->map + a, (size_t)(n - a), sizeof(IdMap), idmap_cmp);
    IdMap *m = malloc(sizeof(IdMap) * (size_t)n);
    if (!m) { qsort(j->map, (size_t)n, sizeof(IdMap), idmap_cmp); j->n_sorted = n; return; }
    int x = 0, y = a, k = 0;
    while (x < a && y < n) m[k++] = idmap_cmp(&j->map[x], &j->map[y]) <= 0 ? j->map[x++] : j->map[y++];
    while (x < a) m[k++] = j->map[x++];
    while (y < n) m[k++] = j->map[y++];
    memcpy(j->map, m, sizeof(IdMap) * (size_t)n);
    free(m);
    j->n_sorted = n;
}

/* first position in idx[0..n) (sorted by key[idx]) whose key is >= s */
static int lower_bound(char **key, const int *idx, int n, const char *s) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(key[idx[mid]], s) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* <channel id="X"><display-name>Name</display-name>... */
static void epg_channel(EpgJob *j, const char *el, const char *el_end) {
    const char *tag_end = strchr(el, '>');
    if (!tag_end || tag_end > el_end) return;
    char *id = xml_attr(el, tag_end, "id");
    if (!id) return;
    /* binary searches (was: every channel for every <channel> and every display-name - minutes on the Pi) */
    for (int k = lower_bound(j->tvgids, j->by_id, j->n_by_id, id); k < j->n_by_id && !strcmp(j->tvgids[j->by_id[k]], id); k++)
        j->exact[j->by_id[k]] = true;
    for (const char *d = strstr(tag_end, "<display-name"); d && d < el_end; d = strstr(d + 1, "<display-name")) {
        const char *t = strchr(d, '>');
        if (!t || t > el_end) break;
        const char *te = strstr(t, "</display-name>");
        if (!te || te > el_end) break;
        char *nm = strndup(t + 1, te - t - 1);
        xml_unescape(nm);
        char *n = norm_name(nm);
        free(nm);
        if (*n)
            for (int k = lower_bound(j->names, j->by_name, j->n_ch, n); k < j->n_ch && !strcmp(j->names[j->by_name[k]], n); k++) {
                int i = j->by_name[k];
                if (!j->exact[i] && (!j->tvgids[i] || strcmp(j->tvgids[i], id))) map_add(j, id, i);
            }
        free(n);
    }
    free(id);
}

static time_t parse_xmltv_time(const char *s) {
    struct tm tm = {0};
    if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 5) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    time_t t = timegm(&tm);
    const char *z = strchr(s, ' ');
    if (z) {
        int sign = 1, hh = 0, mm = 0;
        z++;
        if (*z == '-') sign = -1;
        if (*z == '+' || *z == '-') z++;
        if (sscanf(z, "%2d%2d", &hh, &mm) >= 1) t -= sign * (hh * 3600 + mm * 60);
    }
    return t;
}

/* distance from now to a programme: 0 while it is on */
static time_t prog_dist(time_t a, time_t b, time_t now) {
    if (now < a) return a - now;
    if (now > b) return now - b;
    return 0;
}

static void xml_unescape(char *s) {
    char *w = s;
    for (char *r = s; *r;) {
        if (*r == '&') {
            if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; continue; }
            if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; continue; }
            if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; continue; }
            if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; continue; }
            if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = 0;
}

static char *xml_attr(const char *tag, const char *end, const char *key) {
    char pat[32];
    snprintf(pat, sizeof pat, " %s=\"", key);
    const char *p = strstr(tag, pat);
    if (!p || p > end) return NULL;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e || e > end) return NULL;
    return strndup(p, e - p);
}

static void epg_handle(EpgJob *j, const char *el, const char *el_end, time_t lo, time_t hi, time_t now) {
    map_sort(j);
    const char *tag_end = strchr(el, '>');
    if (!tag_end || tag_end > el_end) return;
    char *chid = xml_attr(el, tag_end, "channel");
    if (!chid) return;
    IdMap key = {chid, 0}, *m = bsearch(&key, j->map, j->n_map, sizeof(IdMap), idmap_cmp);
    if (!m) { free(chid); return; }
    char *st = xml_attr(el, tag_end, "start"), *sp = xml_attr(el, tag_end, "stop");
    time_t a = st ? parse_xmltv_time(st) : 0, b = sp ? parse_xmltv_time(sp) : 0;
    free(st); free(sp); free(chid);
    if (!a || !b || b < lo || a > hi) return;
    const char *t = strstr(tag_end, "<title");
    if (!t || t > el_end) return;
    t = strchr(t, '>');
    if (!t) return;
    t++;
    const char *te = strstr(t, "</title>");
    if (!te || te > el_end) return;
    char *title = strndup(t, te - t);
    xml_unescape(title);
    time_t nd = prog_dist(a, b, now);
    /* the same tvg-id can be used by several channels: m points to the first; go through all */
    int first = (int)(m - j->map);
    while (first > 0 && !strcmp(j->map[first - 1].id, m->id)) first--;
    bool used = false;
    for (int k = first; k < j->n_map && !strcmp(j->map[k].id, m->id); k++) {
        int ci = j->map[k].idx;
        /* name matching is only used if the channel's own tvg-id is not in the guide */
        if (j->exact[ci] && (!j->tvgids[ci] || strcmp(j->tvgids[ci], m->id))) continue;
        if (j->total >= EPG_MAX_TOTAL) break; /* total cap: stop adding */
        if (j->n_progs[ci] >= EPG_MAX_PER_CH) {
            /* full: replace the programme furthest from now if this one is closer */
            int worst = 0;
            time_t wd = prog_dist(j->progs[ci][0].start, j->progs[ci][0].stop, now);
            for (int q = 1; q < j->n_progs[ci]; q++) {
                time_t d = prog_dist(j->progs[ci][q].start, j->progs[ci][q].stop, now);
                if (d > wd) { wd = d; worst = q; }
            }
            if (nd >= wd) continue;
            free(j->progs[ci][worst].title);
            j->progs[ci][worst] = (Prog){a, b, used ? strdup(title) : title};
            used = true;
            continue;
        }
        if (j->n_progs[ci] == j->cap_progs[ci]) {
            j->cap_progs[ci] = j->cap_progs[ci] ? j->cap_progs[ci] * 2 : 32;
            j->progs[ci] = realloc(j->progs[ci], sizeof(Prog) * (size_t)j->cap_progs[ci]);
        }
        j->progs[ci][j->n_progs[ci]++] = (Prog){a, b, used ? strdup(title) : title};
        used = true;
        j->total++;
    }
    if (!used) free(title);
}

static int prog_cmp(const void *a, const void *b) {
    time_t x = ((const Prog *)a)->start, y = ((const Prog *)b)->start;
    return x < y ? -1 : x > y;
}

/* ---- cache of the parsed guide: parsing 100 MB of XMLTV takes a long time on the Pi, so the result is stored
   compactly and reused as long as epg.xml and the channel list are the same ---- */
#define EPG_CACHE_MAGIC 0x54564548u /* "TVEH" (bumped: older caches have no programme caps) */
typedef struct { uint32_t magic, n_ch; int64_t mtime, size; uint64_t names; } EpgCacheHdr;

static bool epg_cache_load(EpgJob *j, const char *cpath, const struct stat *xs) {
    FILE *f = fopen(cpath, "rb");
    if (!f) return false;
    EpgCacheHdr h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == EPG_CACHE_MAGIC && h.n_ch == (uint32_t)j->n_ch &&
              h.mtime == (int64_t)xs->st_mtime && h.size == (int64_t)xs->st_size && h.names == j->names_hash;
    for (int i = 0; ok && i < j->n_ch; i++) {
        uint32_t n;
        if (fread(&n, 4, 1, f) != 1 || n > 100000) { ok = false; break; }
        j->progs[i] = n ? calloc(n, sizeof(Prog)) : NULL;
        j->cap_progs[i] = (int)n;
        for (uint32_t k = 0; k < n; k++) {
            int64_t ab[2];
            uint16_t l;
            if (fread(ab, sizeof ab, 1, f) != 1 || fread(&l, 2, 1, f) != 1) { ok = false; break; }
            char *t = malloc(l + 1);
            if (fread(t, 1, l, f) != l) { free(t); ok = false; break; }
            t[l] = 0;
            j->progs[i][j->n_progs[i]++] = (Prog){(time_t)ab[0], (time_t)ab[1], t};
            j->total++;
        }
    }
    fclose(f);
    if (!ok) { /* broken or outdated: clean up and parse again */
        for (int i = 0; i < j->n_ch; i++) {
            for (int k = 0; k < j->n_progs[i]; k++) free(j->progs[i][k].title);
            free(j->progs[i]);
            j->progs[i] = NULL;
            j->n_progs[i] = j->cap_progs[i] = 0;
        }
        j->total = 0;
    }
    return ok;
}

static void epg_cache_save(EpgJob *j, const char *cpath, const struct stat *xs) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.part", cpath);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    EpgCacheHdr h = {EPG_CACHE_MAGIC, (uint32_t)j->n_ch, (int64_t)xs->st_mtime, (int64_t)xs->st_size, j->names_hash};
    fwrite(&h, sizeof h, 1, f);
    for (int i = 0; i < j->n_ch; i++) {
        uint32_t n = (uint32_t)j->n_progs[i];
        fwrite(&n, 4, 1, f);
        for (uint32_t k = 0; k < n; k++) {
            Prog *p = &j->progs[i][k];
            int64_t ab[2] = {(int64_t)p->start, (int64_t)p->stop};
            size_t l = strlen(p->title);
            uint16_t l16 = l > 65535 ? 65535 : (uint16_t)l;
            fwrite(ab, sizeof ab, 1, f);
            fwrite(&l16, 2, 1, f);
            fwrite(p->title, 1, l16, f);
        }
    }
    if (fclose(f) == 0) rename(tmp, cpath);
    else unlink(tmp);
}

static void epg_work(void *arg) {
    EpgJob *j = arg;
    char path[512], cpath[512];
    snprintf(path, sizeof path, "%s/epg.xml", data_dir());
    snprintf(cpath, sizeof cpath, "%s/epg.cache", data_dir());
    struct stat st;
    if (stat(path, &st) != 0 || time(NULL) - st.st_mtime > 6 * 3600) {
        if (!http_download_ua(j->url, path, 600, IPTV_UA) && stat(path, &st) != 0) return;
    }
    j->downloaded = true;
    if (stat(path, &st) != 0) return;
    if (epg_cache_load(j, cpath, &st)) { j->ok = true; return; }
    gzFile gz = gzopen(path, "rb"); /* reads both .gz and uncompressed */
    if (!gz) return;
    gzbuffer(gz, 1 << 17);
    time_t now = time(NULL), lo = now - EPG_PAST, hi = now + EPG_AHEAD;
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap + 1);
    int n;
    while ((n = gzread(gz, buf + len, (unsigned)(cap - len))) > 0) {
        len += n;
        buf[len] = 0;
        char *p = buf;
        /* where the next <channel is: searched again only when passed (searching the rest of the buffer
           for every <programme took minutes, since the channels all come first) */
        char *sc = strstr(p, "<channel ");
        for (;;) {
            char *sp = strstr(p, "<programme");
            if (sc && sc < p) sc = strstr(p, "<channel ");
            bool is_ch = sc && (!sp || sc < sp);
            char *s = is_ch ? sc : sp;
            if (!s) { p = buf + (len > 16 ? len - 16 : 0); break; }
            const char *endtag = is_ch ? "</channel>" : "</programme>";
            char *e = strstr(s, endtag);
            if (!e) { p = s; break; }
            *e = 0;
            if (is_ch) epg_channel(j, s, e);
            else epg_handle(j, s, e, lo, hi, now);
            p = e + strlen(endtag);
        }
        size_t rest = len - (p - buf);
        memmove(buf, p, rest);
        len = rest;
        if (len == cap) { cap *= 2; buf = realloc(buf, cap + 1); }
    }
    free(buf);
    gzclose(gz);
    for (int i = 0; i < j->n_ch; i++)
        if (j->n_progs[i] > 1) qsort(j->progs[i], j->n_progs[i], sizeof(Prog), prog_cmp);
    j->ok = true;
    epg_cache_save(j, cpath, &st);
}

static void epg_done(void *arg) {
    EpgJob *j = arg;
    D.epg_loading = false;
    if (!j->downloaded) snprintf(D.epg_msg, sizeof D.epg_msg, T("Program guide: could not be downloaded"));
    else if (j->total == 0) snprintf(D.epg_msg, sizeof D.epg_msg, T("Program guide: no channels matched"));
    else {
        int with = 0;
        for (int i = 0; i < j->n_ch; i++) with += j->n_progs[i] > 0;
        snprintf(D.epg_msg, sizeof D.epg_msg, T("Program guide: %d of %d channels"), with, j->n_ch);
    }
    if (j->ok && j->gen == D.gen && j->n_ch == D.n_ch) {
        for (int i = 0; i < D.n_ch; i++) {
            for (int k = 0; k < D.ch[i].n_progs; k++) free(D.ch[i].progs[k].title);
            free(D.ch[i].progs);
            D.ch[i].progs = j->progs[i];
            D.ch[i].n_progs = j->n_progs[i];
        }
    } else {
        for (int i = 0; i < j->n_ch; i++) {
            for (int k = 0; k < j->n_progs[i]; k++) free(j->progs[i][k].title);
            free(j->progs[i]);
        }
    }
    for (int i = 0; i < j->n_map; i++) free(j->map[i].id);
    free(j->map);
    for (int i = 0; i < j->n_ch; i++) { free(j->names[i]); free(j->tvgids[i]); }
    free(j->names);
    free(j->tvgids);
    free(j->by_id);
    free(j->by_name);
    free(j->exact);
    free(j->progs);
    free(j->n_progs);
    free(j->cap_progs);
    free(j->url);
    free(j);
}

static char **sort_keys; /* for key_idx_cmp (qsort has no context; only used on the main thread) */
static int key_idx_cmp(const void *a, const void *b) { return strcmp(sort_keys[*(const int *)a], sort_keys[*(const int *)b]); }

static void epg_start(void) {
    if (D.short_epg) { D.epg_msg[0] = 0; return; } /* fetched per channel, see epg_want */
    if (!D.epg_url || !*D.epg_url) { snprintf(D.epg_msg, sizeof D.epg_msg, T("No program guide - add an EPG URL under Settings (red button)")); return; }
    if (D.epg_loading || D.n_ch == 0) return;
    EpgJob *j = calloc(1, sizeof *j);
    j->url = strdup(D.epg_url);
    j->gen = D.gen;
    j->n_ch = D.n_ch;
    j->progs = calloc(D.n_ch, sizeof(Prog *));
    j->n_progs = calloc(D.n_ch, sizeof(int));
    j->cap_progs = calloc(D.n_ch, sizeof(int));
    j->names = calloc(D.n_ch, sizeof(char *));
    j->tvgids = calloc(D.n_ch, sizeof(char *));
    j->exact = calloc(D.n_ch, sizeof(bool));
    unsigned long hsh = 5381;
    for (int i = 0; i < D.n_ch; i++) {
        for (const char *q = D.ch[i].name; *q; q++) hsh = hsh * 33 + (unsigned char)*q;
        j->names[i] = norm_name(D.ch[i].name);
        if (D.ch[i].tvgid && *D.ch[i].tvgid) {
            j->tvgids[i] = strdup(D.ch[i].tvgid);
            map_add(j, D.ch[i].tvgid, i);
        }
    }
    j->names_hash = hsh;
    qsort(j->map, j->n_map, sizeof(IdMap), idmap_cmp);
    j->n_sorted = j->n_map;
    /* indexes sorted by tvg-id and by name (the strings do not change while the job runs) */
    j->by_id = malloc(sizeof(int) * (size_t)(D.n_ch + 1));
    j->by_name = malloc(sizeof(int) * (size_t)(D.n_ch + 1));
    for (int i = 0; i < D.n_ch; i++) {
        if (j->tvgids[i]) j->by_id[j->n_by_id++] = i;
        j->by_name[i] = i;
    }
    sort_keys = j->tvgids;
    qsort(j->by_id, (size_t)j->n_by_id, sizeof(int), key_idx_cmp);
    sort_keys = j->names;
    qsort(j->by_name, (size_t)D.n_ch, sizeof(int), key_idx_cmp);
    D.epg_loading = true;
    snprintf(D.epg_msg, sizeof D.epg_msg, T("Loading program guide…"));
    job_submit_q(JQ_SLOW, epg_work, epg_done, j);
}

/* ---------- EPG via Xtream (get_short_epg): only for the channels you look at ---------- */
typedef struct SepgRes { int ci, gen; Prog *p; int n; bool failed; struct SepgRes *next; } SepgRes;
static SDL_Mutex *sepg_lock;
static SepgRes *sepg_inbox; /* replies from background threads - emptied by the UI and the playback loop */
typedef struct { char *url; int ci, gen; } SepgJob;

static char *b64dec(const char *s) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(s);
    char *out = malloc(n * 3 / 4 + 4), *w = out;
    unsigned v = 0;
    int bits = 0;
    for (; *s; s++) {
        const char *p = strchr(tbl, *s);
        if (!p || !*s) { if (*s == '=') break; continue; }
        v = (v << 6) | (unsigned)(p - tbl);
        bits += 6;
        if (bits >= 8) { bits -= 8; *w++ = (char)((v >> bits) & 0xff); }
    }
    *w = 0;
    return out;
}

static void sepg_work(void *arg) {
    SepgJob *j = arg;
    SepgRes *r = calloc(1, sizeof *r);
    r->ci = j->ci;
    r->gen = j->gen;
    static const char *const hdr[] = {"User-Agent: " IPTV_UA, NULL};
    HttpBuf b;
    if (http_request("GET", j->url, hdr, NULL, &b, 8) && b.status == 200 && b.data) {
        cJSON *root = cJSON_Parse(b.data), *e;
        cJSON *arr = cJSON_GetObjectItem(root, "epg_listings");
        r->p = calloc(cJSON_GetArraySize(arr) + 1, sizeof(Prog));
        cJSON_ArrayForEach(e, arr) {
            cJSON *t = cJSON_GetObjectItem(e, "title");
            cJSON *a = cJSON_GetObjectItem(e, "start_timestamp"), *z = cJSON_GetObjectItem(e, "stop_timestamp");
            time_t ta = cJSON_IsString(a) ? atol(a->valuestring) : cJSON_IsNumber(a) ? (time_t)a->valuedouble : 0;
            time_t tz = cJSON_IsString(z) ? atol(z->valuestring) : cJSON_IsNumber(z) ? (time_t)z->valuedouble : 0;
            if (!ta || !tz || !cJSON_IsString(t)) continue;
            r->p[r->n++] = (Prog){ta, tz, b64dec(t->valuestring)};
        }
        cJSON_Delete(root);
    } else {
        r->failed = true;
    }
    http_free(&b);
    SDL_LockMutex(sepg_lock);
    r->next = sepg_inbox;
    sepg_inbox = r;
    SDL_UnlockMutex(sepg_lock);
    free(j->url);
    free(j);
}

/* put finished replies into the channels; true if channel 'watch' was updated */
static bool sepg_drain(int watch) {
    if (!sepg_lock) return false;
    SDL_LockMutex(sepg_lock);
    SepgRes *r = sepg_inbox;
    sepg_inbox = NULL;
    SDL_UnlockMutex(sepg_lock);
    bool hit = false;
    while (r) {
        SepgRes *nx = r->next;
        D.sepg_busy--;
        if (r->gen == D.gen) {
            if (r->failed) D.sepg_fail++;
            else if (r->n > 0) D.sepg_ok++;
        }
        if (r->gen == D.gen && r->ci >= 0 && r->ci < D.n_ch) {
            Channel *c = &D.ch[r->ci];
            for (int k = 0; k < c->n_progs; k++) free(c->progs[k].title);
            free(c->progs);
            c->progs = r->p;
            c->n_progs = r->n;
            c->epg_busy = false;
            c->epg_t = app_time();
            if (r->ci == watch) hit = true;
        } else {
            for (int k = 0; k < r->n; k++) free(r->p[k].title);
            free(r->p);
        }
        free(r);
        r = nx;
    }
    /* say so instead of just showing nothing (the guide comes back by itself when the provider fixes it) */
    if (D.short_epg) {
        const char *m = D.sepg_ok == 0 && D.sepg_fail > 0 ? T("The provider's program guide is not responding") : "";
        snprintf(D.epg_msg, sizeof D.epg_msg, "%s", m);
    }
    return hit;
}

static const Prog *prog_at(const Channel *c, time_t t, int offset);

/* request program data for a channel if it is missing or outdated */
static void epg_want(int gen, int ci) {
    if (!D.short_epg || gen != D.gen || ci < 0 || ci >= D.n_ch) return;
    Channel *c = &D.ch[ci];
    if (c->sid <= 0 || c->epg_busy || D.sepg_busy >= 16) return;
    double age = app_time() - c->epg_t;
    if (c->epg_t > 0 && age < 1800 && (prog_at(c, time(NULL), 0) || age < 300)) return;
    const char *lv = strstr(c->url, "/live/");
    if (!lv) return;
    const char *u = lv + 6, *p = strchr(u, '/'), *e = p ? strchr(p + 1, '/') : NULL;
    if (!p || !e) return;
    if (!sepg_lock) sepg_lock = SDL_CreateMutex();
    SepgJob *j = calloc(1, sizeof *j);
    j->url = str_printf("%.*s/player_api.php?username=%.*s&password=%.*s&action=get_short_epg&stream_id=%d&limit=8",
                        (int)(lv - c->url), c->url, (int)(p - u), u, (int)(e - p - 1), p + 1, c->sid);
    j->ci = ci;
    j->gen = gen;
    c->epg_busy = true;
    D.sepg_busy++;
    job_submit(sepg_work, NULL, j);
}

static const Prog *prog_at(const Channel *c, time_t t, int offset) {
    for (int i = 0; i < c->n_progs; i++)
        if (c->progs[i].start <= t && c->progs[i].stop > t) {
            int k = i + offset;
            return k >= 0 && k < c->n_progs ? &c->progs[k] : NULL;
        }
    return NULL;
}

/* ---------- screen ---------- */
typedef struct {
    int col;          /* 0 = groups, 1 = channels */
    int gsel, csel;   /* chosen group / channel (in the filtered list) */
    bool moving;      /* moves a favorite with up/down */
    bool user_moved;  /* the user has already navigated: do not move the selection from under them */
    float gscroll, cscroll;
    int *list;        /* index into D.ch for the current group */
    int n_list;
    int built_gen, built_g, fav_count;
    int cfg_seen;
    char digits[8];
    double digit_t;
    int playing_list_owner;
    double ok_pending; /* time of the OK press: short press = play, held down = menu */
    time_t gtime, gview; /* TV guide: the chosen time (picks the program) and the start of the visible window */
} Iptv;

static void play_current(Iptv *v);

#define GROW 66 /* row height in the group column */
/* left column: 0 = Favorites, 1 = TV guide (the favorites on a timeline), 2.. = D.ui (country headings and groups) */
static int n_groups_ui(void) { return D.n_vis + 2; }
static int ui_entry(int g) { return g >= 2 && g - 2 < D.n_vis ? D.ui[D.vis[g - 2]] : INT_MIN; }
static int ui_group(int g) { int e = ui_entry(g); return e >= 0 ? e : -1; }
static int ui_country(int g) { int e = ui_entry(g); return e < 0 && e != INT_MIN ? -e - 1 : -1; }
static const char *group_name(int g) {
    if (g == 0) return T("Favorites");
    if (g == 1) return T("TV guide");
    int ci = ui_country(g);
    if (ci >= 0) return country_name(D.countries[ci]);
    const char *n = D.groups[ui_group(g)];
    int l = D.n_countries >= 2 ? country_len(n) : 0; /* the country is already in the heading */
    if (l) { n += l + 1; while (*n == ' ') n++; }
    return n;
}

/* the left column can be shorter than gsel (the groups are not loaded yet, or the playlist has none):
   use an "All channels" entry if the playlist has one, otherwise Favorites (0) */
static void clamp_gsel(Iptv *v) {
    if (v->gsel >= 0 && v->gsel < n_groups_ui()) return;
    for (int g = 2; g < n_groups_ui(); g++)
        if (ui_group(g) >= 0 && !strcmp(group_name(g), "All channels")) { v->gsel = g; return; }
    v->gsel = 0;
}

/* never leave the focus on a column that has nothing in it */
static void fix_focus(Iptv *v) {
    clamp_gsel(v);
    if (v->col == 1 && v->n_list == 0) v->col = 0;
}

static void build_list(Iptv *v) {
    if (v->built_gen != D.gen) v->csel = 0; /* new channel list: the old index does not fit */
    if (v->gsel >= n_groups_ui()) { v->gsel = 0; v->csel = 0; }
    free(v->list);
    v->list = malloc(sizeof(int) * (D.n_ch + n_favs + 1));
    v->n_list = 0;
    if (v->gsel <= 1) { /* favorites (also the TV guide) in the user's own order */
        for (int k = 0; k < n_favs; k++)
            for (int i = 0; i < D.n_ch; i++)
                if (!strcmp(D.ch[i].url, favs[k])) { v->list[v->n_list++] = i; break; }
    } else for (int i = 0; i < D.n_ch; i++) {
        Channel *c = &D.ch[i];
        int gi = ui_group(v->gsel), ci = ui_country(v->gsel);
        bool inc = (gi >= 0 && !strcmp(c->group, D.groups[gi])) || (ci >= 0 && in_country(c, ci));
        if (inc) v->list[v->n_list++] = i;
    }
    v->built_gen = D.gen;
    v->built_g = v->gsel;
    v->fav_count = n_favs;
    if (v->csel >= v->n_list) v->csel = v->n_list ? v->n_list - 1 : 0;
}

static void iptv_enter(Screen *s) {
    Iptv *v = s->st;
    favs_load();
    v->gsel = n_favs > 0 ? 0 : 2; /* no favorites: the first country/group (clamped below while the groups load) */
    v->user_moved = false;
    v->col = 1;
    v->cfg_seen = app_cfg_gen();
    if (D.n_ch == 0 || D.status[0]) iptv_load(false);
    clamp_gsel(v);
    build_list(v);
    fix_focus(v);
}

/* the new channel list is in (D.ch/D.n_vis): correct a selection it no longer has */
static void iptv_after_load(void) {
    Screen *s = app_top();
    if (!s || !s->name || strcmp(s->name, "iptv") || !s->st) return;
    Iptv *v = s->st;
    /* first start: while the groups loaded, clamp_gsel() put the selection on Favorites,
       which is empty. Move to the first country/group unless the user has already moved. */
    if (n_favs == 0 && v->gsel == 0 && !v->user_moved && n_groups_ui() > 2) v->gsel = 2;
    clamp_gsel(v);
    build_list(v);
    fix_focus(v);
}

static void iptv_update(Screen *s, float dt) {
    Iptv *v = s->st;
    if (v->ok_pending && app_time() - v->ok_pending > HOLD_GAP) { v->ok_pending = 0; play_current(v); }
    sepg_drain(-1);
    if (v->cfg_seen != app_cfg_gen()) { /* new playlist from the phone */
        v->cfg_seen = app_cfg_gen();
        iptv_load(true);
    }
    clamp_gsel(v);
    if (v->built_gen != D.gen || v->built_g != v->gsel || (v->gsel <= 1 && v->fav_count != n_favs)) build_list(v);
    fix_focus(v);
    if (v->gsel == 1) { /* TV guide: the chosen time never lies in the past; the window follows it */
        time_t now = time(NULL), base = now - now % 1800;
        if (v->gtime < now) v->gtime = now;
        if (v->gview < base) v->gview = base;
        if (v->gtime >= v->gview + 90 * 60) v->gview = v->gtime - v->gtime % 1800 - 1800;
        if (v->gtime < v->gview) v->gview = v->gtime - v->gtime % 1800;
        if (v->gview < base) v->gview = base;
    }
    if (v->digits[0] && app_time() - v->digit_t > 1.4) {
        int num = atoi(v->digits);
        v->digits[0] = 0;
        for (int i = 0; i < v->n_list; i++)
            if (i + 1 == num) { v->csel = i; break; }
    }
    float rh = 100;
    float vis = 8;
    float target = v->csel * rh - rh * 3;
    if (target < 0) target = 0;
    float maxs = v->n_list * rh - vis * rh;
    if (target > maxs) target = maxs > 0 ? maxs : 0;
    v->cscroll = anim_to(v->cscroll, target, dt, 14);
    float gt = v->gsel * GROW - GROW * 5;
    if (gt < 0) gt = 0;
    v->gscroll = anim_to(v->gscroll, gt, dt, 14);
}

static int setup_btn; /* button in focus on the setup screen */
static void draw_setup(void) {
    draw_background();
    draw_topbar("Live TV");
    const char *ip = app_ip();
    char msg[600];
    snprintf(msg, sizeof msg, "%s", T("Add your IPTV provider's playlist (M3U link) or Xtream Codes login.\n\nSign in with Xtream Codes, or enter an M3U link."));
    if (ip[0]) {
        size_t l = strlen(msg);
        snprintf(msg + l, sizeof msg - l, T("\nEasiest: open http://%s on your phone and paste the link there."), ip);
    }
    draw_icon(ICON_TV, UI_W / 2, 320, 140, C_TEXT3);
    draw_empty(T("No channels yet"), msg);
    const char *b[] = {T("Sign in with Xtream Codes"), T("Enter M3U link")};
    draw_buttons(UI_W / 2, 800, b, 2, setup_btn);
}

/* TV guide: the favorites on a 2-hour timeline (program data from get_short_epg, about 8 programs ahead) */
#define GUIDE_SPAN (120 * 60)
typedef struct { time_t start, stop; const Prog *p; } GuideProg;
static int guide_prog_cmp(const void *a, const void *b) {
    time_t x = ((const GuideProg *)a)->start, y = ((const GuideProg *)b)->start;
    return x < y ? -1 : x > y;
}
static int guide_time_cmp(const void *a, const void *b) {
    time_t x = *(const time_t *)a, y = *(const time_t *)b;
    return x < y ? -1 : x > y;
}
static bool guide_tile(const Prog *p, time_t a, time_t b, time_t v0, time_t v1,
                       float gx, float y, float h, float pps, time_t now, bool me) {
    a = a > v0 ? a : v0;
    b = b < v1 ? b : v1;
    if (b <= a) return false;
    float x = gx + (float)(a - v0) * pps, w = (float)(b - a) * pps - 6;
    if (w < 4) return true;
    bool on = p->start <= now && p->stop > now;
    float fp = ui_focus(p, me), fc = fp < 0 ? 0 : fp > 1 ? 1 : fp; /* the program in focus lights up (no lift in the grid) */
    if (fc > 0.01f) ui_focus_under(x, y, w, h, 10, fc);
    fill_rrect(x, y, w, h, 10, color_mix(on ? RGBA(255, 255, 255, 30) : RGBA(255, 255, 255, 14), RGB(66, 76, 98), fc));
    if (fc > 0.01f) ui_focus_over(x, y, w, h, 10, fc);
    if (w > 40) {
        text_draw_clip(F_SEMI, 24, x + 14, y + 14, w - 24, me || on ? C_TEXT : C_TEXT2, AL_LEFT, p->title);
        char span[24], t1[8], t2[8];
        struct tm ta, tb;
        localtime_r(&p->start, &ta);
        localtime_r(&p->stop, &tb);
        strftime(t1, sizeof t1, "%H:%M", &ta);
        strftime(t2, sizeof t2, "%H:%M", &tb);
        snprintf(span, sizeof span, "%s-%s", t1, t2);
        text_draw_clip(F_REG, 20, x + 14, y + 50, w - 24, C_TEXT3, AL_LEFT, span);
    }
    return true;
}
static void draw_guide(Iptv *v, float cx, float cy, float cw, float rh) {
    time_t now = time(NULL), v0 = v->gview, v1 = v0 + GUIDE_SPAN;
    float nw = 250, gx = cx + nw, gw = cw - nw, top = cy + 46;
    float pps = gw / (float)GUIDE_SPAN; /* pixels per second */
    if (v->n_list == 0) {
        text_draw(F_REG, 28, cx + cw / 2, cy + 200, C_TEXT2, AL_CENTER, T("Add favorites to see their programs here (Yellow on a channel)."));
        return;
    }
    /* time labels every half hour */
    for (time_t t = v0; t < v1; t += 1800) {
        char hm[8];
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(hm, sizeof hm, "%H:%M", &tm);
        float x = gx + (float)(t - v0) * pps;
        text_draw(F_SEMI, 22, x + 4, cy + 4, C_TEXT2, AL_LEFT, hm);
        fill_rrect(x, cy + 34, 2, 8, 1, C_TEXT3);
    }
    /* now: a red line behind the programs (the titles stay readable) */
    if (now >= v0 && now < v1) {
        float x = gx + (float)(now - v0) * pps;
        fill_rrect(x - 1.5f, cy + 30, 3, UI_H - cy - 30 - 74, 1.5f, C_DANGER);
    }
    clip_push(cx - 10, top - 6, cw + 20, UI_H - top - 74);
    for (int i = 0; i < v->n_list; i++) {
        float y = top + i * rh - v->cscroll;
        if (y < top - rh || y > UI_H) continue;
        Channel *c = &D.ch[v->list[i]];
        epg_want(v->built_gen, v->list[i]);
        bool row = v->col == 1 && v->csel == i;
        float h = rh - 12;
        /* channel cell */
        fill_rrect(cx, y, nw - 12, h, 12, row ? RGB(52, 60, 78) : C_SURFACE);
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1);
        text_draw(F_SEMI, 22, cx + 16, y + h / 2 - 14, row ? C_TEXT : C_TEXT3, AL_LEFT, num);
        text_draw_clip(F_SEMI, 24, cx + 58, y + h / 2 - 16, nw - 84, row ? C_TEXT : C_TEXT2, AL_LEFT, DNAME(c));
        /* programs: build a non-overlapping row (the provider's guide has duplicates and overlaps) */
        const Prog *selp = row ? prog_at(c, v->gtime, 0) : NULL;
        bool any = false;
        GuideProg *gp = c->n_progs > 0 ? malloc(sizeof *gp * (size_t)c->n_progs) : NULL;
        time_t *tt = NULL;
        int ng = 0;
        if (gp) {
            for (int k = 0; k < c->n_progs; k++)
                gp[k] = (GuideProg){c->progs[k].start, c->progs[k].stop, &c->progs[k]};
            if (c->n_progs > 1) qsort(gp, (size_t)c->n_progs, sizeof *gp, guide_prog_cmp);
            for (int k = 0; k < c->n_progs; k++) {
                const Prog *p = gp[k].p;
                if (p->stop <= p->start) continue;
                /* a duplicate starts within 2 minutes of a program already kept */
                if (ng > 0 && p->start - gp[ng - 1].p->start <= 120) continue;
                gp[ng++] = (GuideProg){p->start, p->stop, p};
            }
            if (ng > 0) tt = malloc(sizeof *tt * (size_t)(2 * ng));
        }
        if (tt) {
            int nt = 0;
            for (int k = 0; k < ng; k++) {
                tt[nt++] = gp[k].start;
                tt[nt++] = gp[k].stop;
            }
            qsort(tt, (size_t)nt, sizeof *tt, guide_time_cmp);
            int nu = 0;
            for (int k = 0; k < nt; k++)
                if (nu == 0 || tt[k] != tt[nu - 1]) tt[nu++] = tt[k];
            /* each interval shows the covering program with the latest start */
            const Prog *runp = NULL;
            time_t runa = 0, runb = 0;
            for (int k = 0; k + 1 < nu; k++) {
                time_t a = tt[k], b = tt[k + 1];
                const Prog *best = NULL;
                for (int j = 0; j < ng; j++) {
                    const Prog *p = gp[j].p;
                    if (p->start <= a && p->stop >= b && (!best || p->start > best->start)) best = p;
                }
                if (best != runp) {
                    if (runp) any |= guide_tile(runp, runa, runb, v0, v1, gx, y, h, pps, now, runp == selp);
                    runp = best;
                    runa = a;
                }
                runb = b;
            }
            if (runp) any |= guide_tile(runp, runa, runb, v0, v1, gx, y, h, pps, now, runp == selp);
        }
        free(tt);
        free(gp);
        if (!any) {
            fill_rrect(gx, y, gw - 6, h, 10, RGBA(255, 255, 255, 8));
            if (row) { ui_focus_under(gx, y, gw - 6, h, 10, 1); fill_rrect(gx, y, gw - 6, h, 10, RGB(52, 60, 78)); ui_focus_over(gx, y, gw - 6, h, 10, 1); }
            text_draw(F_REG, 22, gx + 14, y + h / 2 - 14, C_TEXT3, AL_LEFT,
                      c->epg_busy || (D.short_epg && c->epg_t == 0) ? T("Loading…") : T("No program info"));
        }
    }
    clip_pop();
}

static void iptv_draw(Screen *s) {
    Iptv *v = s->st;
    bool configured = iptv_configured();
    if (!configured) { draw_setup(); return; }
    draw_background();
    draw_topbar("Live TV");
    if (D.n_ch == 0) {
        if (D.loading) {
            draw_spinner(UI_W / 2, UI_H / 2 - 40, 34, C_TEXT);
            text_draw(F_REG, 28, UI_W / 2, UI_H / 2 + 30, C_TEXT2, AL_CENTER, D.status);
        } else draw_empty(T("No channels"), D.status[0] ? D.status : T("Press the red button to change the playlist."));
        return;
    }
    /* groups */
    float gx = 80, gy = 150, gw = 440;
    clip_push(gx - 20, gy, gw + 40, UI_H - gy - 80);
    for (int g = 0; g < n_groups_ui(); g++) {
        float y = gy + g * GROW - v->gscroll;
        if (y < gy - GROW || y > UI_H) continue;
        bool sel = g == v->gsel, f = sel && v->col == 0;
        int gi = ui_group(g), ci = ui_country(g);
        float rh_ = GROW - 8;
        if (f) fill_rrect(gx, y, gw, rh_, 12, C_FOCUS);
        else if (sel) fill_rrect(gx, y, gw, rh_, 12, C_SURFACE2);
        else if (ci >= 0) fill_rrect(gx, y, gw, rh_, 12, RGBA(255, 255, 255, 18));
        Color tc = f ? RGB(10, 10, 10) : sel ? C_TEXT : C_TEXT2;
        Color sc = f ? RGB(70, 70, 75) : C_TEXT3;
        bool indent = gi >= 0 && D.gcountry && D.gcountry[gi] >= 0;
        float tx = gx + (g == 0 || ci >= 0 ? 56 : indent ? 48 : 24), my = y + rh_ / 2;
        if (g == 0) draw_icon(ICON_STAR, gx + 30, my, 24, f ? RGB(10, 10, 10) : RGB(255, 200, 60));
        if (ci >= 0) { /* fold arrow: ▸ collapsed, ▾ expanded */
            float ax = gx + 30;
            if (D.cexp[ci]) fill_tri(ax - 8, my - 5, ax + 8, my - 5, ax, my + 6, tc);
            else fill_tri(ax - 5, my - 8, ax - 5, my + 8, ax + 6, my, tc);
        }
        int count = g <= 1 ? n_favs : ci >= 0 ? D.ccount[ci] : (D.gcount ? D.gcount[gi] : 0);
        char cnt[16];
        snprintf(cnt, sizeof cnt, "%d", count);
        float cw = text_width(F_REG, 20, cnt);
        text_draw_clip(F_SEMI, 25, tx, my - 16, gw - (tx - gx) - cw - 30, tc, AL_LEFT, group_name(g));
        text_draw(F_REG, 20, gx + gw - 16, my - 13, sc, AL_RIGHT, cnt);
    }
    clip_pop();

    /* channels */
    float cx = 560, cy = 150, cw = UI_W - cx - 80, rh = 100;
    if (v->gsel == 1) draw_guide(v, cx, cy, cw, rh);
    else {
    clip_push(cx - 10, cy - 10, cw + 20, UI_H - cy - 70);
    time_t now = time(NULL);
    if (v->n_list == 0) {
        text_draw(F_REG, 28, cx + cw / 2, cy + 200, C_TEXT2, AL_CENTER,
                  v->gsel == 0 ? T("No favorites yet. Choose a channel and press Favorite at the bottom (or Yellow).") : T("No channels in this group"));
    }
    for (int i = 0; i < v->n_list; i++) {
        float y = cy + i * rh - v->cscroll;
        if (y < cy - rh || y > UI_H) continue;
        Channel *c = &D.ch[v->list[i]];
        epg_want(v->built_gen, v->list[i]);
        bool f = v->col == 1 && v->csel == i;
        bool sel = v->csel == i;
        if (f) { draw_shadow(cx, y, cw, rh - 10, 16, 16, 1); fill_rrect(cx, y, cw, rh - 10, 16, RGB(52, 60, 78)); }
        if (f && v->moving) stroke_rrect(cx - 4, y - 4, cw + 8, rh - 2, 20, 4, C_FOCUS);
        else if (sel) fill_rrect(cx, y, cw, rh - 10, 16, C_SURFACE);
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1); /* number = position in the list (the provider's numbers are random) */
        text_draw(F_SEMI, 26, cx + 60, y + 30, f ? C_TEXT : C_TEXT3, AL_RIGHT, num);
        if (logo_card()) fill_rrect(cx + 80, y + 10, 124, 70, 10, RGB(235, 238, 244));
        Image *im = chan_logo(c->logo);
        if (!img_draw(im, cx + 86, y + 14, 112, 62, 8, 1))
            text_draw_clip(F_BOLD, 22, cx + 142, y + 32, 110, logo_card() ? RGB(60, 60, 70) : C_TEXT2, AL_CENTER, DNAME(c));
        float tx = cx + 230;
        if (is_fav(c->url)) draw_icon(ICON_STAR, tx + 10, y + 32, 20, RGB(255, 200, 60)), tx += 30;
        text_draw_clip(F_SEMI, 30, tx, y + 12, 520, C_TEXT, AL_LEFT, DNAME(c));
        const Prog *p = prog_at(c, now, 0);
        if (p) {
            float px = cx + 800, pw = cw - 830;
            char tm1[8], tm2[8];
            struct tm a, b;
            localtime_r(&p->start, &a);
            localtime_r(&p->stop, &b);
            strftime(tm1, sizeof tm1, "%H:%M", &a);
            strftime(tm2, sizeof tm2, "%H:%M", &b);
            text_draw_clip(F_SEMI, 26, px, y + 12, pw - 140, C_TEXT, AL_LEFT, p->title);
            char span[24];
            snprintf(span, sizeof span, "%s-%s", tm1, tm2);
            text_draw(F_REG, 22, px + pw, y + 16, C_TEXT2, AL_RIGHT, span);
            float prog = (float)(now - p->start) / (float)(p->stop - p->start);
            fill_rrect(px, y + 56, pw, 6, 3, RGBA(255, 255, 255, 40));
            fill_rrect(px, y + 56, pw * prog, 6, 3, C_ACCENT);
            const Prog *nx = prog_at(c, now, 1);
            if (nx) {
                char nb[300];
                struct tm t3;
                localtime_r(&nx->start, &t3);
                char tm3[8];
                strftime(tm3, sizeof tm3, "%H:%M", &t3);
                snprintf(nb, sizeof nb, "%s  %s", tm3, nx->title);
                text_draw_clip(F_REG, 20, tx, y + 54, 520, C_TEXT3, AL_LEFT, nb);
            }
        } else {
            text_draw_clip(F_REG, 22, tx, y + 54, 520, C_TEXT3, AL_LEFT, c->group);
        }
    }
    clip_pop();
    }
    if (v->col == 1 && v->gsel == 1 && v->n_list)
        text_draw(F_REG, 22, cx + cw, UI_H - 56, C_TEXT3, AL_RIGHT, T("Left/right: earlier and later programs"));
    if (v->col == 1 && v->gsel == 0 && v->n_list > 1)
        text_draw(F_REG, 22, cx + cw, UI_H - 56, v->moving ? C_TEXT : C_TEXT3, AL_RIGHT,
                  v->moving ? T("Move with up/down · OK when you are done") : T("Right: move channel · The Favorite button at the bottom removes it"));
    if (v->digits[0]) {
        fill_rrect(UI_W - 300, 140, 220, 110, 20, color_alpha(C_SURFACE2, 0.95f));
        text_draw(F_BOLD, 64, UI_W - 190, 156, C_TEXT, AL_CENTER, v->digits);
    }
    float hy = UI_H - 60;
    draw_keybar(80, hy, T("Watch channel"), T("Channel menu"), T("Favorite"), NULL, "Menu");
    /* status at the top, left of the clock (the bottom right holds the favorites hint) */
    const char *st = D.loading ? T("Updating channels…") : D.epg_msg;
    if (st[0]) text_draw(F_REG, 22, UI_W - 96 - text_width(F_SEMI, 34, "00:00") - 40, 72, C_TEXT3, AL_RIGHT, st);
}

/* ---------- playback ---------- */
typedef struct { int *list; int n, cur, shown; int gen; } PlayCtx; /* shown: the channel the banner shows */

/* a playlist's indexes into D.ch are only valid for the channel list (D.gen) they were made for */
static bool pc_index(const PlayCtx *pc, int idx) {
    return pc->gen == D.gen && idx >= 0 && idx < pc->n && pc->list && pc->list[idx] >= 0 && pc->list[idx] < D.n_ch;
}

/* copies of the logo at 1/8 .. 7/8 opacity (path.1 .. path.7) - the OSD script steps through them while the banner fades
   (mpv's overlay-add has no opacity of its own). The pixels are premultiplied, so all four channels are scaled */
#define LOGO_FADES 8
static void logo_fades(const char *path, int w, int h) {
    size_t n = (size_t)w * h * 4;
    uint8_t *src = malloc(n), *dst = malloc(n);
    FILE *f = src && dst ? fopen(path, "rb") : NULL;
    bool ok = f && fread(src, 1, n, f) == n;
    if (f) fclose(f);
    for (int lv = 1; ok && lv < LOGO_FADES; lv++) {
        unsigned k = lv * 256 / LOGO_FADES;
        for (size_t i = 0; i < n; i++) dst[i] = (uint8_t)(src[i] * k >> 8);
        char p[96], tmp[100];
        snprintf(p, sizeof p, "%s.%d", path, lv);
        snprintf(tmp, sizeof tmp, "%s.part", p);
        FILE *o = fopen(tmp, "wb");
        if (!o) break;
        bool w_ok = fwrite(dst, 1, n, o) == n;
        fclose(o);
        if (w_ok) rename(tmp, p); else unlink(tmp);
    }
    free(src);
    free(dst);
}

/* the channel logo for the banner - only if it has already been fetched (network here would block channel changes) */
static void osd_logo(cJSON *o, const char *logo, int slot) {
    int lw, lh;
    char lp[64];
    snprintf(lp, sizeof lp, "/tmp/notatv-logo-%d.bgra", slot);
    char lu[1024]; /* the cleaned logo with transparency (no light card in the banner) where NotaTV can */
    snprintf(lu, sizeof lu, "%s%s", logo_card() ? "" : "logo:", logo ? logo : "");
    /* also while zapping: only from the cache (img_cached), so no network is waited for; a small logo is quick */
    if (!logo_card()) cJSON_AddTrueToObject(o, "logo_clean"); /* no light card in the banner, logo or not */
    if (logo && *logo && img_cached(lu) && img_export_bgra(lu, lp, 220, 120, &lw, &lh)) {
        cJSON_AddStringToObject(o, "logo", lp);
        logo_fades(lp, lw, lh);
        if (hdr_cfg("hdr_always", 0)) { /* the TV in HDR: the OSD script uses this one */
            char hp[64];
            int hw, hh;
            snprintf(hp, sizeof hp, "/tmp/notatv-logo-%d-hdr.bgra", slot);
            if (img_export_bgra_ex(lu, hp, 220, 120, &hw, &hh, true) && hw == lw && hh == lh) {
                cJSON_AddStringToObject(o, "logo_hdr", hp);
                logo_fades(hp, hw, hh);
            }
        }
        cJSON_AddNumberToObject(o, "logo_fades", LOGO_FADES);
        cJSON_AddNumberToObject(o, "logo_w", lw);
        cJSON_AddNumberToObject(o, "logo_h", lh);
    }
}

/* write /tmp/notatv-chan.json (frees o) and tell the OSD script */
static void osd_write(cJSON *o, const char *mode) {
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    FILE *f = js ? fopen("/tmp/notatv-chan.json.part", "w") : NULL;
    if (f) {
        fputs(js, f);
        fclose(f);
        rename("/tmp/notatv-chan.json.part", "/tmp/notatv-chan.json");
        player_message("notatv-chan", mode);
    }
    free(js);
}

/* the channel banner + browse panel are drawn by notatv-osd.lua from /tmp/notatv-chan.json */
/* mode: "show" (after a channel change), "zap" (browsing), "update" (new program info - does not change visibility) */
static void osd_channel(PlayCtx *pc, int idx, const char *mode) {
    if (!pc_index(pc, idx)) return;
    bool zapping = !strcmp(mode, "zap");
    if (strcmp(mode, "update")) pc->shown = idx;
    Channel *c = &D.ch[pc->list[idx]];
    epg_want(pc->gen, pc->list[idx]);
    time_t now = time(NULL);
    const Prog *p = prog_at(c, now, 0), *nx = prog_at(c, now, 1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "num", idx + 1);
    cJSON_AddStringToObject(o, "name", DNAME(c));
    const char *g = c->group;
    int l = country_len(g);
    if (l) { /* "DK: TV2 PLAY" -> country "Denmark", group "TV2 PLAY" */
        char code[8] = "";
        memcpy(code, g, l);
        cJSON_AddStringToObject(o, "country", country_name(code));
        g += l + 1;
        while (*g == ' ') g++;
    }
    cJSON_AddStringToObject(o, "group", g);
    cJSON_AddNumberToObject(o, "pos", idx + 1);
    cJSON_AddNumberToObject(o, "total", pc->n);
    cJSON_AddNumberToObject(o, "now_t", (double)now);
    if (p) {
        cJSON_AddStringToObject(o, "now", p->title ? p->title : "");
        cJSON_AddNumberToObject(o, "now_start", (double)p->start);
        cJSON_AddNumberToObject(o, "now_stop", (double)p->stop);
    }
    if (nx) {
        cJSON_AddStringToObject(o, "next", nx->title ? nx->title : "");
        cJSON_AddNumberToObject(o, "next_start", (double)nx->start);
    }
    /* neighbors for the browse panel */
    cJSON *arr = cJSON_AddArrayToObject(o, "list");
    for (int k = -3; k <= 3; k++) {
        int j = ((idx + k) % pc->n + pc->n) % pc->n;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "num", j + 1);
        cJSON_AddStringToObject(e, "name", DNAME(&D.ch[pc->list[j]]));
        cJSON_AddItemToArray(arr, e);
    }
    (void)zapping;
    osd_logo(o, c->logo, idx % 2);
    osd_write(o, mode);
}

static void on_channel(void *ud, int idx) {
    PlayCtx *pc = ud;
    if (!pc_index(pc, idx)) return;
    int ci = pc->list[idx];
    recent_add(&D.ch[ci]);
    pc->cur = idx;
    osd_channel(pc, idx, "show");
}

static void on_preview(void *ud, int idx) {
    PlayCtx *pc = ud;
    if (pc_index(pc, idx)) osd_channel(pc, idx, "zap");
}

static void on_tick(void *ud) {
    PlayCtx *pc = ud;
    if (!pc_index(pc, pc->shown)) { sepg_drain(-1); return; }
    if (sepg_drain(pc->list[pc->shown])) osd_channel(pc, pc->shown, "update");
}

static void play_list(int *list, int n, int start, int gen) {
    if (n <= 0 || gen != D.gen) return;
    for (int i = 0; i < n; i++) if (list[i] < 0 || list[i] >= D.n_ch) return;
    if (start < 0 || start >= n) start = 0;
    const char *path = "/tmp/notatv-iptv.m3u";
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "#EXTM3U\n");
    for (int i = 0; i < n; i++) {
        Channel *c = &D.ch[list[i]];
        fprintf(f, "#EXTINF:-1,%d %s\n%s\n", i + 1, DNAME(c), c->url);
    }
    fclose(f);
    PlayCtx pc = {list, n};
    recent_add(&D.ch[list[start]]);
    PlayRequest rq = {0};
    rq.url = path;
    rq.title = NULL;
    rq.subtitle = D.ch[list[start]].group;
    rq.is_live = true;
    rq.playlist_start = start;
    rq.channel_changed = on_channel;
    rq.channel_preview = on_preview;
    rq.playlist_n = n;
    rq.ud = &pc;
    static const char *extra[] = {"--loop-playlist=inf", "--prefetch-playlist=no", "--user-agent=" IPTV_UA, NULL};
    rq.extra = extra;
    pc.cur = start;
    pc.gen = gen;
    rq.tick = on_tick;
    osd_channel(&pc, start, "show"); /* the banner for the first channel (mpv is not running yet - read at file-loaded) */
    player_run(&rq);
}

void iptv_play_recent(int idx) {
    recent_load();
    if (idx < 0 || idx >= n_recent) return;
    /* find the channel in the loaded list to get channel changes: by address, otherwise by name
       (some providers give new addresses when the list is fetched again) */
    if (D.n_ch) {
        int start = -1;
        for (int i = 0; i < D.n_ch && start < 0; i++)
            if (!strcmp(D.ch[i].url, recents[idx].url)) start = i;
        for (int i = 0; i < D.n_ch && start < 0; i++)
            if (!strcmp(DNAME(&D.ch[i]), recents[idx].name)) start = i;
        if (start >= 0) {
            int *all = malloc(sizeof(int) * D.n_ch);
            if (all) {
                for (int i = 0; i < D.n_ch; i++) all[i] = i;
                play_list(all, D.n_ch, start, D.gen);
                free(all);
                return;
            }
        }
    } else iptv_load(false); /* the list was not loaded yet (Live TV not opened since start): next time it is */
    /* only this channel: a banner of its own, so the OSD does not show the channel from the last time */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "num", 1);
    cJSON_AddStringToObject(o, "name", recents[idx].name);
    cJSON_AddNumberToObject(o, "pos", 1);
    cJSON_AddNumberToObject(o, "total", 1);
    cJSON_AddNumberToObject(o, "now_t", (double)time(NULL));
    osd_logo(o, recents[idx].logo, 0);
    osd_write(o, "show");
    PlayRequest rq = {0};
    rq.url = recents[idx].url;
    rq.title = recents[idx].name;
    rq.is_live = true;
    static const char *extra[] = {"--user-agent=" IPTV_UA, NULL};
    rq.extra = extra;
    player_run(&rq);
}

static void url_entered(void *ud, const char *text) {
    (void)ud;
    if (!text) return;
    char *t = strdup(text);
    str_trim(t);
    cfg_set("iptv_url", t);
    free(t);
    iptv_load(true);
}

static void epg_entered(void *ud, const char *text) {
    (void)ud;
    if (!text) return;
    char *t = strdup(text);
    str_trim(t);
    cfg_set("iptv_epg", t);
    free(t);
    char path[512];
    snprintf(path, sizeof path, "%s/epg.xml", data_dir());
    unlink(path);
    free(D.epg_url);
    D.epg_url = epg_url_cfg();
    int with_sid = 0;
    for (int i = 0; i < D.n_ch; i++) with_sid += D.ch[i].sid > 0;
    D.short_epg = !*cfg_get("iptv_epg", "") && with_sid > D.n_ch / 2;
    epg_start();
}

/* ---------- Xtream accounts: the one in use (cfg xtream_*) and the others that have been added ---------- */
typedef struct { char server[200], user[100], pass[100]; } XAcc;
#define MAX_XACC 8
static XAcc xacc[MAX_XACC];
static int n_xacc;

static void xacc_path(char *p, size_t n) { snprintf(p, n, "%s/xtream-accounts.json", data_dir()); }
static void xacc_save(void) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n_xacc; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "server", xacc[i].server);
        cJSON_AddStringToObject(o, "user", xacc[i].user);
        cJSON_AddStringToObject(o, "pass", xacc[i].pass);
        cJSON_AddItemToArray(arr, o);
    }
    char *js = cJSON_PrintUnformatted(arr), path[512], tmp[540];
    cJSON_Delete(arr);
    xacc_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) { fputs(js, f); fclose(f); chmod(tmp, 0600); rename(tmp, path); }
    free(js);
}
static int xacc_find(const char *srv, const char *us) {
    for (int i = 0; i < n_xacc; i++) if (!strcmp(xacc[i].server, srv) && !strcmp(xacc[i].user, us)) return i;
    return -1;
}
static void xacc_put(const char *srv, const char *us, const char *pw) {
    int i = xacc_find(srv, us);
    if (i < 0 && n_xacc < MAX_XACC) i = n_xacc++;
    if (i < 0) i = MAX_XACC - 1; /* full: the last one is replaced */
    snprintf(xacc[i].server, sizeof xacc[i].server, "%s", srv);
    snprintf(xacc[i].user, sizeof xacc[i].user, "%s", us);
    snprintf(xacc[i].pass, sizeof xacc[i].pass, "%s", pw);
}
/* the saved list, plus the account in use (from before the list existed) */
static void xacc_load(void) {
    n_xacc = 0;
    char path[512];
    xacc_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        cJSON *arr = cJSON_Parse(buf), *o;
        cJSON_ArrayForEach(o, arr) {
            cJSON *sv = cJSON_GetObjectItem(o, "server"), *us = cJSON_GetObjectItem(o, "user"), *pw = cJSON_GetObjectItem(o, "pass");
            if (cJSON_IsString(sv) && cJSON_IsString(us) && n_xacc < MAX_XACC) xacc_put(sv->valuestring, us->valuestring, cJSON_IsString(pw) ? pw->valuestring : "");
        }
        cJSON_Delete(arr);
    }
    const char *srv = cfg_get("xtream_server", ""), *us = cfg_get("xtream_user", "");
    if (*srv && *us && xacc_find(srv, us) < 0) { xacc_put(srv, us, cfg_get("xtream_pass", "")); xacc_save(); }
}
static int xacc_active(void) { return xacc_find(cfg_get("xtream_server", ""), cfg_get("xtream_user", "")); }
static const char *host_of(const char *srv) { const char *h = strstr(srv, "://"); return h ? h + 3 : srv; }

/* use this account: new provider, new channels and program guide */
static void xacc_activate(const XAcc *a) {
    cfg_set("xtream_server", a->server);
    cfg_set("xtream_user", a->user);
    cfg_set("xtream_pass", a->pass);
    cfg_set("iptv_url", "");
    char path[512];
    snprintf(path, sizeof path, "%s/epg.xml", data_dir());
    unlink(path);
    iptv_load(true);
}

/* what the settings show: the account in use and when the subscription ends (asked from the provider now and then) */
static struct { char key[320]; char line[220]; double t; bool busy; } acc_info;
typedef struct { char key[320], url[700], user[100], host[120]; char line[220]; } AccJob;

static char *fmt_expiry(char *out, size_t n, long e) {
    time_t t = e;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%d.%m.%Y", &tm);
    return out;
}
static void acc_work(void *arg) {
    AccJob *j = arg;
    static const char *const hdr[] = {"User-Agent: " IPTV_UA, NULL};
    HttpBuf b;
    char who[240];
    snprintf(who, sizeof who, "%s @ %s", j->user, j->host);
    if (http_request("GET", j->url, hdr, NULL, &b, 12) && b.data) {
        cJSON *root = cJSON_Parse(b.data);
        cJSON *ui = cJSON_GetObjectItem(root, "user_info");
        if (ui) {
            cJSON *st = cJSON_GetObjectItem(ui, "status"), *exp = cJSON_GetObjectItem(ui, "exp_date");
            const char *status = cJSON_IsString(st) ? st->valuestring : "Active";
            long e = cJSON_IsString(exp) ? atol(exp->valuestring) : cJSON_IsNumber(exp) ? (long)exp->valuedouble : 0;
            char d[32];
            if (e > 0) fmt_expiry(d, sizeof d, e);
            if (e > 0 && (e < time(NULL) || !strcmp(status, "Expired"))) snprintf(j->line, sizeof j->line, T("%s - expired %s"), who, d);
            else if (strcmp(status, "Active")) snprintf(j->line, sizeof j->line, T("%s - not active (%s)"), who, status);
            else if (e > 0) snprintf(j->line, sizeof j->line, T("%s - until %s"), who, d);
            else snprintf(j->line, sizeof j->line, T("%s - no expiry date"), who);
        }
        cJSON_Delete(root);
    }
    http_free(&b);
}
static void acc_done(void *arg) {
    AccJob *j = arg;
    acc_info.busy = false;
    acc_info.t = app_time();
    if (!strcmp(j->key, acc_info.key) && j->line[0]) snprintf(acc_info.line, sizeof acc_info.line, "%s", j->line);
    free(j);
}
const char *iptv_account_line(void) {
    const char *srv = cfg_get("xtream_server", ""), *us = cfg_get("xtream_user", "");
    if (!*srv || !*us) return *cfg_get("iptv_url", "") ? T("M3U playlist link") : T("Not signed in");
    char key[320];
    snprintf(key, sizeof key, "%s|%s", srv, us);
    if (strcmp(key, acc_info.key)) {
        snprintf(acc_info.key, sizeof acc_info.key, "%s", key);
        acc_info.line[0] = 0;
        acc_info.t = 0;
    }
    if (!acc_info.busy && (acc_info.t == 0 || app_time() - acc_info.t > 300)) {
        char *url = xtream_url("player_api.php", NULL);
        if (url) {
            AccJob *j = calloc(1, sizeof *j);
            snprintf(j->key, sizeof j->key, "%s", key);
            snprintf(j->url, sizeof j->url, "%s", url);
            snprintf(j->user, sizeof j->user, "%s", us);
            snprintf(j->host, sizeof j->host, "%s", host_of(srv));
            free(url);
            acc_info.busy = true;
            job_submit(acc_work, acc_done, j);
        }
    }
    static char shown[260];
    if (acc_info.line[0]) return acc_info.line;
    snprintf(shown, sizeof shown, "%s @ %s", us, host_of(srv));
    return shown;
}

/* verify the login against the provider's API before we save it. The account in use is left alone until it works. */
typedef struct { char url[700]; long status; char msg[200]; bool ok; XAcc acc; } XtJob;

static void xt_check_work(void *arg) {
    XtJob *j = arg;
    static const char *const hdr[] = {"User-Agent: " IPTV_UA, NULL};
    HttpBuf b;
    if (!http_request("GET", j->url, hdr, NULL, &b, 25)) {
        snprintf(j->msg, sizeof j->msg, T("Cannot reach the server - check the address"));
        http_free(&b);
        return;
    }
    j->status = b.status;
    cJSON *root = b.data ? cJSON_Parse(b.data) : NULL;
    cJSON *ui = cJSON_GetObjectItem(root, "user_info");
    cJSON *auth = cJSON_GetObjectItem(ui, "auth");
    bool authed = cJSON_IsNumber(auth) ? auth->valueint == 1 : (cJSON_IsString(auth) && !strcmp(auth->valuestring, "1"));
    if (!root) snprintf(j->msg, sizeof j->msg, b.status == 200 ? T("The server did not respond like an Xtream server") :
                        T("The server rejected the request (HTTP %ld)"), b.status);
    else if (!authed) snprintf(j->msg, sizeof j->msg, T("Wrong username or password"));
    else {
        cJSON *st = cJSON_GetObjectItem(ui, "status"), *exp = cJSON_GetObjectItem(ui, "exp_date");
        const char *status = cJSON_IsString(st) ? st->valuestring : "Active";
        if (strcmp(status, "Active")) snprintf(j->msg, sizeof j->msg, T("The account is not active (%s)"), status);
        else {
            j->ok = true;
            long e = cJSON_IsString(exp) ? atol(exp->valuestring) : cJSON_IsNumber(exp) ? (long)exp->valuedouble : 0;
            if (e > 0) {
                char d[32];
                snprintf(j->msg, sizeof j->msg, T("Signed in - subscription until %s"), fmt_expiry(d, sizeof d, e));
            } else snprintf(j->msg, sizeof j->msg, T("Signed in"));
        }
    }
    cJSON_Delete(root);
    http_free(&b);
}

static void xt_check_done(void *arg) {
    XtJob *j = arg;
    app_toast("%s", j->msg);
    if (j->ok) {
        xacc_load();
        xacc_put(j->acc.server, j->acc.user, j->acc.pass);
        xacc_save();
        xacc_activate(&j->acc);
        acc_info.t = 0;
    }
    free(j);
}

static char pend_server[200], pend_user[100];
static void xt_pass(void *ud, const char *text) {
    (void)ud;
    if (!text) return;
    XtJob *j = calloc(1, sizeof *j);
    snprintf(j->acc.server, sizeof j->acc.server, "%s", pend_server);
    snprintf(j->acc.user, sizeof j->acc.user, "%s", pend_user);
    snprintf(j->acc.pass, sizeof j->acc.pass, "%s", text);
    char *u = xtream_url_for(pend_server, pend_user, text, "player_api.php", NULL);
    if (!u) { free(j); return; }
    snprintf(j->url, sizeof j->url, "%s", u);
    free(u);
    app_toast(T("Signing in…"));
    job_submit(xt_check_work, xt_check_done, j);
}
static void xt_user(void *ud, const char *text) {
    (void)ud;
    if (!text || !*text) return;
    snprintf(pend_user, sizeof pend_user, "%s", text);
    keyboard_open(T("Xtream: password"), "", true, xt_pass, NULL);
}
static void xt_server(void *ud, const char *text) {
    (void)ud;
    if (!text || !*text) return;
    char *base = xtream_base(text);
    snprintf(pend_server, sizeof pend_server, "%s", base);
    free(base);
    keyboard_open(T("Xtream: username"), "", false, xt_user, NULL);
}

static void switch_done(void *ud, int idx) {
    (void)ud;
    if (idx < 0 || idx >= n_xacc || idx == xacc_active()) return;
    xacc_activate(&xacc[idx]);
    acc_info.t = 0;
    app_toast(T("Switched to %s"), xacc[idx].user);
}
void iptv_account_switch(void) {
    xacc_load();
    if (n_xacc < 2) { app_toast(T("Only one account - add another first")); return; }
    static char lbl[MAX_XACC][240];
    const char *items[MAX_XACC];
    for (int i = 0; i < n_xacc; i++) {
        snprintf(lbl[i], sizeof lbl[i], "%s @ %s", xacc[i].user, host_of(xacc[i].server));
        items[i] = lbl[i];
    }
    choice_open(T("Switch account"), items, n_xacc, xacc_active() < 0 ? 0 : xacc_active(), switch_done, NULL);
}

/* what Live TV has stored from a provider: the playlist and guide (with the login in the stream addresses),
   favorites and recent channels (full stream addresses) */
static void iptv_wipe_data(void) {
    const char *const files[] = {"iptv.m3u", "iptv.url", "epg.xml", "epg.cache", "iptv-fav.txt", "iptv-recent.txt", NULL};
    for (int i = 0; files[i]; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s", data_dir(), files[i]);
        wipe_file(p);
    }
    for (int i = 0; i < n_favs; i++) free(favs[i]);
    free(favs);
    favs = NULL;
    n_favs = 0;
    for (int i = 0; i < n_recent; i++) { free(recents[i].name); free(recents[i].url); free(recents[i].logo); }
    n_recent = 0;
    /* the channel list in memory */
    iptv_epoch++;
    free_channels(D.ch, D.n_ch);
    D.ch = NULL;
    D.n_ch = 0;
    for (int i = 0; i < D.n_groups; i++) free(D.groups[i]);
    free(D.groups);
    D.groups = NULL;
    D.n_groups = 0;
    free(D.epg_url);
    D.epg_url = NULL;
    D.loading = false;
    D.status[0] = 0;
    D.gen++;
    build_countries();
    rebuild_vis();
}

/* sign out of Live TV completely: all accounts, the playlist/guide links and everything above */
void iptv_forget_all(void) {
    n_xacc = 0;
    char p[512];
    xacc_path(p, sizeof p);
    wipe_file(p);
    cfg_set("xtream_server", "");
    cfg_set("xtream_user", "");
    cfg_set("xtream_pass", "");
    cfg_set("iptv_url", "");
    cfg_set("iptv_epg", "");
    iptv_wipe_data();
    acc_info.key[0] = acc_info.line[0] = 0;
    wipe_session_traces();
}

static void signout_done(void *ud, bool yes) {
    (void)ud;
    if (!yes) return;
    xacc_load();
    int a = xacc_active();
    if (a < 0) { /* an M3U link, not an Xtream account: only that is signed out */
        cfg_set("iptv_url", "");
        cfg_set("iptv_epg", "");
        iptv_wipe_data();
        wipe_session_traces();
        app_toast(T("Signed out"));
        return;
    }
    if (a >= 0) { memmove(&xacc[a], &xacc[a + 1], sizeof(XAcc) * (n_xacc - a - 1)); n_xacc--; }
    xacc_save();
    if (n_xacc > 0) {
        /* the signed-out account's playlist, guide, favorites and recent channels are deleted before the next one is loaded */
        iptv_wipe_data();
        wipe_session_traces();
        XAcc next = xacc[0];
        xacc_activate(&next);
        app_toast(T("Signed out - now using %s"), next.user);
    } else {
        iptv_forget_all();
        app_toast(T("Signed out"));
    }
    acc_info.t = 0;
}
void iptv_account_signout(void) {
    if (!*cfg_get("xtream_user", "") && !*cfg_get("iptv_url", "")) { app_toast(T("Not signed in")); return; }
    confirm_open(T("Sign out of this account?"), T("Its channels, favorites and history are deleted. Other accounts you have added stay saved."), T("Sign out"), T("Cancel"), signout_done, NULL);
}

static void menu_done(void *ud, int idx) {
    (void)ud;
    switch (idx) {
    case 0: iptv_load(true); app_toast(T("Updating channels…")); break;
    case 1: keyboard_open(T("Playlist URL (M3U)"), cfg_get("iptv_url", "http://"), false, url_entered, NULL); break;
    case 2: iptv_xtream_login(); break;
    case 3: keyboard_open(T("Program guide URL (XMLTV)"), cfg_get("iptv_epg", D.epg_url ? D.epg_url : "http://"), false, epg_entered, NULL); break;
    default: break;
    }
}

void iptv_xtream_login(void) {
    keyboard_open(T("Xtream: server address (e.g. host.example:8080)"), cfg_get("xtream_server", ""), false, xt_server, NULL);
}

static void setup_done(void *ud, int idx) {
    (void)ud;
    if (idx == 0) iptv_xtream_login();
    else if (idx == 1) keyboard_open(T("Playlist URL (M3U)"), cfg_get("iptv_url", "http://"), false, url_entered, NULL);
}

static void play_current(Iptv *v) {
    if (v->built_gen != D.gen) build_list(v); /* called before the frame's rebuild: the old list is invalid */
    if (!v->n_list || v->csel < 0 || v->csel >= v->n_list) return;
    int *copy = malloc(sizeof(int) * v->n_list);
    memcpy(copy, v->list, sizeof(int) * v->n_list);
    play_list(copy, v->n_list, v->csel, D.gen);
    free(copy);
}

static Iptv *menu_owner;
static void guide_shown(void *ud, int idx) { (void)ud; (void)idx; }

static void show_guide(Channel *c) {
    time_t now = time(NULL);
    char *items[24];
    int n = 0;
    for (int i = 0; i < c->n_progs && n < 24; i++) {
        if (c->progs[i].stop < now) continue;
        char a[8], b[8];
        struct tm t1, t2;
        localtime_r(&c->progs[i].start, &t1);
        localtime_r(&c->progs[i].stop, &t2);
        strftime(a, sizeof a, "%H:%M", &t1);
        strftime(b, sizeof b, "%H:%M", &t2);
        items[n++] = str_printf("%s-%s  %s", a, b, c->progs[i].title);
    }
    if (n == 0) { app_toast(T("No program guide for %s"), DNAME(c)); return; }
    choice_open(DNAME(c), (const char *const *)items, n, 0, guide_shown, NULL);
    for (int i = 0; i < n; i++) free(items[i]);
}

static void ch_menu_done(void *ud, int idx) {
    Iptv *v = ud;
    if (v != menu_owner) return;
    if (v->built_gen != D.gen) build_list(v); /* the channel list changed while the menu was open */
    if (!v->n_list || v->csel < 0 || v->csel >= v->n_list) return;
    Channel *c = &D.ch[v->list[v->csel]];
    if (idx == 0) play_current(v);
    else if (idx == 1) app_toast(toggle_fav(c->url) ? T("%s added to favorites") : T("%s removed from favorites"), DNAME(c));
    else if (idx == 2) show_guide(c);
    else if (idx == 3) { v->moving = true; v->col = 1; app_toast(T("Move with up/down - press OK when you are done")); }
}

/* swap two favorites (in the list and in the file) */
static void fav_swap(Iptv *v, int a, int b) {
    if (a < 0 || b < 0 || a >= v->n_list || b >= v->n_list) return;
    const char *ua = D.ch[v->list[a]].url, *ub = D.ch[v->list[b]].url;
    int ia = -1, ib = -1;
    for (int i = 0; i < n_favs; i++) { if (!strcmp(favs[i], ua)) ia = i; if (!strcmp(favs[i], ub)) ib = i; }
    if (ia < 0 || ib < 0) return;
    char *t = favs[ia]; favs[ia] = favs[ib]; favs[ib] = t;
    int l = v->list[a]; v->list[a] = v->list[b]; v->list[b] = l;
    favs_save();
}

static void open_channel_menu(Iptv *v) {
    if (!v->n_list) return;
    Channel *c = &D.ch[v->list[v->csel]];
    const char *items[4] = {T("Watch channel"), is_fav(c->url) ? T("Remove from favorites") : T("Add to favorites"), T("Program schedule"),
                            T("Move favorite")};
    menu_owner = v;
    choice_open(DNAME(c), items, v->gsel == 0 && v->n_list > 1 ? 4 : 3, 0, ch_menu_done, v);
}

static bool iptv_input(Screen *s, const InputEvent *ev) {
    Iptv *v = s->st;
    bool configured = iptv_configured();
    if (ev->a == A_MENU || (!configured && ev->a == A_INFO)) {
        const char *items[] = {T("Update channels now"), T("Change playlist (M3U link)"), T("Xtream Codes login"), T("Program guide (EPG) URL")};
        choice_open("Live TV", items, 4, 0, menu_done, NULL);
        return true;
    }
    if (!configured || D.n_ch == 0) {
        if (!configured && buttons_nav(&setup_btn, 2, ev)) return true;
        if (ev->a == A_OK) {
            if (ev->repeat) return true;
            if (!configured) setup_done(NULL, setup_btn);
            else { /* playlist given, but no channels (error/loading): choose again */
                const char *items[] = {T("Sign in with Xtream Codes"), T("Enter playlist link (M3U)")};
                choice_open(T("Add IPTV"), items, 2, 0, setup_done, NULL);
            }
            return true;
        }
        return false;
    }
    if (ev->a == A_UP || ev->a == A_DOWN || ev->a == A_LEFT || ev->a == A_RIGHT ||
        ev->a == A_CHUP || ev->a == A_CHDOWN) v->user_moved = true;
    if (v->built_gen != D.gen) build_list(v); /* new channel list: the indexes from the old one must not be used */
    if (v->moving) { /* move mode: up/down moves the favorite, anything else ends it */
        if (v->gsel != 0 || v->col != 1) v->moving = false;
        else if (ev->a == A_UP && v->csel > 0) { fav_swap(v, v->csel, v->csel - 1); v->csel--; return true; }
        else if (ev->a == A_DOWN && v->csel < v->n_list - 1) { fav_swap(v, v->csel, v->csel + 1); v->csel++; return true; }
        else if (ev->a == A_UP || ev->a == A_DOWN) return true;
        else if (!ev->repeat) { v->moving = false; return true; }
        else return true;
    }
    switch (ev->a) {
    case A_UP:
        if (v->col == 0) { if (v->gsel > 0) v->gsel--; }
        else if (v->csel > 0) v->csel--;
        return true;
    case A_DOWN:
        if (v->col == 0) { if (v->gsel >= n_groups_ui() - 1) return false; v->gsel++; } /* bottom: the key bar */
        else { if (v->csel >= v->n_list - 1) return false; v->csel++; }
        return true;
    case A_CHUP: v->csel = v->csel > 8 ? v->csel - 8 : 0; return true;
    case A_CHDOWN: v->csel = v->csel + 8 < v->n_list ? v->csel + 8 : (v->n_list ? v->n_list - 1 : 0); return true;
    case A_LEFT:
        if (v->col == 1 && v->gsel == 1 && v->n_list) { /* TV guide: the program before, or out to the left column */
            const Prog *p = prog_at(&D.ch[v->list[v->csel]], v->gtime, 0);
            time_t now = time(NULL);
            if (v->gtime > now) { v->gtime = p ? p->start - 1 : v->gtime - 1800; if (v->gtime < now) v->gtime = now; return true; }
        }
        if (v->col == 1) v->col = 0;
        return true;
    case A_RIGHT:
        if (v->col == 1 && v->gsel == 1) { /* TV guide: the next program */
            if (!v->n_list) return true;
            Channel *c = &D.ch[v->list[v->csel]];
            const Prog *p = prog_at(c, v->gtime, 0);
            if (p && prog_at(c, p->stop, 0)) v->gtime = p->stop;
            else if (!p && v->gtime < time(NULL) + 6 * 3600) v->gtime += 1800;
            return true;
        }
        if (v->col == 0 && v->n_list > 0) { v->col = 1; v->gtime = time(NULL); }
        else if (v->col == 1 && v->gsel == 0 && v->n_list > 1 && !ev->repeat) v->moving = true; /* pick up a favorite */
        return true;
    case A_OK:
        if (v->col == 0 && ui_country(v->gsel) >= 0) { /* country heading: expand/collapse */
            if (!ev->repeat) { D.cexp[ui_country(v->gsel)] ^= 1; rebuild_vis(); }
        } else if (v->col == 0) { if (!ev->repeat) { if (v->n_list) v->col = 1; v->csel = 0; v->gtime = time(NULL); } }
        else if (v->n_list) {
            if (!ev->repeat) v->ok_pending = app_time();          /* wait: short press or held? */
            else if (v->ok_pending) { v->ok_pending = 0; open_channel_menu(v); }
        }
        return true;
    case A_INFO: /* green: channel menu (same as held OK) */
        if (v->col == 1 && v->n_list) open_channel_menu(v);
        return true;
    case A_YELLOW:
        if (v->col == 1 && v->n_list && !ev->repeat) {
            Channel *c = &D.ch[v->list[v->csel]];
            app_toast(toggle_fav(c->url) ? T("%s added to favorites") : T("%s removed from favorites"), DNAME(c));
        }
        return true;
    case A_DIGIT: case A_CHAR:
        if (ev->ch >= '0' && ev->ch <= '9' && v->col == 1) {
            size_t l = strlen(v->digits);
            if (l < 4) { v->digits[l] = (char)ev->ch; v->digits[l + 1] = 0; }
            v->digit_t = app_time();
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void iptv_destroy(Screen *s) {
    Iptv *v = s->st;
    if (menu_owner == v) menu_owner = NULL;
    free(v->list);
    free(v);
    free(s);
}

Screen *iptv_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "iptv";
    s->st = calloc(1, sizeof(Iptv));
    s->enter = iptv_enter;
    s->update = iptv_update;
    s->draw = iptv_draw;
    s->input = iptv_input;
    s->destroy = iptv_destroy;
    return s;
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
static void act_xtream(void) { iptv_xtream_login(); }
static const char *iptv_signed_out(void) { return *cfg_get("xtream_user", "") || *cfg_get("iptv_url", "") ? NULL : ITEM_HIDDEN; }
/* "Switch account" only with two or more accounts (the list is read at most once a second) */
static const char *iptv_one_account(void) {
    static double next;
    static int n;
    if (app_time() >= next) { next = app_time() + 1; xacc_load(); n = n_xacc; }
    return n >= 2 ? NULL : ITEM_HIDDEN;
}
static void apply_iptv(void) { app_toast(T("Playlist saved - open Live TV")); }
static const Item live_items[] = {
    {N_("Account"), IT_INFO, .value = iptv_account_line},
    {N_("Add account"), IT_ACTION, .action = act_xtream, .help = N_("Xtream Codes login from your IPTV provider. The current account stays until the new one works")},
    {N_("Switch account"), IT_ACTION, .action = iptv_account_switch, .help = N_("Choose between the accounts you have added"), .locked = iptv_one_account},
    {N_("Sign out of this account"), IT_ACTION, .action = iptv_account_signout, .locked = iptv_signed_out},
    {N_("IPTV playlist (M3U link)"), IT_TEXT, "iptv_url", "", .changed = apply_iptv},
    {N_("IPTV program guide (XMLTV)"), IT_TEXT, "iptv_epg", ""},
};

static void iptv_row_open(const HomeItem *it, int idx) { (void)idx; if (it->key && !strncmp(it->key, "iptv:", 5)) iptv_play_recent(atoi(it->key + 5)); }
/* the big top on the home screen: the latest channel with its logo (press down for the others) */
static bool iptv_draw_shelf(float x, float y) {
    HomeItem *items = NULL;
    if (iptv_recent(&items) <= 0 || !items) return false;
    HomeItem *it = &items[0];
    Color accent = color_mix(RGB(43, 95, 217), C_TEXT, 0.45f);
    y += 10;
    if (logo_card()) {
        draw_shadow(x, y, 360, 200, 28, 30, 1);
        fill_rrect(x, y, 360, 200, 28, RGB(235, 238, 244));
    }
    if (!img_draw(chan_logo(it->image), x + 30, y + 30, 300, 140, 14, 1))
        text_draw_clip(F_BOLD, 34, x + 180, y + 78, 320, logo_card() ? RGB(60, 60, 70) : C_TEXT, AL_CENTER, it->title ? it->title : "");
    text_draw(F_BOLD, 24, x + 410, y + 34, accent, AL_LEFT, T("RECENTLY WATCHED"));
    text_draw_clip(F_BOLD, 60, x + 410, y + 70, 1100, C_TEXT, AL_LEFT, it->title ? it->title : "");
    text_draw(F_REG, 28, x + 410, y + 150, C_TEXT2, AL_LEFT, T("Live TV  ·  press down for your recent channels"));
    return true;
}
const NotaApp nota_app = {
    NOTA_API, "iptv", N_("Live TV"), N_("Watch live TV and channels from your IPTV provider"), APP_VERSION, 10, APP_ROW_LOGOS,
    ICON_TV, {43, 95, 217, 255}, {22, 52, 140, 255}, {255, 255, 255, 255},
    .open = iptv_screen, .forget = iptv_forget_all,
    .settings = live_items, .n_settings = sizeof live_items / sizeof live_items[0],
    .row_title = N_("Recent channels"), .row = iptv_recent, .row_open = iptv_row_open, .draw_shelf = iptv_draw_shelf,
};
