/* NotaTV app package "tv2" - TV 2 Play via Widevine (wvserve-hjaelper + player_run).
 *
 * Login: Auth0 Device Code Flow (the same public client as the TV 2 Play TV app).
 * Browse: the private pages-graph / play API that the web player uses. The app only
 * uses the user's own TV 2 subscription; the Widevine module is never shipped here.
 *
 * The playback engine (Widevine module + decryption) lives in wvserve in the app's
 * data folder. This file is the UI, login, browse, search and playback hand-off.
 */
#include "tv2.h"
#include "vendor/cJSON.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* TV2_CLIENT_ID: the TV 2 TV app's public Auth0 client id, from the private file secrets/tv2-client-id (see app.mk) */
#define TV2_AUDIENCE "https://play.tv2.dk"
#define AUTH_TOKEN_URL "https://auth.tv2.dk/oauth/token"
#define AUTH_DEVICE_URL "https://auth.tv2.dk/oauth/device/code"
#define PLAY_API "https://play.tv2api.dk/"
#define PAGES_API "https://pages-graph.discovery.tv2api.dk/graphql"
#define USER_AGENT "NotaTV TV 2 Play"
#define APP_VER "2026.11.7021017"
#define PORT 8888

#define CARD_W 300.0f
#define CARD_H 169.0f
#define CARD_GAP 24.0f
#define CARD_COLS 5
#define SEC_H 300.0f
#define MAX_SEC 64

enum { S_LOGIN, S_CODE, S_LOADING, S_READY, S_ERROR };
enum { CARD_VOD, CARD_SERIES, CARD_COLLECTION, CARD_LIVE };
enum { FMT_LANDSCAPE, FMT_POSTER };
enum { SZ_SMALL, SZ_MEDIUM, SZ_LARGE };
enum { M_HOME, M_LIVE, M_CATEGORIES, M_SEARCH, M_MYPAGE, M_LOGOUT };
enum { V_HOME, V_BROWSE, V_DETAIL, V_SEARCH, V_LIVE, V_CATEGORIES, V_PROFILE, V_WHO };

#define MAX_PROF 8
typedef struct { char name[96], token[2048], id[64], avatar[1024]; bool kids; } Tv2Prof;
enum { SF_ALL, SF_FILM, SF_SERIES };

typedef struct {
    char *guid, *path, *title, *sub, *img, *lock, *label, *label_color, *acc;
    int type, fmt, size;
    double progress;
    int ent; /* 0 unknown, 1 allowed, 2 rental/purchase, 3 not checked */
} Card;

typedef struct {
    char *title;
    Card *cards;
    int n, cap, off, fmt, size;
} Section;

typedef struct {
    char *guid, *path, *title, *sub, *img, *logo, *titleart, *cta, *cta_path;
    char *label, *label_color, *acc;
    int cta_ent;
} Hero;

#define MAX_HERO 8
#define MAX_HIST 8

typedef struct {
    int state, view;
    char msg[320], heading[240], query[240], path[512];
    char token[4096], refresh[4096], profile[2048], profile_name[160], profile_avatar[1024];
    double token_exp;
    bool polling, auth_retried;
    char device_code[256], user_code[64], verify_uri[512], verify_complete[2048];
    double poll_next, code_expires, interval;
    Section sec[MAX_SEC];
    Hero hero[MAX_HERO];
    int nhero, hero_i;
    bool hero_focus;
    int nsec, row, col;
    float scroll;
    SideMenu side;
    int side_map[8];
    int gen;
    /* details */
    char detail_desc[1400], detail_img[1024], detail_cta[96], detail_cta_path[512];
    int detail_cta_ent;
    bool detail_focus;
    /* search */
    int search_filter, hist_sel;
    bool search_home;
    char hist[MAX_HIST][240];
    int nhist;
    int profile_sel;
    /* the account's profiles ("Hvem ser med?") */
    Tv2Prof prof[MAX_PROF];
    int nprof, who_sel;
    bool force_pick; /* "Skift profil": always show the picker */
    bool bg_profiles; /* profiles refreshed in the background on open: only update name/avatar/token */
} Tv2;

typedef struct Tv2Req Tv2Req;
struct Tv2Req {
    char *url, *body, *token, *profile;
    const char *method;
    bool form, api;
    cJSON *json;
    long status;
    int gen;
    Tv2 *owner; /* screen that owns the request (entitlement jobs use -1) */
    void (*done)(void *ud, cJSON *json, long status);
    void *ud;
};

#define ENT_BATCH 8
typedef struct {
    Tv2 *v;
    int gen, view, n;
    char guid[ENT_BATCH][64];
    char path[ENT_BATCH][512];
} EntJob;

static Tv2 *live;
/* TV 2 Google TV palette (from the APK's colors.xml) */
static const Color TV2_RED = {240, 40, 30, 255};
static const Color TV2_MIDNIGHT = {0, 5, 35, 255};
static const Color TV2_NAVY = {10, 15, 44, 255};
static const Color TV2_SURFACE = {20, 25, 53, 255};
static const Color TV2_SURFACE2 = {31, 35, 61, 255};
static const Color TV2_TEXT = {255, 255, 255, 255};
static const Color TV2_TEXT2 = {194, 195, 202, 255};
static const Color TV2_TEXT3 = {112, 115, 132, 255};
static const Color TV2_SAND = {255, 245, 190, 255};
static const Color TV2_BLACK = {0, 0, 0, 255};

/* Tokenized type scale. TV 2 uses 7/9/12/14/18/30sp; these are the
 * 1080p pixel equivalents that fit NotaTV's renderer. */
#define TS_HERO   52
#define TS_H1     34
#define TS_H2     24
#define TS_BODY   19
#define TS_LABEL  14
#define TS_SMALL  12
#define TS_BUTTON 20

#define HERO_Y 100.0f
#define HERO_H 500.0f
#define DETAIL_H 540.0f

/* ---------- small helpers ---------- */
static char *app_dir(void) { return str_printf("%s/apps/tv2", data_dir()); }
static char *app_file(const char *name) { return str_printf("%s/apps/tv2/%s", data_dir(), name); }

static void history_load(Tv2 *v) {
    v->nhist = 0;
    char *p = app_file("search_history");
    char *s = file_read_all(p, 8192);
    free(p);
    if (!s) return;
    char *save = NULL;
    for (char *line = strtok_r(s, "\n", &save); line && v->nhist < MAX_HIST; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) continue;
        snprintf(v->hist[v->nhist++], sizeof v->hist[0], "%s", line);
    }
    free(s);
}
static void history_save(Tv2 *v) {
    char *p = app_file("search_history");
    FILE *f = fopen(p, "w");
    if (f) {
        for (int i = 0; i < v->nhist; i++) fprintf(f, "%s\n", v->hist[i]);
        fclose(f);
    }
    free(p);
}
static void history_add(Tv2 *v, const char *q) {
    if (!q || !*q) return;
    int found = -1;
    for (int i = 0; i < v->nhist; i++) if (!strcmp(v->hist[i], q)) { found = i; break; }
    if (found >= 0) {
        for (int i = found; i > 0; i--) memmove(v->hist[i], v->hist[i - 1], sizeof v->hist[i]);
    } else {
        if (v->nhist < MAX_HIST) v->nhist++;
        for (int i = v->nhist - 1; i > 0; i--) memmove(v->hist[i], v->hist[i - 1], sizeof v->hist[i]);
    }
    snprintf(v->hist[0], sizeof v->hist[0], "%s", q);
    history_save(v);
}

static bool file_exists(const char *path) {
    return path && access(path, F_OK) == 0;
}

static char *jstr(cJSON *o, const char *key) {
    if (!o) return NULL;
    cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) && v->valuestring && v->valuestring[0] ? str_dup(v->valuestring) : NULL;
}
static double jnum(cJSON *o, const char *key) {
    if (!o) return 0;
    cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}
static const char *jval(cJSON *o, const char *key) {
    if (!o) return NULL;
    cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void set_error(Tv2 *v, const char *fmt, ...) {
    va_list ap;
    v->state = S_ERROR;
    v->polling = false;
    va_start(ap, fmt);
    vsnprintf(v->msg, sizeof v->msg, fmt, ap);
    va_end(ap);
}
static void set_loading(Tv2 *v, const char *msg) {
    v->state = S_LOADING;
    v->polling = false;
    snprintf(v->msg, sizeof v->msg, "%s", msg);
}

/* ---------- cards and sections ---------- */
static void card_free(Card *c) {
    free(c->guid); free(c->path); free(c->title); free(c->sub); free(c->img); free(c->lock);
    free(c->label); free(c->label_color); free(c->acc);
    memset(c, 0, sizeof *c);
}
static Color hex_color(const char *s, Color fallback) {
    if (!s || s[0] != '#') return fallback;
    unsigned r, g, b;
    if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) != 3) return fallback;
    return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
}
static bool text_is_light(Color c) { return (c.r * 299 + c.g * 587 + c.b * 114) > 145000; }

static bool image_is_poster(const char *url) {
    return url && (strstr(url, "ar=2%3A3") || strstr(url, "ar=2:3"));
}
static bool section_is_poster(const char *title, cJSON *cards) {
    if (title && (strstr(title, "film") || strstr(title, "Film"))) return true;
    int n = cJSON_GetArraySize(cards), posters = 0, seen = 0;
    for (int i = 0; i < n && seen < 8; i++) {
        cJSON *c = cJSON_GetArrayItem(cards, i);
        const char *u = jval(c, "imageUrl");
        if (!u) {
            cJSON *im = cJSON_GetObjectItem(c, "image");
            if (im) u = jval(im, "url");
        }
        if (!u) continue;
        seen++;
        if (image_is_poster(u)) posters++;
    }
    return seen >= 3 && posters * 2 >= seen;
}
static float card_width_for(int fmt, int size) {
    if (fmt == FMT_POSTER) return size == SZ_SMALL ? 160.0f : size == SZ_LARGE ? 240.0f : 200.0f;
    return size == SZ_SMALL ? 240.0f : size == SZ_LARGE ? 360.0f : 300.0f;
}
static float card_height_for(int fmt, int size) {
    float w = card_width_for(fmt, size);
    return fmt == FMT_POSTER ? w * 1.5f : w * 9.0f / 16.0f;
}
static float section_h(const Section *s) {
    if (!s || s->n <= 0) return 0;
    return card_height_for(s->fmt, s->size) + 132.0f;
}
static int section_cols(const Section *s) {
    if (!s || s->n <= 0) return CARD_COLS;
    float cw = card_width_for(s->fmt, s->size);
    int n = (int)((UI_W - (SIDE_W + 60) - 24) / (cw + CARD_GAP));
    if (n < 2) n = 2;
    return n > (s->fmt == FMT_POSTER ? 6 : CARD_COLS) ? (s->fmt == FMT_POSTER ? 6 : CARD_COLS) : n;
}
static void hero_free(Hero *h) {
    free(h->guid); free(h->path); free(h->title); free(h->sub); free(h->img); free(h->logo);
    free(h->titleart); free(h->cta); free(h->cta_path); free(h->label); free(h->label_color); free(h->acc);
    memset(h, 0, sizeof *h);
}
static void clear_hero(Tv2 *v) {
    for (int i = 0; i < v->nhero; i++) hero_free(&v->hero[i]);
    v->nhero = 0;
    v->hero_i = 0;
    v->hero_focus = false;
}
static void clear_sections(Tv2 *v) {
    for (int i = 0; i < v->nsec; i++) {
        Section *s = &v->sec[i];
        for (int j = 0; j < s->n; j++) card_free(&s->cards[j]);
        free(s->cards);
        free(s->title);
    }
    memset(v->sec, 0, sizeof v->sec);
    v->nsec = 0;
    v->row = v->col = 0;
    v->scroll = 0;
}
static Section *add_section(Tv2 *v, const char *title) {
    if (v->nsec >= MAX_SEC) return NULL;
    Section *s = &v->sec[v->nsec++];
    memset(s, 0, sizeof *s);
    s->title = str_dup(title && *title ? title : T("TV 2 Play"));
    s->fmt = FMT_LANDSCAPE;
    s->size = SZ_MEDIUM;
    return s;
}
/* The shell decodes images with stb_image, which cannot read WebP or SVG.
 * TV 2's imgix CDN can serve JPEG/PNG instead: fm=webp -> fm=jpg, and SVG
 * artwork -> the same URL with fm=png. Without this, the "Se videre" card
 * (fm=webp) and the category tiles (SVG) show only the fallback tile. */
static char *img_url_fix(const char *url) {
    if (!url || !*url) return NULL;
    char *s = str_dup(url);
    if (!s) return NULL;
    char *p;
    while ((p = strstr(s, "fm=webp"))) {
        memmove(p + 6, p + 7, strlen(p + 7) + 1); /* "fm=webp" (7) -> "fm=jpg" (6) */
        memcpy(p, "fm=jpg", 6);
    }
    if (strstr(s, ".svg") && !strstr(s, "fm=png")) {
        char *n = str_printf("%s%sfm=png", s, strchr(s, '?') ? "&" : "?");
        free(s);
        s = n;
    }
    return s;
}
static void add_card(Section *s, const char *guid, const char *path, const char *title,
                     const char *sub, const char *img, int type, double progress) {
    if (!s || (!title && !img)) return;
    if (s->n == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 16;
        Card *tmp = realloc(s->cards, sizeof(Card) * ncap);
        if (!tmp) return; /* keep the old buffer and drop this card on OOM */
        s->cards = tmp;
        s->cap = ncap;
    }
    Card *c = &s->cards[s->n++];
    memset(c, 0, sizeof *c);
    c->guid = str_dup(guid);
    c->path = str_dup(path);
    c->title = str_dup(title);
    c->sub = str_dup(sub);
    c->img = img_url_fix(img);
    c->type = type;
    c->progress = progress;
    c->fmt = s->fmt;
    c->size = s->size;
}
static void card_meta(Card *c, const char *label, const char *label_color, const char *acc) {
    if (!c) return;
    free(c->label); free(c->label_color); free(c->acc);
    c->label = str_dup(label);
    c->label_color = str_dup(label_color);
    c->acc = str_dup(acc);
}
static void compact_sections(Tv2 *v) {
    int old = v->nsec, w = 0;
    for (int i = 0; i < old; i++) {
        Section *s = &v->sec[i];
        if (s->n == 0) {
            free(s->cards);
            free(s->title);
            memset(s, 0, sizeof *s);
            continue;
        }
        if (w != i) {
            v->sec[w] = *s;
            memset(s, 0, sizeof *s);
        }
        w++;
    }
    v->nsec = w;
    if (v->row >= v->nsec) v->row = v->nsec > 0 ? v->nsec - 1 : 0;
    if (v->row < 0) v->row = 0;
    if (v->nsec > 0 && v->col >= v->sec[v->row].n) v->col = v->sec[v->row].n - 1;
    if (v->col < 0) v->col = 0;
}

static int card_type(const char *path, const char *dest_type) {
    if (dest_type && !strcmp(dest_type, "COLLECTION")) return CARD_COLLECTION;
    if (!path) return CARD_VOD;
    if (!strncmp(path, "/serie/", 7)) return CARD_SERIES;
    if (!strncmp(path, "/film/", 6) || !strncmp(path, "/program/", 9)) return CARD_VOD;
    if (!strncmp(path, "/afspil/", 8)) return CARD_VOD;
    if (!strncmp(path, "/event/", 7)) return CARD_LIVE;
    if (!strncmp(path, "/live/", 6)) return CARD_LIVE;
    return CARD_COLLECTION;
}

/* A content guid is passed to wvserve through a shell command (run_cmd uses
 * system()). Only allow characters that cannot break out of the single-quoted
 * argument, so a malformed path from the API can never inject a command. */
static bool guid_is_safe(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
            return false;
    return true;
}

static char *guid_from_play_url(const char *url) {
    if (!url) return NULL;
    const char *p = strstr(url, "/afspil/");
    p = p ? p + 8 : url;
    const char *slash = strrchr(p, '/');
    if (slash) p = slash + 1;
    size_t n = strcspn(p, "?#");
    return n ? str_printf("%.*s", (int)n, p) : NULL;
}

/* ---------- HTTP jobs ---------- */
static void treq_free(Tv2Req *r) {
    cJSON_Delete(r->json);
    free(r->url); free(r->body); free(r->token); free(r->profile);
    free(r);
}
static void treq_work(void *arg) {
    Tv2Req *r = arg;
    char auth[4300], prof[2200];
    const char *hdr_form[] = {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded", NULL};
    const char *hdr_api[16];
    int hn = 0;
    hdr_api[hn++] = "Accept: application/json";
    hdr_api[hn++] = "Content-Type: application/json";
    hdr_api[hn++] = "Client-Platform: Web";
    hdr_api[hn++] = "Client-Platform-Class: chrome";
    hdr_api[hn++] = "Client-Platform-Audience: PLAY";
    hdr_api[hn++] = "Client-Platform-OS-Version: chrome";
    hdr_api[hn++] = "Client-Platform-AppVersion: " APP_VER;
    hdr_api[hn++] = "client-platform-compatibility-version: 24";
    hdr_api[hn++] = "client-platform-refetch: 0";
    hdr_api[hn++] = "client-platform-timezone: -60";
    hdr_api[hn++] = "User-Agent: " USER_AGENT;
    if (r->token && *r->token) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", r->token);
        hdr_api[hn++] = auth;
    }
    if (r->profile && *r->profile) {
        snprintf(prof, sizeof prof, "Client-Platform-User-Profile: %s", r->profile);
        hdr_api[hn++] = prof;
    }
    hdr_api[hn] = NULL;

    HttpBuf b;
    memset(&b, 0, sizeof b);
    if (http_request(r->method, r->url, r->form ? hdr_form : hdr_api, r->body, &b, 25)) {
        r->status = b.status;
        if (b.data && b.len) r->json = cJSON_Parse(b.data);
    }
    if (getenv("TV2_DEBUG") && (r->status < 200 || r->status >= 300))
        fprintf(stderr, "tv2 debug: HTTP %ld %s -> %.1200s\n", r->status, r->url, b.data ? b.data : "");
    http_free(&b);
}
static void treq_done(void *arg) {
    Tv2Req *r = arg;
    if (r->gen < 0) {
        if (r->done) r->done(r->ud, r->json, r->status); /* entitlement jobs check ownership themselves */
    } else {
        Tv2 *v = r->owner;
        if (v == live && v->gen == r->gen && r->done) r->done(r->ud, r->json, r->status);
    }
    treq_free(r);
}
static void req_submit(Tv2Req *r) { job_submit(treq_work, treq_done, r); }

static void tv2_req(const char *url, const char *body, bool api,
                    void (*done)(void *, cJSON *, long), Tv2 *v) {
    Tv2Req *r = calloc(1, sizeof *r);
    r->method = "POST";
    r->url = str_dup(url);
    r->body = str_dup(body);
    r->form = !api;
    r->api = api;
    r->done = done;
    r->ud = v;
    r->owner = v;
    r->gen = v->gen;
    if (api) {
        r->token = str_dup(v->token);
        r->profile = str_dup(v->profile);
    }
    if (getenv("TV2_DEBUG"))
        fprintf(stderr, "tv2 debug: POST %s body=%s\n", url, body ? body : "");
    req_submit(r);
}
static void tv2_form(const char *url, const char *body, void (*done)(void *, cJSON *, long), Tv2 *v) {
    tv2_req(url, body, false, done, v);
}
static void tv2_api(const char *url, const char *body, void (*done)(void *, cJSON *, long), Tv2 *v) {
    tv2_req(url, body, true, done, v);
}
/* entitlement preflight: always deliver, so the job can free itself even if the screen is gone */
static void tv2_ent_api(const char *url, const char *body, void (*done)(void *, cJSON *, long), void *ud, Tv2 *v) {
    Tv2Req *r = calloc(1, sizeof *r);
    r->method = "POST";
    r->url = str_dup(url);
    r->body = str_dup(body);
    r->form = false;
    r->api = true;
    r->done = done;
    r->ud = ud;
    r->owner = v;
    r->gen = -1;
    r->token = str_dup(v->token);
    r->profile = str_dup(v->profile);
    if (getenv("TV2_DEBUG")) fprintf(stderr, "tv2 debug: ENT %s\n", body ? body : "");
    req_submit(r);
}

/* JSON body builders: cJSON does the escaping (query strings contain quotes). */
static char *graphql_body(const char *op, const char *query, cJSON *variables) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "operationName", op);
    cJSON_AddStringToObject(o, "query", query);
    if (variables) cJSON_AddItemToObject(o, "variables", variables);
    else cJSON_AddItemToObject(o, "variables", cJSON_CreateObject());
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

/* ---------- entitlement preflight for film cards ---------- */
static void set_cards_ent(Tv2 *v, const char *guid, int ent, const char *lock) {
    if (!guid || !*guid) return;
    for (int i = 0; i < v->nsec; i++)
        for (int j = 0; j < v->sec[i].n; j++) {
            Card *c = &v->sec[i].cards[j];
            if (!c->guid || strcmp(c->guid, guid)) continue;
            c->ent = ent;
            free(c->lock);
            c->lock = (ent == 2 && lock && *lock) ? str_dup(lock) : NULL;
        }
}
static void remove_denied_cards(Tv2 *v, bool hide) {
    if (!hide) return;
    for (int si = 0; si < v->nsec; si++) {
        Section *s = &v->sec[si];
        int w = 0;
        for (int i = 0; i < s->n; i++) {
            if (s->cards[i].ent == 2) { card_free(&s->cards[i]); continue; }
            if (w != i) s->cards[w] = s->cards[i];
            w++;
        }
        s->n = w;
    }
    compact_sections(v);
    if (v->nsec == 0) snprintf(v->msg, sizeof v->msg, T("No titles in your subscription"));
}

static int locked_mode(void) {
    int m = cfg_int("tv2_locked", 1);
    return m < 0 ? 0 : (m > 2 ? 2 : m);
}

static void ent_done(void *ud, cJSON *json, long status);
static void entitlement_start(Tv2 *v) {
    EntJob *job = calloc(1, sizeof *job);
    job->v = v;
    job->gen = v->gen;
    job->view = v->view;
    for (int si = 0; si < v->nsec && job->n < ENT_BATCH; si++) {
        Section *s = &v->sec[si];
        for (int ci = 0; ci < s->n && job->n < ENT_BATCH; ci++) {
            Card *c = &s->cards[ci];
            if (c->ent != 0 || (c->type != CARD_VOD && c->type != CARD_SERIES) ||
                !c->guid || !*c->guid || !c->path) continue;
            if (strncmp(c->path, "/film/", 6) && strncmp(c->path, "/serie/", 7) &&
                strncmp(c->path, "/program/", 9)) continue;
            snprintf(job->guid[job->n], sizeof job->guid[job->n], "%s", c->guid);
            snprintf(job->path[job->n], sizeof job->path[job->n], "%s", c->path);
            job->n++;
        }
    }
    if (!job->n) { free(job); return; }

    char *q = str_dup("query Entitlements(");
    for (int i = 0; i < job->n; i++) {
        char *n = str_printf("%s$r%d: DetailsPageReferenceInput!%s", q, i, i + 1 < job->n ? ", " : ") {");
        free(q); q = n;
    }
    for (int i = 0; i < job->n; i++) {
        char *n = str_printf("%s d%d: detailsPage(reference: $r%d) { __typename ...EntFields }", q, i, i);
        free(q); q = n;
    }
    {
        char *n = str_printf("%s } fragment EntFields on DetailsPageType { "
                             "... on DetailsPage { decks { __typename "
                             "... on DetailsPageTopDeck { deck { buttonsV2 { ctaButton { text buttonType action { __typename "
                             "... on UpsellOverlayAction { contentId } "
                             "... on DetailsPageInternalLinkAction { url } } } } } } } } "
                             "... on DetailsPageRedirect { redirectUrl } }", q);
        free(q); q = n;
    }

    cJSON *vars = cJSON_CreateObject();
    for (int i = 0; i < job->n; i++) {
        cJSON *ref = cJSON_CreateObject();
        cJSON_AddStringToObject(ref, "value", job->path[i]);
        cJSON_AddStringToObject(ref, "type", "WEB_REF");
        char key[8];
        snprintf(key, sizeof key, "r%d", i);
        cJSON_AddItemToObject(vars, key, ref);
    }
    char *body = graphql_body("Entitlements", q, vars);
    free(q);
    tv2_ent_api(PLAY_API, body, ent_done, job, v);
    free(body);
}

static void ent_done(void *ud, cJSON *json, long status) {
    EntJob *job = ud;
    Tv2 *v = job->v;
    if (v != live || v->gen != job->gen) { free(job); return; }

    for (int i = 0; i < job->n; i++)
        set_cards_ent(v, job->guid[i], 3, NULL); /* don't retry a card that gives no useful answer */
    if (status == 200 && json) {
        cJSON *data = cJSON_GetObjectItem(json, "data");
        for (int i = 0; i < job->n; i++) {
            char key[8];
            snprintf(key, sizeof key, "d%d", i);
            cJSON *dp = data ? cJSON_GetObjectItem(data, key) : NULL;
            if (!cJSON_IsObject(dp)) continue;
            const char *dtn = jval(dp, "__typename");
            if (!dtn || strcmp(dtn, "DetailsPage")) continue;
            cJSON *decks = cJSON_GetObjectItem(dp, "decks");
            cJSON *deck;
            bool allowed = false, denied = false;
            char why[240] = "";
            cJSON_ArrayForEach(deck, decks) {
                const char *tn = jval(deck, "__typename");
                if (!tn || strcmp(tn, "DetailsPageTopDeck")) continue;
                cJSON *d = cJSON_GetObjectItem(deck, "deck");
                cJSON *buttons = cJSON_GetObjectItem(d, "buttonsV2");
                cJSON *cta = cJSON_GetObjectItem(buttons, "ctaButton");
                cJSON *act = cJSON_GetObjectItem(cta, "action");
                const char *atn = jval(act, "__typename");
                const char *url = jval(act, "url");
                const char *txt = jval(cta, "text");
                const char *bt = jval(cta, "buttonType");
                if (txt && *txt) snprintf(why, sizeof why, "%s", txt);
                if (bt && !strcmp(bt, "PLAY_ENTRY")) allowed = true;
                if (atn && !strcmp(atn, "DetailsPageInternalLinkAction") &&
                    url && !strncmp(url, "/afspil/", 8)) allowed = true;
                if (atn && !strcmp(atn, "UpsellOverlayAction")) denied = true;
                if (atn && !strcmp(atn, "DetailsPageLoginAction")) denied = true;
                if (atn && !strcmp(atn, "DetailsPageInternalLinkAction") &&
                    (!url || strncmp(url, "/afspil/", 8))) denied = true;
                if (bt && !strcmp(bt, "COMMERCIAL")) denied = true;
                if (txt && (strstr(txt, "Prøv") || strstr(txt, "prøv") || strstr(txt, "Lej") ||
                            strstr(txt, "lej") || strstr(txt, "Køb") || strstr(txt, "køb") ||
                            strstr(txt, "abonnement") || strstr(txt, "Abonnement") ||
                            strstr(txt, "Showtime") || strstr(txt, "SkyShowtime")))
                    denied = true;
            }
            if (allowed && !denied) set_cards_ent(v, job->guid[i], 1, NULL);
            else if (denied) set_cards_ent(v, job->guid[i], 2, why);
        }
    }
    int mode = locked_mode();
    bool hide = (job->view == V_SEARCH) ? (mode >= 2) : (mode >= 1);
    remove_denied_cards(v, hide);
    free(job);
    entitlement_start(v);
}

/* ---------- token storage ---------- */
static void token_path(char *buf, size_t n) { snprintf(buf, n, "%s/apps/tv2/token", data_dir()); }
/* one resume file per TV 2 profile (progress-<id>), so people don't get each other's positions */
static void progress_path(char *buf, size_t n) {
    char id[64];
    snprintf(id, sizeof id, "%s", cfg_get("tv2_profile_id", ""));
    for (char *c = id; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) *c = '_';
    if (id[0]) snprintf(buf, n, "%s/apps/tv2/progress-%s", data_dir(), id);
    else snprintf(buf, n, "%s/apps/tv2/progress", data_dir());
}

static void ensure_app_dir(void) {
    char *base = str_printf("%s/apps", data_dir());
    mkdir(base, 0700);
    free(base);
    char *d = app_dir();
    mkdir(d, 0700);
    free(d);
}
static void read_token_file(Tv2 *v) {
    char path[600];
    token_path(path, sizeof path);
    char *s = file_read_all(path, 8192);
    v->token[0] = 0;
    if (s) {
        str_trim(s);
        snprintf(v->token, sizeof v->token, "%s", s);
        free(s);
    }
}
static void write_token_file(Tv2 *v) {
    if (!v->token[0]) return;
    ensure_app_dir();
    char path[600], tmp[620];
    token_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fwrite(v->token, 1, strlen(v->token), f);
    fflush(f);
    fclose(f);
    chmod(tmp, 0600);
    rename(tmp, path);
}
static void clear_token_file(void) {
    char path[600];
    token_path(path, sizeof path);
    unlink(path);
}
static bool token_valid(const Tv2 *v) {
    return v->token[0] && (v->token_exp <= 0 || v->token_exp > (double)time(NULL) + 60);
}
static void save_token_json(Tv2 *v, cJSON *j) {
    char *at = jstr(j, "access_token");
    if (!at) return;
    snprintf(v->token, sizeof v->token, "%s", at);
    free(at);
    char *rt = jstr(j, "refresh_token");
    if (rt) {
        snprintf(v->refresh, sizeof v->refresh, "%s", rt);
        cfg_set("tv2_refresh", v->refresh);
        free(rt);
    }
    cJSON *ex = cJSON_GetObjectItem(j, "expires_in");
    double secs = cJSON_IsNumber(ex) ? ex->valuedouble : 86400;
    v->token_exp = (double)time(NULL) + secs;
    char *e = str_printf("%.0f", v->token_exp);
    cfg_set("tv2_token_exp", e);
    free(e);
    write_token_file(v);
}

/* ---------- progress (local resume) ---------- */
static double progress_load(const char *guid) {
    if (!guid || !*guid) return 0;
    char path[600];
    progress_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[600], g[160];
    double pos = 0, dur = 0, out = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%159s %lf %lf", g, &pos, &dur) == 3 && !strcmp(g, guid) && pos > 5) out = pos;
    }
    fclose(f);
    return out;
}
static void progress_write(const char *guid, double pos, double dur, bool remove) {
    if (!guid || !*guid) return;
    ensure_app_dir();
    char path[600], tmp[620];
    progress_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    bool wrote = false;
    if (in) {
        char line[600], g[160];
        while (fgets(line, sizeof line, in)) {
            double p, d;
            if (sscanf(line, "%159s %lf %lf", g, &p, &d) == 3) {
                if (!strcmp(g, guid)) continue;
                fputs(line, out);
                wrote = true;
            }
        }
        fclose(in);
    }
    if (!remove && pos > 5 && dur > 0 && pos < dur - 15) {
        fprintf(out, "%s %.3f %.3f\n", guid, pos, dur);
        wrote = true;
    }
    fclose(out);
    if (wrote) rename(tmp, path); else { unlink(tmp); unlink(path); }
}

/* ---------- login ---------- */
static void device_done(void *ud, cJSON *json, long status);

static void start_login(Tv2 *v) {
    v->gen++;
    set_loading(v, T("Getting a sign-in code…"));
    char *scope = url_encode("openid profile email offline_access");
    char *aud = url_encode(TV2_AUDIENCE);
    char *body = str_printf("client_id=%s&scope=%s&audience=%s", TV2_CLIENT_ID, scope, aud);
    free(scope); free(aud);
    tv2_form(AUTH_DEVICE_URL, body, device_done, v);
    free(body);
}

static void login_error(Tv2 *v, cJSON *j, long status) {
    char *err = j ? jstr(j, "error") : NULL;
    char *desc = j ? jstr(j, "error_description") : NULL;
    set_error(v, T("Sign-in failed: %s"), desc ? desc : (err ? err : T("no connection")));
    free(err); free(desc);
    (void)status;
}

static void profiles_done(void *ud, cJSON *json, long status);
static void home_load(Tv2 *v);

static const char PROFILES_QUERY[] =
    "query Profiles { profiles { nodes { avatar { url } profileToken profileId restricted name } } }";
static void load_profiles(Tv2 *v) {
    char *body = graphql_body("Profiles", PROFILES_QUERY, cJSON_CreateObject());
    tv2_api(PLAY_API, body, profiles_done, v);
    free(body);
}

static void token_done(void *ud, cJSON *json, long status) {
    Tv2 *v = ud;
    v->polling = false;
    if (v->state != S_CODE) return;
    char *at = status == 200 ? jstr(json, "access_token") : NULL;
    if (at) {
        free(at);
        save_token_json(v, json);
        v->auth_retried = false;
        v->profile[0] = 0;
        v->profile_name[0] = 0;
        cfg_set("tv2_profile", "");
        cfg_set("tv2_profile_name", "");
        app_toast(T("Signed in to TV 2 Play"));
        v->state = S_LOADING;
        snprintf(v->msg, sizeof v->msg, T("Getting your profile…"));
        v->gen++;
        load_profiles(v);
        return;
    }
    char *err = json ? jstr(json, "error") : NULL;
    char *desc = json ? jstr(json, "error_description") : NULL;
    if (err && !strcmp(err, "authorization_pending")) { free(err); free(desc); return; }
    if (err && !strcmp(err, "slow_down")) {
        v->interval += 5;
        v->poll_next = app_time() + v->interval;
    } else if (err) {
        set_error(v, T("Sign-in failed: %s"), desc ? desc : err);
    } else if (status != 200) {
        set_error(v, T("Sign-in failed (%ld)"), status);
    }
    free(err); free(desc);
}

static void device_done(void *ud, cJSON *json, long status) {
    Tv2 *v = ud;
    char *dc = status == 200 ? jstr(json, "device_code") : NULL;
    char *uc = status == 200 ? jstr(json, "user_code") : NULL;
    char *uri = status == 200 ? jstr(json, "verification_uri") : NULL;
    char *cmp = status == 200 ? jstr(json, "verification_uri_complete") : NULL;
    if (dc && uc) {
        snprintf(v->device_code, sizeof v->device_code, "%s", dc);
        snprintf(v->user_code, sizeof v->user_code, "%s", uc);
        snprintf(v->verify_uri, sizeof v->verify_uri, "%s", uri ? uri : "https://auth.tv2.dk/activate");
        snprintf(v->verify_complete, sizeof v->verify_complete, "%s", cmp && *cmp ? cmp : v->verify_uri);
        cJSON *iv = cJSON_GetObjectItem(json, "interval");
        cJSON *ex = cJSON_GetObjectItem(json, "expires_in");
        v->interval = cJSON_IsNumber(iv) ? iv->valuedouble : 5;
        v->code_expires = app_time() + (cJSON_IsNumber(ex) ? ex->valuedouble : 900);
        v->poll_next = app_time() + v->interval;
        v->state = S_CODE;
    } else {
        login_error(v, json, status);
    }
    free(dc); free(uc); free(uri); free(cmp);
}

static void refresh_done(void *ud, cJSON *json, long status) {
    Tv2 *v = ud;
    char *at = status == 200 && json ? jstr(json, "access_token") : NULL;
    if (at) {
        free(at);
        save_token_json(v, json);
        v->state = S_LOADING;
        snprintf(v->msg, sizeof v->msg, T("Getting your profile…"));
        v->gen++;
        load_profiles(v);
        return;
    }
    v->state = S_LOGIN;
    snprintf(v->msg, sizeof v->msg, T("Your sign-in has expired. Sign in again."));
    clear_token_file();
    v->token[0] = 0;
    cfg_set("tv2_refresh", "");
    v->refresh[0] = 0;
}

static bool refresh_sync(Tv2 *v) {
    if (!v->refresh[0]) return false;
    char *rt = url_encode(v->refresh);
    char *aud = url_encode(TV2_AUDIENCE);
    char *body = str_printf("grant_type=refresh_token&client_id=%s&refresh_token=%s&audience=%s", TV2_CLIENT_ID, rt, aud);
    free(rt); free(aud);
    const char *hdr[] = {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded", NULL};
    HttpBuf b;
    memset(&b, 0, sizeof b);
    bool ok = false;
    if (http_request("POST", AUTH_TOKEN_URL, hdr, body, &b, 20) && b.status == 200 && b.data) {
        cJSON *j = cJSON_Parse(b.data);
        char *at = j ? jstr(j, "access_token") : NULL;
        if (at) { free(at); save_token_json(v, j); ok = true; }
        cJSON_Delete(j);
    }
    if (!ok && (b.status == 400 || b.status == 401)) {
        cfg_set("tv2_refresh", "");
        v->refresh[0] = 0;
    }
    http_free(&b);
    free(body);
    return ok;
}

static void refresh_async(Tv2 *v) {
    if (!v->refresh[0]) { v->state = S_LOGIN; return; }
    v->gen++;
    set_loading(v, T("Renewing sign-in…"));
    char *rt = url_encode(v->refresh);
    char *aud = url_encode(TV2_AUDIENCE);
    char *body = str_printf("grant_type=refresh_token&client_id=%s&refresh_token=%s&audience=%s", TV2_CLIENT_ID, rt, aud);
    free(rt); free(aud);
    tv2_form(AUTH_TOKEN_URL, body, refresh_done, v);
    free(body);
}

static void auth_failed(Tv2 *v) {
    clear_token_file();
    v->token[0] = 0;
    v->token_exp = 0;
    cfg_set("tv2_token_exp", "0");
    if (v->refresh[0] && !v->auth_retried) {
        v->auth_retried = true;
        refresh_async(v);
    } else {
        v->state = S_LOGIN;
        v->auth_retried = false;
        snprintf(v->msg, sizeof v->msg, T("Your sign-in has expired. Sign in again."));
    }
}

/* ---------- page/search/details parsing ---------- */
static const char PAGE_QUERY[] =
    "query Page($path: String!) { page(path: $path) { path title content { __typename ... on Hero { id cards { __typename ... on HeroCard { id "
    "title subTitle imageUrl logoUrl titleArtUrl accessibilityTitle labels { text color } destination { externalId path type } action { "
    "__typename clientAction { __typename ... on DestinationClientAction { destination { externalId path type } } ... on "
    "OpenEntitlementClientAction { __typename } } } ctaButton { title dsType accessibilityTitle action { __typename clientAction { __typename ... "
    "on DestinationClientAction { destination { externalId path type } } ... on OpenEntitlementClientAction { __typename } } } } } } } ... on "
    "RowDeck { title cards { __typename ... on VodCard { id title subTitle imageUrl logoUrl accessibilityTitle labels { text color } destination "
    "{ externalId path type } progress { percentage } action { __typename clientAction { __typename ... on DestinationClientAction { destination "
    "{ externalId path type } } ... on OpenEntitlementClientAction { __typename } } } } ... on ContentCard { id title subTitle imageUrl logoUrl "
    "accessibilityTitle labels { text color } destination { externalId path type } action { __typename clientAction { __typename ... on "
    "DestinationClientAction { destination { externalId path type } } ... on OpenEntitlementClientAction { __typename } } } } ... on "
    "CollectionCard { id title accessibilityTitle image { url } destination { externalId path type } } ... on LiveCard { id title subTitle "
    "imageUrl logoUrl accessibilityTitle labels { text color } destination { externalId path type } action { __typename clientAction { __typename "
    "... on DestinationClientAction { destination { externalId path type } } ... on OpenEntitlementClientAction { __typename } } } } } } ... on "
    "StackedDeck { title cards { __typename ... on VodCard { id title subTitle imageUrl logoUrl accessibilityTitle labels { text color } "
    "destination { externalId path type } progress { percentage } action { __typename clientAction { __typename ... on DestinationClientAction { "
    "destination { externalId path type } } ... on OpenEntitlementClientAction { __typename } } } } ... on ContentCard { id title subTitle "
    "imageUrl logoUrl accessibilityTitle labels { text color } destination { externalId path type } action { __typename clientAction { __typename "
    "... on DestinationClientAction { destination { externalId path type } } ... on OpenEntitlementClientAction { __typename } } } } ... on "
    "CollectionCard { id title accessibilityTitle image { url } destination { externalId path type } } ... on LiveCard { id title subTitle "
    "imageUrl logoUrl accessibilityTitle labels { text color } destination { externalId path type } action { __typename clientAction { __typename "
    "... on DestinationClientAction { destination { externalId path type } } ... on OpenEntitlementClientAction { __typename } } } } } } } } }"
    ;
static const char SEARCH_QUERY[] =
    "query SearchResultsPage($query: String!, $filters: [SearchPageFilterInput!], $offset: Int!) { "
    "searchResultsPage(query: $query, filters: $filters, offset: $offset) { __typename title containsResults decks { __typename "
    "... on SearchPageSearchResult { totalCount offset cardList { __typename "
    "... on SearchPageDefaultCard { id title image { url } variant size action { __typename "
    "... on SearchPageInternalLinkAction { contentId url } "
    "... on SearchPagePlaybackAction { contentId url } } } } } } } }";

static const char DETAILS_QUERY[] =
    "query DetailsPage($reference: DetailsPageReferenceInput!) { detailsPage(reference: $reference) { __typename ... on DetailsPage { id decks { "
    "__typename ... on DetailsPageTopDeck { deck { title { text } description buttonsV2 { ctaButton { text buttonType action { __typename ... on "
    "DetailsPageInternalLinkAction { url } ... on UpsellOverlayAction { contentId } } } secondaryButtons { text action { __typename ... on "
    "DetailsPageInternalLinkAction { url } } } } } } ... on DetailsPageEpisodeListDeck { deck { __typename ... on DetailsPageAnchoredCardList { "
    "anchors { title url index } cardList { id offset totalCount items { __typename ... on DetailsPageCard { id title info image { url } action { "
    "__typename ... on DetailsPageInternalLinkAction { url pageType } } } } } } ... on DetailsPageCardList { id offset totalCount items { "
    "__typename ... on DetailsPageCard { id title info image { url } action { __typename ... on DetailsPageInternalLinkAction { url pageType } } "
    "} } } } } ... on DetailsPageRelatedDeck { cardList { id offset totalCount items { __typename ... on DetailsPageCard { id title info image { "
    "url } action { __typename ... on DetailsPageInternalLinkAction { url pageType } } } } } } } } ... on DetailsPageRedirect { redirectUrl "
    "permanent } } }"
    ;
static const char EPISODES_QUERY[] =
    "query DetailPageCardList($detailsPageCardListId: ID!, $limit: Int!, $offset: Int!) { "
    "detailsPageCardListItems(id: $detailsPageCardListId, limit: $limit, offset: $offset) { __typename "
    "... on DetailsPageCard { id title info image { url } action { __typename "
    "... on DetailsPageInternalLinkAction { url pageType } } } } }";

static const char *card_label(cJSON *card, const char **color) {
    *color = NULL;
    cJSON *labels = cJSON_GetObjectItem(card, "labels");
    cJSON *l = cJSON_IsArray(labels) ? cJSON_GetArrayItem(labels, 0) : NULL;
    if (!cJSON_IsObject(l)) return NULL;
    *color = jval(l, "color");
    return jval(l, "text");
}
static void parse_hero(Tv2 *v, cJSON *hero) {
    cJSON *cards = cJSON_GetObjectItem(hero, "cards");
    if (!cJSON_IsArray(cards)) return;
    cJSON *card;
    cJSON_ArrayForEach(card, cards) {
        if (v->nhero >= MAX_HERO) break;
        cJSON *dst = cJSON_GetObjectItem(card, "destination");
        cJSON *act = cJSON_GetObjectItem(card, "action");
        cJSON *cli = cJSON_GetObjectItem(act, "clientAction");
        cJSON *cdst = cJSON_GetObjectItem(cli, "destination");
        cJSON *cta = cJSON_GetObjectItem(card, "ctaButton");
        cJSON *cact = cJSON_GetObjectItem(cta, "action");
        cJSON *ccli = cJSON_GetObjectItem(cact, "clientAction");
        cJSON *ccdst = cJSON_GetObjectItem(ccli, "destination");
        const char *path = jval(cdst, "path");
        if (!path) path = jval(dst, "path");
        const char *cta_path = jval(ccdst, "path");
        if (!cta_path) cta_path = path;
        const char *img = jval(card, "imageUrl");
        const char *logo = jval(card, "logoUrl");
        const char *art = jval(card, "titleArtUrl");
        const char *title = jval(card, "title");
        const char *sub = jval(card, "subTitle");
        const char *acc = jval(card, "accessibilityTitle");
        const char *cta_txt = jval(cta, "title");
        const char *cta_acc = jval(cta, "accessibilityTitle");
        const char *lcolor = NULL;
        const char *label = card_label(card, &lcolor);
        if (!title && acc) title = acc;
        if (!cta_txt) cta_txt = (cta_path && !strncmp(cta_path, "/afspil/", 8)) ? T("Play") : T("More info");
        Hero *h = &v->hero[v->nhero++];
        memset(h, 0, sizeof *h);
        h->title = str_dup(title);
        h->sub = str_dup(sub);
        h->img = img_url_fix(img);
        h->logo = img_url_fix(logo);
        h->titleart = img_url_fix(art);
        h->path = str_dup(path);
        h->cta = str_dup(cta_txt);
        h->cta_path = str_dup(cta_path);
        h->label = str_dup(label);
        h->label_color = str_dup(lcolor);
        h->acc = str_dup(acc ? acc : cta_acc);
        const char *ctn = jval(ccli, "__typename");
        h->cta_ent = ctn && !strcmp(ctn, "OpenEntitlementClientAction");
        char *guid = guid_from_play_url(path);
        if (!guid) guid = str_dup(jval(dst, "externalId"));
        h->guid = guid;
    }
    if (v->nhero > 0 && v->hero[0].cta_ent) {
        for (int i = 1; i < v->nhero; i++) {
            if (!v->hero[i].cta_ent) { v->hero_i = i; break; }
        }
    }
}

static void parse_page_cards(Tv2 *v, Section *s, cJSON *cards) {
    cJSON *card;
    cJSON_ArrayForEach(card, cards) {
        cJSON *dst = cJSON_GetObjectItem(card, "destination");
        cJSON *img = cJSON_GetObjectItem(card, "image");
        const char *guid = jval(dst, "externalId");
        if (!guid) guid = jval(card, "id");
        const char *path = jval(dst, "path");
        const char *type = jval(dst, "type");
        const char *title = jval(card, "title");
        const char *sub = jval(card, "subTitle");
        const char *url = jval(card, "imageUrl");
        if (!url && img) url = jval(img, "url");
        double prog = 0;
        cJSON *pr = cJSON_GetObjectItem(card, "progress");
        if (pr) prog = jnum(pr, "percentage");
        if (path && path[0] == '#') continue;
        /* provider/category logos are SVG; without a title they would be an empty tile */
        if ((!title || !*title) && url && strstr(url, ".svg")) continue;
        int ct = card_type(path, type);
        bool live_view = (v->view == V_LIVE);
        if (live_view != (ct == CARD_LIVE)) continue;
        add_card(s, guid, path, title, sub, url, ct, prog);
        Card *c = s->n ? &s->cards[s->n - 1] : NULL;
        if (!c) continue;
        const char *lcolor = NULL;
        const char *label = card_label(card, &lcolor);
        const char *acc = jval(card, "accessibilityTitle");
        card_meta(c, label, lcolor, acc);
        if (ct == CARD_LIVE) {
            cJSON *act = cJSON_GetObjectItem(card, "action");
            cJSON *cli = cJSON_GetObjectItem(act, "clientAction");
            const char *ctn = jval(cli, "__typename");
            if (ctn && !strcmp(ctn, "OpenEntitlementClientAction")) {
                c->ent = 2;
                c->lock = str_dup(T("Needs access to the TV 2 channel"));
            } else c->ent = 1;
        }
    }
}

static void parse_page(Tv2 *v, cJSON *page) {
    const char *title = jval(page, "title");
    if (title && *title) snprintf(v->heading, sizeof v->heading, "%s", title);
    cJSON *content = cJSON_GetObjectItem(page, "content");
    cJSON *deck;
    cJSON_ArrayForEach(deck, content) {
        const char *tn = jval(deck, "__typename");
        if (tn && !strcmp(tn, "Hero") && v->view == V_HOME) {
            parse_hero(v, deck);
            continue;
        }
        if (!tn || (strcmp(tn, "RowDeck") && strcmp(tn, "StackedDeck"))) continue;
        cJSON *cards = cJSON_GetObjectItem(deck, "cards");
        if (!cJSON_IsArray(cards) || cJSON_GetArraySize(cards) == 0) continue;
        Section *s = add_section(v, jval(deck, "title"));
        if (!s) continue;
        if (section_is_poster(jval(deck, "title"), cards)) {
            s->fmt = FMT_POSTER;
            s->size = SZ_MEDIUM;
        }
        const char *st = jval(deck, "title");
        if (st && (strstr(st, "Afsnit") || strstr(st, "Episoder"))) s->size = SZ_SMALL;
        if (st && !strcmp(st, "Se videre")) s->size = SZ_LARGE;
        parse_page_cards(v, s, cards);
    }
}

static void page_done(void *ud, cJSON *json, long status) {
    Tv2 *v = ud;
    if (status == 401) { auth_failed(v); return; }
    if (status != 200 || !json) { set_error(v, T("TV 2 answered with error %ld"), status); return; }
    cJSON *data = cJSON_GetObjectItem(json, "data");
    cJSON *page = data ? cJSON_GetObjectItem(data, "page") : NULL;
    if (!cJSON_IsObject(page)) { set_error(v, T("Could not load the TV 2 page")); return; }
    clear_sections(v);
    parse_page(v, page);
    compact_sections(v);
    v->auth_retried = false;
    v->state = S_READY;
    v->msg[0] = 0;
    if (v->nsec == 0) snprintf(v->msg, sizeof v->msg, T("Nothing on this page"));
    entitlement_start(v);
}

static void home_load(Tv2 *v) {
    v->gen++;
    v->view = V_HOME;
    v->path[0] = 0;
    v->state = S_LOADING;
    v->search_home = false;
    v->detail_focus = false;
    snprintf(v->msg, sizeof v->msg, T("Loading the front page…"));
    clear_sections(v);
    clear_hero(v);
    v->hero_focus = true;
    cJSON *vars = cJSON_CreateObject();
    cJSON_AddStringToObject(vars, "path", "/");
    char *body = graphql_body("Page", PAGE_QUERY, vars);
    tv2_api(PAGES_API, body, page_done, v);
    free(body);
}

static void page_load(Tv2 *v, const char *path, const char *title, int view) {
    v->view = view;
    v->gen++;
    if (path != v->path) snprintf(v->path, sizeof v->path, "%s", path ? path : "/");
    v->state = S_LOADING;
    v->search_home = false;
    v->hero_focus = false;
    v->detail_focus = false;
    snprintf(v->heading, sizeof v->heading, "%s", title && *title ? title : T("TV 2 Play"));
    snprintf(v->msg, sizeof v->msg, T("Loading…"));
    clear_sections(v);
    clear_hero(v);
    cJSON *vars = cJSON_CreateObject();
    cJSON_AddStringToObject(vars, "path", v->path[0] ? v->path : "/");
    char *body = graphql_body("Page", PAGE_QUERY, vars);
    tv2_api(PAGES_API, body, page_done, v);
    free(body);
}
static void page_by_path(Tv2 *v, const char *path, const char *title) {
    page_load(v, path, title, V_BROWSE);
}
static void categories_load(Tv2 *v) {
    page_load(v, "/kategorier", T("Categories"), V_CATEGORIES);
}
static void live_load(Tv2 *v) {
    page_load(v, "/live", T("Live TV"), V_LIVE);
}
static void profile_open(Tv2 *v) {
    v->view = V_PROFILE;
    v->state = S_READY;
    v->nsec = 0;
    v->msg[0] = 0;
    v->profile_sel = 0;
    v->hero_focus = false;
    v->detail_focus = false;
    v->heading[0] = 0;
}

static void search_http_done(void *ud, cJSON *json, long status) {
    Tv2 *v = ud;
    if (status == 401) { auth_failed(v); return; }
    if (status != 200 || !json) { set_error(v, T("The search failed (%ld)"), status); return; }
    cJSON *data = cJSON_GetObjectItem(json, "data");
    cJSON *sr = data ? cJSON_GetObjectItem(data, "searchResultsPage") : NULL;
    if (!cJSON_IsObject(sr)) { set_error(v, T("Could not search")); return; }
    clear_sections(v);
    cJSON *decks = cJSON_GetObjectItem(sr, "decks");
    cJSON *deck;
    cJSON_ArrayForEach(deck, decks) {
        const char *tn = jval(deck, "__typename");
        if (!tn || strcmp(tn, "SearchPageSearchResult")) continue;
        cJSON *list = cJSON_GetObjectItem(deck, "cardList");
        if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) continue;
        Section *s = add_section(v, v->query[0] ? v->query : T("Results"));
        cJSON *card;
        cJSON_ArrayForEach(card, list) {
            cJSON *act = cJSON_GetObjectItem(card, "action");
            cJSON *img = cJSON_GetObjectItem(card, "image");
            const char *atn = jval(act, "__typename");
            if (!atn || (strcmp(atn, "SearchPageInternalLinkAction") && strcmp(atn, "SearchPagePlaybackAction"))) continue;
            const char *guid = jval(act, "contentId");
            const char *path = jval(act, "url");
            const char *title = jval(card, "title");
            const char *url = img ? jval(img, "url") : NULL;
            int ct = card_type(path, NULL);
            if (ct == CARD_LIVE) continue;
            add_card(s, guid, path, title, NULL, url, ct, 0);
        }
    }
    if (v->search_filter != SF_ALL) {
        for (int si = 0; si < v->nsec; si++) {
            Section *s = &v->sec[si];
            int w = 0;
            for (int i = 0; i < s->n; i++) {
                Card *c = &s->cards[i];
                bool keep = true;
                if (v->search_filter == SF_FILM)
                    keep = c->path && (!strncmp(c->path, "/film/", 6) || !strncmp(c->path, "/program/", 9));
                else if (v->search_filter == SF_SERIES)
                    keep = c->path && !strncmp(c->path, "/serie/", 7);
                if (!keep) { card_free(c); continue; }
                if (w != i) s->cards[w] = s->cards[i];
                w++;
            }
            s->n = w;
        }
        compact_sections(v);
    } else compact_sections(v);
    v->auth_retried = false;
    v->state = S_READY;
    v->msg[0] = 0;
    if (v->nsec == 0) snprintf(v->msg, sizeof v->msg, T("No results"));
    entitlement_start(v);
}

static void search_submit(Tv2 *v, const char *q) {
    v->view = V_SEARCH;
    v->search_home = false;
    v->gen++;
    v->path[0] = 0;
    v->state = S_LOADING;
    snprintf(v->heading, sizeof v->heading, T("Search: %s"), q);
    snprintf(v->msg, sizeof v->msg, T("Searching…"));
    clear_sections(v);
    cJSON *vars = cJSON_CreateObject();
    cJSON_AddStringToObject(vars, "query", q);
    cJSON_AddItemToObject(vars, "filters", cJSON_CreateArray());
    cJSON_AddNumberToObject(vars, "offset", 0);
    char *body = graphql_body("SearchResultsPage", SEARCH_QUERY, vars);
    tv2_api(PLAY_API, body, search_http_done, v);
    free(body);
}

static void search_done(void *ud, const char *q) {
    Tv2 *v = ud;
    if (!q || v != live) return;
    char *t = str_dup(q);
    str_trim(t);
    if (*t) {
        snprintf(v->query, sizeof v->query, "%s", t);
        v->search_home = false;
        history_add(v, t);
        search_submit(v, t);
    }
    free(t);
}

static void episodes_done(void *ud, cJSON *json, long status) {
    Tv2 *v = ud;
    if (status == 401) { auth_failed(v); return; }
    if (status != 200 || !json) { set_error(v, T("Could not load episodes (%ld)"), status); return; }
    cJSON *data = cJSON_GetObjectItem(json, "data");
    cJSON *items = data ? cJSON_GetObjectItem(data, "detailsPageCardListItems") : NULL;
    if (!cJSON_IsArray(items)) { set_error(v, T("Could not load episodes")); return; }
    clear_sections(v);
    Section *s = NULL;
    int n = cJSON_GetArraySize(items);
    for (int i = 0; i < n; i++) {
        if (i % CARD_COLS == 0) {
            s = add_section(v, T("Episodes"));
        }
        cJSON *item = cJSON_GetArrayItem(items, i);
        cJSON *act = cJSON_GetObjectItem(item, "action");
        const char *atn = jval(act, "__typename");
        if (!atn || strcmp(atn, "DetailsPageInternalLinkAction")) continue;
        const char *url = jval(act, "url");
        const char *page_type = jval(act, "pageType");
        if (page_type && strcmp(page_type, "PLAYBACK")) continue;
        char *guid = guid_from_play_url(url);
        cJSON *img = cJSON_GetObjectItem(item, "image");
        const char *title = jval(item, "title");
        const char *info = jval(item, "info");
        const char *image = img ? jval(img, "url") : NULL;
        if (guid && title) add_card(s, guid, url, title, info, image, CARD_VOD, 0);
        free(guid);
    }
    compact_sections(v);
    v->auth_retried = false;
    v->state = S_READY;
    v->msg[0] = 0;
    if (v->nsec == 0) snprintf(v->msg, sizeof v->msg, T("No episodes found"));
}

static void load_episodes(Tv2 *v, const char *id) {
    v->gen++;
    v->state = S_LOADING;
    snprintf(v->msg, sizeof v->msg, T("Loading episodes…"));
    cJSON *vars = cJSON_CreateObject();
    cJSON_AddStringToObject(vars, "detailsPageCardListId", id);
    cJSON_AddNumberToObject(vars, "limit", 100);
    cJSON_AddNumberToObject(vars, "offset", 0);
    char *body = graphql_body("DetailPageCardList", EPISODES_QUERY, vars);
    tv2_api(PLAY_API, body, episodes_done, v);
    free(body);
}

static void details_done(void *ud, cJSON *json, long status) {
    Tv2 *v = ud;
    if (status == 401) { auth_failed(v); return; }
    if (status != 200 || !json) { set_error(v, T("Could not load the series (%ld)"), status); return; }
    cJSON *data = cJSON_GetObjectItem(json, "data");
    cJSON *dp = data ? cJSON_GetObjectItem(data, "detailsPage") : NULL;
    if (!cJSON_IsObject(dp)) { set_error(v, T("Could not load the series")); return; }
    const char *tn = jval(dp, "__typename");
    if (tn && !strcmp(tn, "DetailsPageRedirect")) {
        const char *url = jval(dp, "redirectUrl");
        if (url) { page_by_path(v, url, v->heading); return; }
    }
    cJSON *decks = cJSON_GetObjectItem(dp, "decks");
    cJSON *deck;
    const char *episode_id = NULL;
    cJSON *episode_items = NULL;
    cJSON *related_items = NULL;
    cJSON_ArrayForEach(deck, decks) {
        const char *dtn = jval(deck, "__typename");
        if (dtn && !strcmp(dtn, "DetailsPageTopDeck")) {
            cJSON *d = cJSON_GetObjectItem(deck, "deck");
            cJSON *t = cJSON_GetObjectItem(d, "title");
            const char *txt = t ? jval(t, "text") : NULL;
            if (txt && *txt) snprintf(v->heading, sizeof v->heading, "%s", txt);
            const char *desc = jval(d, "description");
            if (desc && *desc) snprintf(v->detail_desc, sizeof v->detail_desc, "%s", desc);
            cJSON *bv = cJSON_GetObjectItem(d, "buttonsV2");
            cJSON *cta = cJSON_GetObjectItem(bv, "ctaButton");
            const char *ct = jval(cta, "text");
            const char *bt = jval(cta, "buttonType");
            if (ct && *ct) snprintf(v->detail_cta, sizeof v->detail_cta, "%s", ct);
            cJSON *act = cJSON_GetObjectItem(cta, "action");
            const char *atn = jval(act, "__typename");
            const char *url = jval(act, "url");
            if (url && *url) snprintf(v->detail_cta_path, sizeof v->detail_cta_path, "%s", url);
            v->detail_cta_ent = 0;
            if (atn && (!strcmp(atn, "UpsellOverlayAction") || !strcmp(atn, "DetailsPageLoginAction")))
                v->detail_cta_ent = 1;
            if (bt && !strcmp(bt, "COMMERCIAL")) v->detail_cta_ent = 1;
            if (atn && !strcmp(atn, "DetailsPageInternalLinkAction") && (!url || strncmp(url, "/afspil/", 8)))
                v->detail_cta_ent = 1;
            if (!v->detail_cta[0])
                snprintf(v->detail_cta, sizeof v->detail_cta, "%s", T("Play"));
        }
        if (dtn && !strcmp(dtn, "DetailsPageRelatedDeck")) {
            cJSON *cl = cJSON_GetObjectItem(deck, "cardList");
            cJSON *items = cJSON_GetObjectItem(cl, "items");
            if (cJSON_IsArray(items)) related_items = items;
        }
        if (dtn && !strcmp(dtn, "DetailsPageEpisodeListDeck")) {
            cJSON *d = cJSON_GetObjectItem(deck, "deck");
            cJSON *cl = cJSON_GetObjectItem(d, "cardList");
            if (cl) {
                const char *id = jval(cl, "id");
                if (id) episode_id = id;
                cJSON *items = cJSON_GetObjectItem(cl, "items");
                if (cJSON_IsArray(items)) episode_items = items;
            }
        }
    }
    if (episode_id) { load_episodes(v, episode_id); return; }
    clear_sections(v);
    if (episode_items) {
        Section *s = add_section(v, T("Episodes"));
        if (s) s->size = SZ_SMALL;
        cJSON *item;
        cJSON_ArrayForEach(item, episode_items) {
            cJSON *act = cJSON_GetObjectItem(item, "action");
            const char *atn = jval(act, "__typename");
            if (!atn || strcmp(atn, "DetailsPageInternalLinkAction")) continue;
            const char *url = jval(act, "url");
            const char *pt = jval(act, "pageType");
            if (pt && strcmp(pt, "PLAYBACK")) continue;
            char *guid = guid_from_play_url(url);
            cJSON *img = cJSON_GetObjectItem(item, "image");
            const char *title = jval(item, "title");
            const char *info = jval(item, "info");
            const char *image = img ? jval(img, "url") : NULL;
            if (guid && title) add_card(s, guid, url, title, info, image, CARD_VOD, 0);
            free(guid);
        }
    }
    if (related_items) {
        Section *rs = add_section(v, T("More like this"));
        if (rs) { rs->fmt = FMT_POSTER; rs->size = SZ_MEDIUM; }
        cJSON *item;
        cJSON_ArrayForEach(item, related_items) {
            cJSON *act = cJSON_GetObjectItem(item, "action");
            const char *atn = jval(act, "__typename");
            if (!atn || strcmp(atn, "DetailsPageInternalLinkAction")) continue;
            const char *url = jval(act, "url");
            const char *pt = jval(act, "pageType");
            if (pt && strcmp(pt, "DETAILS")) continue;
            cJSON *img = cJSON_GetObjectItem(item, "image");
            const char *title = jval(item, "title");
            const char *info = jval(item, "info");
            const char *image = img ? jval(img, "url") : NULL;
            if (title && url) add_card(rs, NULL, url, title, info, image, CARD_VOD, 0);
        }
    }
    compact_sections(v);
    v->auth_retried = false;
    v->state = S_READY;
    v->msg[0] = 0;
    if (v->nsec == 0) snprintf(v->msg, sizeof v->msg, T("Nothing to play"));
}

static void details_load(Tv2 *v, const char *path, const char *title, const char *img) {
    v->view = V_DETAIL;
    v->gen++;
    v->search_home = false;
    v->hero_focus = false;
    v->detail_focus = true;
    v->detail_desc[0] = 0;
    v->detail_cta[0] = 0;
    v->detail_cta_path[0] = 0;
    v->detail_cta_ent = 0;
    if (img && *img && img != v->detail_img) snprintf(v->detail_img, sizeof v->detail_img, "%s", img);
    if (path != v->path) snprintf(v->path, sizeof v->path, "%s", path ? path : "");
    v->state = S_LOADING;
    snprintf(v->heading, sizeof v->heading, "%s", title && *title ? title : T("TV 2 Play"));
    snprintf(v->msg, sizeof v->msg, T("Loading…"));
    clear_sections(v);
    cJSON *vars = cJSON_CreateObject();
    cJSON *ref = cJSON_CreateObject();
    cJSON_AddStringToObject(ref, "value", v->path);
    cJSON_AddStringToObject(ref, "type", "WEB_REF");
    cJSON_AddItemToObject(vars, "reference", ref);
    char *body = graphql_body("DetailsPage", DETAILS_QUERY, vars);
    tv2_api(PLAY_API, body, details_done, v);
    free(body);
}

/* the side menu shows who is watching: the name on the "Min side" row and under "TV 2 Play" */
static void side_profile(Tv2 *v) {
    for (int i = 0; i < v->side.n && i < 8; i++)
        if (v->side_map[i] == M_MYPAGE)
            snprintf(v->side.items[i].label, sizeof v->side.items[i].label, "%s", v->profile_name[0] ? v->profile_name : T("My page"));
    snprintf(v->side.subtitle, sizeof v->side.subtitle, "%s", v->profile_name);
}

static void profile_use(Tv2 *v, int i) {
    if (i < 0 || i >= v->nprof) return;
    Tv2Prof *p = &v->prof[i];
    snprintf(v->profile, sizeof v->profile, "%s", p->token);
    snprintf(v->profile_name, sizeof v->profile_name, "%s", p->name);
    cfg_set("tv2_profile", v->profile);
    cfg_set("tv2_profile_name", v->profile_name);
    cfg_set("tv2_profile_id", p->id);
    snprintf(v->profile_avatar, sizeof v->profile_avatar, "%s", p->avatar);
    cfg_set("tv2_profile_avatar", p->avatar);
    if (p->avatar[0]) fprintf(stderr, "tv2: profile avatar %s\n", p->avatar); /* which image format TV 2 uses */
    side_profile(v);
}

static void who_open(Tv2 *v) {
    v->view = V_WHO;
    v->state = S_READY;
    v->nsec = 0;
    v->nhero = 0;
    v->msg[0] = 0;
    v->who_sel = 0;
    const char *cur = cfg_get("tv2_profile_id", "");
    for (int i = 0; i < v->nprof; i++) if (*cur && !strcmp(v->prof[i].id, cur)) v->who_sel = i;
}

/* After login, refresh and "Skift profil": keep the chosen profile (fresh token), ask "Hvem ser med?" when the
 * account has several and none is chosen yet. */
static void profiles_done(void *ud, cJSON *json, long status) {
    Tv2 *v = ud;
    if (status == 401) { auth_failed(v); return; }
    if (status == 200 && json) {
        cJSON *data = cJSON_GetObjectItem(json, "data");
        cJSON *profiles = data ? cJSON_GetObjectItem(data, "profiles") : NULL;
        cJSON *nodes = profiles ? cJSON_GetObjectItem(profiles, "nodes") : NULL;
        cJSON *node;
        v->nprof = 0;
        if (!cJSON_IsArray(nodes)) nodes = NULL;
        cJSON_ArrayForEach(node, nodes) {
            if (v->nprof >= MAX_PROF) break;
            char *pt = jstr(node, "profileToken");
            if (!pt || !*pt) { free(pt); continue; }
            Tv2Prof *p = &v->prof[v->nprof++];
            memset(p, 0, sizeof *p);
            char *name = jstr(node, "name"), *id = jstr(node, "profileId");
            snprintf(p->token, sizeof p->token, "%s", pt);
            snprintf(p->name, sizeof p->name, "%s", name ? name : "");
            snprintf(p->id, sizeof p->id, "%s", id ? id : "");
            p->kids = cJSON_IsTrue(cJSON_GetObjectItem(node, "restricted"));
            char *av = jstr(cJSON_GetObjectItem(node, "avatar"), "url");
            if (av && *av) {
                char *fx = img_url_fix(av);
                snprintf(p->avatar, sizeof p->avatar, "%s", fx ? fx : av);
                free(fx);
            }
            free(av); free(pt); free(name); free(id);
        }
    }
    const char *cur = cfg_get("tv2_profile_id", "");
    int keep = -1;
    for (int i = 0; i < v->nprof; i++) if (*cur && !strcmp(v->prof[i].id, cur)) keep = i;
    bool pick = v->force_pick, bg = v->bg_profiles;
    v->force_pick = v->bg_profiles = false;
    if (bg && !pick) { if (keep >= 0) profile_use(v, keep); return; } /* the page is already loading */
    if (v->nprof > 1 && (pick || keep < 0)) { who_open(v); return; }
    if (v->nprof) profile_use(v, keep >= 0 ? keep : 0);
    home_load(v);
}

/* ---------- playback ---------- */
static bool token_ensure(Tv2 *v) {
    if (token_valid(v)) return true;
    if (refresh_sync(v)) return true;
    return false;
}

/* wvserve writes the reason to serve.log. Show something useful instead of the
 * old generic "could not start" for titles that the subscription does not cover. */
static void play_error_toast(const char *log) {
    char *s = file_read_all(log, 20000);
    if (!s) { app_toast(T("Could not start TV 2 playback")); return; }
    if (strstr(s, "LicenseNotGranted") || strstr(s, "proper product"))
        app_toast(T("This title must be bought or rented"));
    else if (strstr(s, "ConcurrencyLimitViolation") || strstr(s, "too many active streams"))
        app_toast(T("TV 2 Play: too many active streams"));
    else if (strstr(s, "ingen WIDEVINE-stream") || strstr(s, "Missing media"))
        app_toast(T("This title cannot be played with Widevine"));
    else if (strstr(s, "ingen noegle"))
        app_toast(T("Could not get the DRM key"));
    else if (strstr(s, "resolve fejlede"))
        app_toast(T("Could not load the TV 2 stream"));
    else
        app_toast(T("Could not start TV 2 playback"));
    free(s);
}

/* Google's Widevine module (closed, so not in the package or the repo) and the NSPR libraries it needs are
 * downloaded on the box the first time something is played: the official .debs from Raspberry Pi's archive and
 * Debian, pinned by SHA256 (the same files as tested), unpacked with busybox ar/xz/tar into <data>/apps/tv2/lib.
 * Runs in the background; the UI waits with a frame loop (Back cancels the wait, not the download). */
#define CDM_DEB "https://archive.raspberrypi.com/debian/pool/main/w/widevine/libwidevinecdm0_4.10.2662.3+1_arm64.deb"
#define CDM_SHA "0e87823a228246f4f1c7c90ac48c5f888d0607d028ac0ffd17fe610d548f5bb3"
#define NSPR_DEB "https://deb.debian.org/debian/pool/main/n/nspr/libnspr4_4.36-1_arm64.deb"
#define NSPR_DEB2 "https://snapshot.debian.org/archive/debian/20251101T000000Z/pool/main/n/nspr/libnspr4_4.36-1_arm64.deb"
#define NSPR_SHA "b8cd0221e4c35a77ec6e3b89959ca96deb75919a3c55b6735ca0288a1b61bdbb"

extern const NotaApp nota_app; /* at the bottom */

static bool cdm_fetch(const char *dir) {
    char *done = str_printf("%s/wvdl.done", dir), *fail = str_printf("%s/wvdl.fail", dir);
    unlink(done); unlink(fail);
    run_cmd("( d='%s'; t=\"$d/wvdl\"; rm -rf \"$t\" \"$d/lib.new\"; mkdir -p \"$t\" \"$d/lib.new\" && cd \"$t\" && "
            "get() { h=$1; shift; for u in \"$@\"; do curl -fsSL -m 300 --retry 2 -o p.deb \"$u\" && "
            "[ \"$(sha256sum p.deb | cut -d' ' -f1)\" = \"$h\" ] && ar x p.deb data.tar.xz && "
            "xz -dc data.tar.xz | tar -x -f - && rm -f p.deb data.tar.xz && return 0; done; return 1; }; "
            "get %s '%s' && get %s '%s' '%s' && "
            "cp opt/WidevineCdm/gmp-widevinecdm/latest/libwidevinecdm.so usr/lib/aarch64-linux-gnu/libnspr4.so "
            "usr/lib/aarch64-linux-gnu/libplc4.so usr/lib/aarch64-linux-gnu/libplds4.so \"$d/lib.new/\" && "
            "rm -rf \"$d/lib\" && mv \"$d/lib.new\" \"$d/lib\" && touch '%s' || touch '%s'; "
            "cd /; rm -rf \"$t\" \"$d/lib.new\" ) >/dev/null 2>&1 &",
            dir, CDM_SHA, CDM_DEB, NSPR_SHA, NSPR_DEB, NSPR_DEB2, done, fail);
    bool ok = false;
    for (;;) {
        if (file_exists(done)) { ok = true; break; }
        if (file_exists(fail)) { app_toast(T("Could not download the Widevine module - check the network and try again")); break; }
        if (app_wait_frame(T("Downloading the Widevine module (first time, about 8 MB)…"))) break;
    }
    unlink(done); unlink(fail);
    free(done); free(fail);
    return ok;
}

/* Play one title/episode via wvserve + player_run. *started is false if wvserve
 * never served a manifest (missing files, resolve/license error, or the user
 * backed out while loading). The returned PlayResult carries pos/dur/eof. */
static PlayResult play_one(Tv2 *v, const char *guid, const char *title,
                           const char *sub, const char *img, bool *started) {
    PlayResult res;
    memset(&res, 0, sizeof res);
    res.cancelled = true;
    *started = false;
    if (!guid_is_safe(guid)) { app_toast(T("Nothing to play")); return res; }

    char *dir = app_dir();
    /* wvserve comes with the package; a test box may still have it in the data folder */
    const char *pkg = app_pkg_dir(&nota_app);
    char *wv = str_printf("%s/wvserve", pkg);
    if (!*pkg || !file_exists(wv)) { free(wv); wv = str_printf("%s/wvserve", dir); }
    char *cdm = str_printf("%s/lib/libwidevinecdm.so", dir);
    if (file_exists(wv) && !file_exists(cdm)) cdm_fetch(dir);
    char *tok = str_printf("%s/token", dir);
    char *log = str_printf("%s/serve.log", dir);
    char *posf = str_printf("%s/wvpos", dir);
    if (!file_exists(wv) || !file_exists(cdm)) {
        if (!file_exists(wv)) app_toast(T("The TV 2 player is missing - install the app again in the App Store"));
        free(dir); free(wv); free(cdm); free(tok); free(log); free(posf);
        return res;
    }
    write_token_file(v);
    unlink(posf); /* kun en position fra denne afspilning */

    double start = progress_load(guid);
    /* profileToken is base64/JWT (no quote); guard anyway so it can never break the shell word */
    char *prof = (v->profile[0] && !strchr(v->profile, '\'')) ? str_printf("TV2_PROFILE='%s' ", v->profile) : str_dup("");
    run_cmd("kill $(pidof wvserve) 2>/dev/null; sleep 0.3; "
            "%sTV2_POSFILE='%s' LD_LIBRARY_PATH='%s/lib' '%s' '%s' \"$(cat '%s')\" '%s' %d >'%s' 2>&1 &",
            prof, posf, dir, wv, cdm, tok, guid, PORT, log);
    free(prof);

    char url[80];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/manifest.mpd", PORT);
    bool ready = false, cancelled = false, failed = false;
    for (int i = 0; i < 80; i++) {
        if (app_wait_frame(T("Starting TV 2 playback…"))) { cancelled = true; break; }
        HttpBuf b;
        memset(&b, 0, sizeof b);
        if (http_get(url, NULL, &b) && b.status == 200) { http_free(&b); ready = true; break; }
        http_free(&b);
        /* wvserve exits immediately on resolve/license errors; do not wait 16 s */
        if ((i % 5) == 4) {
            char *why = file_read_all(log, 16000);
            if (why && (strstr(why, "resolve fejlede") || strstr(why, "ingen noegle") ||
                        strstr(why, "LicenseNotGranted") || strstr(why, "ConcurrencyLimitViolation") ||
                        strstr(why, "ingen WIDEVINE-stream") || strstr(why, "Missing media")))
                failed = true;
            free(why);
            if (failed) break;
        }
        usleep(200000);
    }
    if (!ready) {
        if (!cancelled) play_error_toast(log);
        run_cmd("kill $(pidof wvserve) 2>/dev/null");
        free(dir); free(wv); free(cdm); free(tok); free(log); free(posf);
        res.cancelled = cancelled;
        return res;
    }

    PlayRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.title = (title && *title) ? title : "TV 2 Play";
    rq.subtitle = sub;
    rq.poster = (img && *img) ? img : NULL;
    rq.start = start;
    /* mpv 0.40's --ytdl=no breaks its DASH/edl fallback for the local MPD
     * (the manifest is still recognized, but no tracks are selected). Keep
     * the hook enabled; it does nothing for a 127.0.0.1 URL. */
    static const char *const tv2_mpv_extra[] = {"--ytdl=yes", NULL};
    rq.extra = tv2_mpv_extra;
    res = player_run(&rq);
    if (!res.cancelled && res.pos > 5) {
        FILE *f = fopen(posf, "w");
        if (f) { fprintf(f, "%.1f", res.pos); fclose(f); }
    }
    /* Wait for wvserve to finish its UNLOCK (progress) before reloading Page. */
    run_cmd("kill $(pidof wvserve) 2>/dev/null; "
            "for i in 1 2 3 4 5 6 7 8 9 10; do pidof wvserve >/dev/null || break; sleep 0.1; done");
    if (!res.cancelled) {
        if (res.pos > 5 && res.dur > 0 && res.pos < res.dur - 15)
            progress_write(guid, res.pos, res.dur, false);
        else
            progress_write(guid, 0, 0, true);
    }
    free(dir); free(wv); free(cdm); free(tok); free(log); free(posf);
    *started = true;
    return res;
}

/* After an episode plays to the end, find the next one. Episodes live in one or
 * more sections titled "Afsnit" (details_done makes one; episodes_done chunks
 * them in fives), so treat all "Afsnit" sections as one ordered list. */
static bool next_episode(Tv2 *v, const char *guid, char **ng, char **nt, char **ns, char **ni) {
    bool take = false;
    for (int si = 0; si < v->nsec; si++) {
        Section *s = &v->sec[si];
        if (!s->title || strcmp(s->title, T("Episodes"))) continue;
        for (int i = 0; i < s->n; i++) {
            Card *c = &s->cards[i];
            if (take) {
                if (c->type != CARD_VOD || !c->guid || !*c->guid) return false;
                *ng = str_dup(c->guid); *nt = str_dup(c->title);
                *ns = str_dup(c->sub); *ni = str_dup(c->img);
                return true;
            }
            if (c->guid && guid && !strcmp(c->guid, guid)) take = true;
        }
    }
    return false;
}

static void tv2_play(Tv2 *v, const Card *c) {
    if (!c || !c->guid || !*c->guid) { app_toast(T("Nothing to play")); return; }

    char *guid = str_dup(c->guid), *title = str_dup(c->title);
    char *sub = str_dup(c->sub), *img = str_dup(c->img);
    bool to_login = false, first = true;
    for (;;) {
        if (!token_ensure(v)) { to_login = first; break; } /* keep current set; freed after loop */
        bool started = false;
        PlayResult res = play_one(v, guid, title, sub, img, &started);
        first = false;
        char *ng = NULL, *nt = NULL, *ns = NULL, *ni = NULL;
        /* auto-advance only when an episode played to its end on the series page */
        if (started && res.eof && !res.cancelled && v == live && v->view == V_DETAIL)
            next_episode(v, guid, &ng, &nt, &ns, &ni);
        free(guid); free(title); free(sub); free(img);
        guid = ng; title = nt; sub = ns; img = ni; /* next episode, or all NULL */
        if (!guid) break;
        app_toast(T("Next episode…"));
    }
    free(guid); free(title); free(sub); free(img);

    if (to_login) {
        v->state = S_LOGIN;
        snprintf(v->msg, sizeof v->msg, T("Your sign-in has expired. Sign in again."));
        return;
    }
    /* TV 2's server-side "Se videre" is updated by wvserve's concurrency
     * progress. Reload the current view so the new position is visible
     * immediately when the player closes. */
    if (v == live) {
        if (v->view == V_HOME) home_load(v);
        else if (v->view == V_DETAIL && v->path[0]) details_load(v, v->path, v->heading, v->detail_img);
        else if (v->view == V_BROWSE && v->path[0]) page_by_path(v, v->path, v->heading);
    }
}

/* ---------- navigation ---------- */
static void confirm_noop(void *ud, bool yes) { (void)ud; (void)yes; }
static void locked_prompt(Tv2 *v, const Card *c) {
    const char *title = (c->title && *c->title) ? c->title : T("This title");
    const char *why = (c->lock && *c->lock) ? c->lock : T("It must be bought or added to your subscription.");
    char *msg = str_printf(T("%s must be bought or added on before it can be played.\n\n%s\n\nAdd it in the TV 2 Play app or at tv2play.dk."), title, why);
    confirm_open(T("Not part of your subscription"), msg, T("Close"), "", confirm_noop, v);
    free(msg);
}

static void open_card(Tv2 *v, const Card *c) {
    if (!c) return;
    if (c->ent == 2) { locked_prompt(v, c); return; }
    switch (c->type) {
    case CARD_COLLECTION:
        if (c->path && *c->path) page_by_path(v, c->path, c->title);
        else app_toast(T("Empty collection"));
        break;
    case CARD_SERIES:
        if (c->path && *c->path) details_load(v, c->path, c->title, c->img);
        else app_toast(T("Could not open the series"));
        break;
    case CARD_LIVE:
        app_toast(T("Live TV needs live DRM, which is not supported yet"));
        break;
    default:
        if (c->path && (!strncmp(c->path, "/film/", 6) || !strncmp(c->path, "/program/", 9) ||
                        !strncmp(c->path, "/event/", 7)))
            details_load(v, c->path, c->title, c->img);
        else
            tv2_play(v, c);
        break;
    }
}

static void back_home(Tv2 *v) {
    v->view = V_HOME;
    v->side.active = 0;
    home_load(v);
}

static void retry_view(Tv2 *v) {
    if (!token_valid(v)) {
        if (v->refresh[0]) refresh_async(v);
        else start_login(v);
        return;
    }
    if (v->view == V_SEARCH && v->query[0]) search_submit(v, v->query);
    else if (v->view == V_DETAIL && v->path[0]) details_load(v, v->path, v->heading, v->detail_img);
    else if (v->view == V_BROWSE && v->path[0]) page_by_path(v, v->path, v->heading);
    else if (v->view == V_LIVE) live_load(v);
    else if (v->view == V_CATEGORIES) categories_load(v);
    else if (v->view == V_PROFILE) profile_open(v);
    else home_load(v);
}

static void search_open(Tv2 *v) {
    v->view = V_SEARCH;
    v->state = S_READY;
    v->nsec = 0;
    v->query[0] = 0;
    v->search_home = true;
    v->search_filter = SF_ALL;
    v->hist_sel = 0;
    v->heading[0] = 0;
    v->msg[0] = 0;
    clear_sections(v);
    keyboard_open(T("Search TV 2 Play"), "", false, search_done, v);
}

static void tv2_logout(void);

static void side_activate(Tv2 *v, int item) {
    int idx = item >= 0 && item < 8 ? v->side_map[item] : M_HOME;
    if (idx != M_LOGOUT) v->side.active = item;
    switch (idx) {
    case M_HOME:
        if (v->state == S_READY) back_home(v);
        else if (v->state == S_ERROR) retry_view(v);
        else if (v->state == S_LOGIN) start_login(v);
        break;
    case M_LIVE:
        if (v->state == S_READY) live_load(v);
        break;
    case M_CATEGORIES:
        if (v->state == S_READY) categories_load(v);
        break;
    case M_SEARCH:
        if (v->state == S_READY) search_open(v);
        break;
    case M_MYPAGE:
        if (v->state == S_READY) profile_open(v);
        break;
    case M_LOGOUT:
        tv2_logout();
        break;
    default: break;
    }
}

static void build_side(Tv2 *v) {
    side_clear(&v->side);
    memset(v->side_map, 0, sizeof v->side_map);
    v->side_map[0] = M_HOME;
    side_add(&v->side, T("Home"), ICON_TV, false);
    v->side_map[1] = M_LIVE;
    side_add(&v->side, T("Live TV"), ICON_PLAY, false);
    v->side_map[2] = M_CATEGORIES;
    side_add(&v->side, T("Categories"), ICON_FOLDER, false);
    v->side_map[3] = M_SEARCH;
    side_add(&v->side, T("Search"), ICON_SEARCH, false);
    v->side_map[4] = M_MYPAGE;
    side_add(&v->side, T("My page"), ICON_GEAR, false);
    v->side.active = 0;
}

/* ---------- drawing ---------- */
/* one UTF-8 letter of s (the initial on a profile without an avatar) */
static void initial_of(const char *s, char out[8]) {
    int l = 1;
    memset(out, 0, 8);
    if (!s || !*s) { out[0] = '?'; return; }
    if (s[0] & 0x80) while (l < 4 && (s[l] & 0xC0) == 0x80) l++;
    memcpy(out, s, l);
}

/* the side menu, with the chosen profile's avatar instead of the gear on "Min side" (drawn on top of it; the
   rows are laid out like side_draw in widgets.c: from y 190, 72 apart, no gaps in this menu) */
static void tv2_side_draw(Tv2 *v) {
    side_draw(&v->side);
    if (!v->profile_name[0] && !v->profile_avatar[0]) return;
    int k = -1;
    for (int i = 0; i < v->side.n && i < 8; i++) if (v->side_map[i] == M_MYPAGE) k = i;
    if (k < 0) return;
    float cx = SIDE_W / 2, cy = 190 - v->side.scroll + k * 72 + 31, d = 44;
    if (cy < 180 || cy > UI_H - 40) return;
    Image *im = v->profile_avatar[0] ? img_get(v->profile_avatar) : NULL;
    if (!im || !img_draw_cover(im, cx - d / 2, cy - d / 2, d, d, d / 2, 1)) {
        char ini[8];
        initial_of(v->profile_name, ini);
        fill_rrect(cx - d / 2, cy - d / 2, d, d, d / 2, TV2_RED);
        text_draw(F_BOLD, 24, cx, cy - 15, TV2_TEXT, AL_CENTER, ini);
    }
}

static void draw_tv2_topbar(const char *title) {
    float x = SIDE_W + 60;
    if (title) text_draw(F_BOLD, 44, x, 56, TV2_TEXT, AL_LEFT, title);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof clk, "%H:%M", &tm);
    text_draw(F_SEMI, 34, UI_W - 96, 62, TV2_TEXT, AL_RIGHT, clk);
}

static void draw_tv2_bg(void) {
    fill_rect(0, 0, UI_W, UI_H, TV2_MIDNIGHT);
    fill_circle(150, 40, 520, RGBA(240, 40, 30, 16));
    fill_circle(UI_W - 80, UI_H - 60, 620, RGBA(90, 30, 140, 14));
    fill_rrect_vgrad(0, 0, UI_W, 230, 0, RGBA(0, 5, 35, 220), RGBA(0, 5, 35, 0));
}

static void draw_label_chip(const char *text, const char *hex, float x, float y) {
    if (!text || !*text) return;
    Color bg = hex_color(hex, TV2_SAND);
    float tw = text_width(F_BOLD, TS_LABEL, text);
    float w = tw + 22;
    fill_rrect(x, y, w, 28, 6, bg);
    text_draw(F_BOLD, TS_LABEL, x + w / 2, y + 6,
              text_is_light(bg) ? TV2_BLACK : TV2_TEXT, AL_CENTER, text);
}

static void draw_card(const Card *c, float x, float y, bool focus) {
    float cw = card_width_for(c->fmt, c->size), ch = card_height_for(c->fmt, c->size);
    float fa = ui_focus(c, focus);
    float sc = 1.0f + 0.09f * fa;
    float ww = cw * sc, hh = ch * sc;
    float xx = x - (ww - cw) / 2, yy = y - (hh - ch) / 2;
    ui_focus_under(xx, yy, ww, hh, 12, fa);
    fill_rrect(xx, yy, ww, hh, 12, TV2_SURFACE);
    Image *im = (c->img && *c->img) ? img_get(c->img) : NULL;
    if (!im || !img_draw_cover(im, xx, yy, ww, hh, 12, 1)) {
        fill_rrect(xx, yy, ww, hh, 12, color_mix(TV2_SURFACE, TV2_RED, 0.28f));
        if (c->title)
            text_draw_wrap(F_SEMI, TS_H2, xx + 14, yy + 18, ww - 28, 3, 28, TV2_TEXT2,
                           c->acc && *c->acc ? c->acc : c->title);
    }
    /* Dim cards that are not in focus so the focused one clearly stands out
     * (the thin focus ring alone was too subtle over bright artwork). */
    if (fa < 0.999f)
        fill_rrect(xx, yy, ww, hh, 12, RGBA(0, 5, 35, (unsigned char)(130 * (1.0f - fa))));
    if (c->label && *c->label) draw_label_chip(c->label, c->label_color, xx + 12, yy + 12);
    else if (c->type == CARD_SERIES) {
        fill_rrect(xx + 12, yy + 12, 74, 28, 6, RGBA(0, 0, 0, 190));
        text_draw(F_BOLD, 16, xx + 49, yy + 17, TV2_TEXT, AL_CENTER, T("SERIES"));
    }
    if (c->ent == 2) {
        const char *badge = (c->lock && (strstr(c->lock, "Lej") || strstr(c->lock, "lej")))
                                ? T("RENT") : T("ADD-ON");
        float bw = 38 + text_width(F_BOLD, TS_LABEL, badge);
        float bx = xx + ww - bw - 12;
        fill_rrect(bx, yy + 12, bw, 28, 6, RGBA(0, 0, 0, 210));
        draw_icon(ICON_LOCK, bx + 16, yy + 26, 16, TV2_TEXT);
        text_draw(F_BOLD, TS_LABEL, bx + 30, yy + 18, TV2_TEXT, AL_LEFT, badge);
    }
    if (c->progress > 0) {
        float bw = ww - 24;
        fill_rect(xx + 12, yy + hh - 10, bw, 4, RGBA(255, 255, 255, 80));
        fill_rect(xx + 12, yy + hh - 10, bw * (float)(c->progress / 100.0), 4, TV2_RED);
    }
    ui_focus_over(xx, yy, ww, hh, 12, fa);
    /* a bright, clearly visible frame on the focused card (fades in with focus) */
    if (fa > 0.01f)
        stroke_rrect(xx, yy, ww, hh, 12, 3, RGBA(255, 255, 255, (unsigned char)(235 * fa)));
    int tpx = c->size == SZ_SMALL ? 18 : TS_H2;
    text_draw_clip(F_SEMI, tpx, x, y + ch + 10, cw, focus ? TV2_TEXT : TV2_TEXT2, AL_LEFT,
                   c->title ? c->title : (c->acc ? c->acc : ""));
    text_draw_clip(F_REG, c->size == SZ_SMALL ? 15 : TS_BODY, x, y + ch + 38, cw, TV2_TEXT3,
                   AL_LEFT, c->sub ? c->sub : "");
}

static float rows_top(Tv2 *v) {
    if (v->view == V_HOME) return v->nhero ? HERO_Y + HERO_H + 24 : 150;
    if (v->view == V_DETAIL) return DETAIL_H + 40;
    if (v->view == V_SEARCH) return 190;
    return 150;
}
static float section_sum_before(Tv2 *v, int row) {
    float h = 0;
    for (int i = 0; i < row && i < v->nsec; i++) h += section_h(&v->sec[i]);
    return h;
}
static float scroll_target(Tv2 *v) {
    if ((v->view == V_HOME && v->hero_focus) || (v->view == V_DETAIL && v->detail_focus) ||
        (v->view == V_SEARCH && v->search_home))
        return 0;
    float focus_y = v->view == V_SEARCH ? rows_top(v) : v->view == V_HOME ? 140 : v->view == V_DETAIL ? 140 : 150;
    float t = rows_top(v) + section_sum_before(v, v->row) - focus_y;
    return t > 0 ? t : 0;
}

static void draw_sections(Tv2 *v) {
    float x0 = SIDE_W + 60;
    float top = rows_top(v) - v->scroll;
    bool rows_focused = !((v->view == V_HOME && v->hero_focus) ||
                          (v->view == V_DETAIL && v->detail_focus));
    clip_push(0, 120, UI_W, UI_H - 180);
    float y = top;
    for (int si = 0; si < v->nsec; si++) {
        Section *s = &v->sec[si];
        float h = section_h(s);
        if (y + h >= 100 && y <= UI_H) {
            if (s->title && *s->title)
                text_draw(F_BOLD, TS_H1, x0, y, TV2_TEXT, AL_LEFT, s->title);
            int cols = section_cols(s);
            int first = s->off;
            if (si == v->row) {
                first = v->col >= 2 ? v->col - 2 : 0;
                s->off = first;
            }
            if (first > s->n - cols) first = s->n - cols;
            if (first < 0) first = 0;
            float cw = card_width_for(s->fmt, s->size);
            for (int j = 0; j < cols && first + j < s->n; j++) {
                float x = x0 + j * (cw + CARD_GAP);
                draw_card(&s->cards[first + j], x, y + 48,
                          rows_focused && si == v->row && first + j == v->col);
            }
        }
        y += h;
    }
    clip_pop();
}

static void draw_hero(Tv2 *v) {
    if (v->nhero <= 0) return;
    Hero *h = &v->hero[v->hero_i];
    float y = HERO_Y - v->scroll;
    if (y + HERO_H < 100) return;
    Image *bg = (h->img && *h->img) ? img_get(h->img) : NULL;
    if (bg) img_draw_cover(bg, 0, y, UI_W, HERO_H, 0, 1);
    else fill_rect(0, y, UI_W, HERO_H, TV2_NAVY);
    fill_rrect_vgrad(0, y, UI_W, HERO_H, 0, RGBA(0, 5, 35, 20), RGBA(0, 5, 35, 245));
    fill_hgrad(0, y, 1180, HERO_H, RGBA(0, 5, 35, 185), RGBA(0, 5, 35, 0));
    if (h->label && *h->label) draw_label_chip(h->label, h->label_color, SIDE_W + 60, y + 38);
    text_draw_wrap(F_BOLD, TS_HERO, SIDE_W + 60, y + 80, 780, 2, 58, TV2_TEXT,
                   h->title ? h->title : (h->acc ? h->acc : ""));
    if (h->sub)
        text_draw_wrap(F_REG, TS_BODY, SIDE_W + 60, y + 258, 740, 2, 28, TV2_TEXT2, h->sub);
    const char *cta = (h->cta && *h->cta) ? h->cta : T("More info");
    float tw = text_width(F_SEMI, TS_BUTTON, cta);
    float bw = tw + 64, bh = 58, bx = SIDE_W + 60, by = y + 390;
    if (v->hero_focus) fill_rrect(bx - 3, by - 3, bw + 6, bh + 6, 32, TV2_TEXT);
    Color bgc = h->cta_ent ? TV2_RED : TV2_TEXT;
    Color fgc = h->cta_ent ? TV2_TEXT : TV2_BLACK;
    fill_rrect(bx, by, bw, bh, 29, bgc);
    text_draw(F_SEMI, TS_BUTTON, bx + bw / 2, by + 15, fgc, AL_CENTER, cta);
    if (v->nhero > 1) {
        float cx = UI_W / 2;
        for (int i = 0; i < v->nhero; i++)
            fill_circle(cx + (i - (v->nhero - 1) / 2.0f) * 18, y + HERO_H - 26, 4,
                        i == v->hero_i ? TV2_TEXT : RGBA(255, 255, 255, 85));
    }
}

static void draw_detail_header(Tv2 *v) {
    float y = HERO_Y - v->scroll;
    if (y + DETAIL_H < 100) return;
    Image *bg = (v->detail_img[0]) ? img_get(v->detail_img) : NULL;
    if (bg) img_draw_cover(bg, -420, y, UI_W + 420, DETAIL_H, 0, 1);
    else fill_rect(0, y, UI_W, DETAIL_H, TV2_NAVY);
    fill_hgrad(0, y, 1180, DETAIL_H, RGBA(0, 5, 35, 248), RGBA(0, 5, 35, 0));
    fill_rrect_vgrad(0, y, UI_W, DETAIL_H, 0, RGBA(0, 5, 35, 0), RGBA(0, 5, 35, 165));
    text_draw_wrap(F_BOLD, 46, SIDE_W + 60, y + 90, 900, 2, 54, TV2_TEXT,
                   v->heading[0] ? v->heading : T("TV 2 Play"));
    if (v->detail_desc[0])
        text_draw_wrap(F_REG, 20, SIDE_W + 60, y + 200, 780, 3, 29, TV2_TEXT2, v->detail_desc);
    const char *cta = v->detail_cta[0] ? v->detail_cta : T("Play");
    float tw = text_width(F_SEMI, TS_BUTTON, cta);
    float bw = tw + 64, bh = 58, bx = SIDE_W + 60, by = y + 390;
    if (v->detail_focus) fill_rrect(bx - 3, by - 3, bw + 6, bh + 6, 32, TV2_TEXT);
    Color bgc = v->detail_cta_ent ? TV2_RED : TV2_TEXT;
    Color fgc = v->detail_cta_ent ? TV2_TEXT : TV2_BLACK;
    fill_rrect(bx, by, bw, bh, 29, bgc);
    text_draw(F_SEMI, TS_BUTTON, bx + bw / 2, by + 15, fgc, AL_CENTER, cta);
}

static void draw_search_filter(Tv2 *v) {
    float x = SIDE_W + 60, y = 132;
    const char *labels[3] = {T("All"), T("Movies"), T("Series")};
    for (int i = 0; i < 3; i++) {
        float w = text_width(F_SEMI, TS_LABEL, labels[i]) + 34;
        fill_rrect(x, y, w, 36, 18, v->search_filter == i ? TV2_TEXT : TV2_SURFACE);
        text_draw(F_SEMI, TS_LABEL, x + w / 2, y + 9,
                  v->search_filter == i ? TV2_BLACK : TV2_TEXT2, AL_CENTER, labels[i]);
        x += w + 12;
    }
}

static void draw_search_home(Tv2 *v) {
    float x = SIDE_W + 60, y = 160;
    if (v->nhist > 0) {
        text_draw(F_BOLD, TS_H2, x, y, TV2_TEXT, AL_LEFT, T("Recent searches"));
        text_draw(F_REG, TS_BODY, x, y + 44, TV2_TEXT2, AL_LEFT,
                  T("Press OK to open the keyboard"));
        for (int i = 0; i < v->nhist; i++) {
            float yy = y + 100 + i * 64;
            bool sel = i == v->hist_sel;
            fill_rrect(x, yy, 760, 52, 10, sel ? TV2_TEXT : TV2_SURFACE);
            text_draw(F_SEMI, TS_BODY, x + 20, yy + 14,
                      sel ? TV2_BLACK : TV2_TEXT2, AL_LEFT, v->hist[i]);
        }
    } else {
        text_draw(F_BOLD, TS_H1, x, y, TV2_TEXT, AL_LEFT, T("Search"));
        text_draw(F_REG, TS_BODY, x, y + 48, TV2_TEXT2, AL_LEFT,
                  T("Press OK to open the keyboard"));
        text_draw(F_REG, TS_BODY, x, y + 130, TV2_TEXT3, AL_LEFT,
                  T("No earlier searches yet"));
    }
}

static void draw_profile(Tv2 *v) {
    float pw = 720, ph = 500, px = UI_W / 2 - pw / 2, py = 190;
    fill_rrect(px, py, pw, ph, 24, TV2_SURFACE);
    text_draw(F_BOLD, 40, UI_W / 2, py + 42, TV2_TEXT, AL_CENTER, T("My page"));
    text_draw(F_SEMI, 30, UI_W / 2, py + 120, TV2_TEXT, AL_CENTER,
              v->profile_name[0] ? v->profile_name : T("My profile"));
    text_draw(F_REG, TS_BODY, UI_W / 2, py + 168, TV2_TEXT2, AL_CENTER,
              T("Signed in to TV 2 Play"));
    const char *btn[2] = {T("Switch profile"), T("Sign out")};
    float bw[2], gap = 30, tot = gap;
    for (int i = 0; i < 2; i++) { bw[i] = text_width(F_SEMI, TS_BUTTON, btn[i]) + 70; tot += bw[i]; }
    float bx = UI_W / 2 - tot / 2, by = py + 320;
    for (int i = 0; i < 2; i++) {
        bool f = v->profile_sel == i;
        if (f) fill_rrect(bx - 3, by - 3, bw[i] + 6, 64, 32, TV2_TEXT);
        fill_rrect(bx, by, bw[i], 58, 29, f ? TV2_TEXT : TV2_SURFACE2);
        text_draw(F_SEMI, TS_BUTTON, bx + bw[i] / 2, by + 15, f ? TV2_BLACK : TV2_TEXT, AL_CENTER, btn[i]);
        bx += bw[i] + gap;
    }
    text_draw(F_REG, TS_SMALL, UI_W / 2, py + ph - 40, TV2_TEXT3, AL_CENTER, "TV 2 Play " APP_VERSION);
}

/* "Hvem ser med?": the account's profiles as round avatars with the name below */
static void draw_who(Tv2 *v) {
    text_draw(F_BOLD, TS_H1, UI_W / 2, 220, TV2_TEXT, AL_CENTER, T("Who's watching?"));
    float d = 200, gap = 70, tot = v->nprof * d + (v->nprof - 1) * gap;
    float x = UI_W / 2 - tot / 2, y = 380;
    for (int i = 0; i < v->nprof; i++, x += d + gap) {
        Tv2Prof *p = &v->prof[i];
        bool f = i == v->who_sel;
        float sc = f ? 1.08f : 1, dd = d * sc, xx = x - (dd - d) / 2, yy = y - (dd - d) / 2;
        if (f) fill_rrect(xx - 6, yy - 6, dd + 12, dd + 12, (dd + 12) / 2, TV2_TEXT);
        Image *im = p->avatar[0] ? img_get(p->avatar) : NULL;
        if (!im || !img_draw_cover(im, xx, yy, dd, dd, dd / 2, f ? 1 : 0.75f)) {
            fill_rrect(xx, yy, dd, dd, dd / 2, f ? TV2_RED : TV2_SURFACE2);
            char ini[8];
            initial_of(p->name, ini);
            text_draw(F_BOLD, 90, xx + dd / 2, yy + dd / 2 - 55, TV2_TEXT, AL_CENTER, ini);
        }
        text_draw_clip(F_SEMI, 30, x + d / 2, y + d + 30, d + gap, f ? TV2_TEXT : TV2_TEXT2, AL_CENTER, p->name);
        if (p->kids) text_draw(F_REG, TS_SMALL, x + d / 2, y + d + 74, TV2_TEXT3, AL_CENTER, T("Kids profile"));
    }
}

static void hero_open(Tv2 *v) {
    if (!v->nhero) return;
    Hero *h = &v->hero[v->hero_i];
    if (h->cta_ent) {
        Card c;
        memset(&c, 0, sizeof c);
        c.title = h->title;
        c.lock = (char *)T("Must be bought or added on");
        locked_prompt(v, &c);
        return;
    }
    const char *path = (h->cta_path && *h->cta_path) ? h->cta_path : h->path;
    if (!path || !*path) { app_toast(T("Nothing to open")); return; }
    char *guid = strstr(path, "/afspil/") ? guid_from_play_url(path) : NULL;
    if (guid) {
        Card c;
        memset(&c, 0, sizeof c);
        c.guid = guid;
        c.path = str_dup(path);
        c.title = str_dup(h->title);
        c.sub = str_dup(h->sub);
        c.img = str_dup(h->img);
        c.type = CARD_VOD;
        tv2_play(v, &c);
        card_free(&c);
        return;
    }
    if (!strncmp(path, "/serie/", 7) || !strncmp(path, "/film/", 6) ||
        !strncmp(path, "/program/", 9) || !strncmp(path, "/event/", 7))
        details_load(v, path, h->title, h->img);
    else page_by_path(v, path, h->title);
}

static void detail_cta_open(Tv2 *v) {
    if (v->detail_cta_ent) {
        Card c;
        memset(&c, 0, sizeof c);
        c.title = v->heading;
        c.lock = (char *)T("Must be bought or added on");
        locked_prompt(v, &c);
        return;
    }
    char *guid = strstr(v->detail_cta_path, "/afspil/") ? guid_from_play_url(v->detail_cta_path) : NULL;
    if (guid) {
        Card c;
        memset(&c, 0, sizeof c);
        c.guid = guid;
        c.path = str_dup(v->detail_cta_path);
        c.title = str_dup(v->heading);
        c.sub = str_dup(v->detail_desc);
        c.img = str_dup(v->detail_img);
        c.type = CARD_VOD;
        tv2_play(v, &c);
        card_free(&c);
        return;
    }
    if (v->detail_cta_path[0] && strcmp(v->detail_cta_path, v->path))
        details_load(v, v->detail_cta_path, v->heading, v->detail_img);
    else
        app_toast(T("This title cannot be played yet"));
}

static void tv2_draw(Screen *s) {
    Tv2 *v = s->st;
    draw_tv2_bg();
    float cx = UI_W / 2;

    if (v->state != S_READY) {
        draw_icon(ICON_PLAY, cx, 200, 100, TV2_RED);
        if (v->state == S_CODE) {
            text_draw(F_BOLD, 42, cx, 320, TV2_TEXT, AL_CENTER, T("Sign in to TV 2 Play"));
            text_draw(F_REG, 28, cx - 520, 400, TV2_TEXT2, AL_LEFT, T("1. Go to auth.tv2.dk/activate"));
            text_draw(F_REG, 28, cx - 520, 444, TV2_TEXT2, AL_LEFT, T("   - or scan the QR code"));
            text_draw(F_REG, 28, cx - 520, 520, TV2_TEXT2, AL_LEFT, T("2. Enter the code:"));
            text_draw(F_BOLD, 64, cx - 520, 570, TV2_TEXT, AL_LEFT, v->user_code);
            draw_qr(v->verify_complete[0] ? v->verify_complete : v->verify_uri, cx + 260, 400, 280);
            draw_spinner(cx - 480, 740, 16, TV2_TEXT3);
            text_draw(F_REG, 22, cx - 440, 726, TV2_TEXT3, AL_LEFT, T("Waiting for approval…"));
        } else if (v->state == S_LOADING) {
            draw_spinner(cx, 480, 30, TV2_TEXT);
            text_draw(F_REG, 26, cx, 550, TV2_TEXT2, AL_CENTER, v->msg);
        } else {
            text_draw(F_BOLD, 42, cx, 330, TV2_TEXT, AL_CENTER,
                      v->state == S_ERROR ? T("Something went wrong") : T("Sign in to TV 2 Play"));
            text_draw_wrap(F_REG, 28, cx - 520, 400, 1040, 6, 40, TV2_TEXT2,
                           v->msg[0] ? v->msg : T("Watch TV 2 Play on your TV.\n\nPress OK to sign in with a code on the screen."));
            const char *b[1] = {v->state == S_ERROR ? T("Try again") : T("Sign in")};
            draw_buttons(cx, 780, b, 1, 0);
            text_draw(F_REG, 22, cx, UI_H - 70, TV2_TEXT3, AL_CENTER, T("Left: menu"));
        }
        draw_tv2_topbar("TV 2 Play");
        tv2_side_draw(v);
        return;
    }

    switch (v->view) {
    case V_HOME:
        draw_hero(v);
        if (v->nsec) draw_sections(v);
        else if (!v->nhero) draw_empty("TV 2 Play", v->msg[0] ? v->msg : T("Nothing here"));
        draw_tv2_topbar(T("Home"));
        draw_keybar(SIDE_W + 60, UI_H - 62, T("Play"), NULL, NULL, T("Search"), T("Menu"));
        break;
    case V_DETAIL:
        draw_detail_header(v);
        if (v->nsec) draw_sections(v);
        else if (v->msg[0] && strcmp(v->msg, T("Nothing to play")))
            text_draw_wrap(F_REG, TS_BODY, SIDE_W + 60, rows_top(v) - v->scroll,
                           UI_W - SIDE_W - 120, 4, 30, TV2_TEXT2, v->msg);
        draw_tv2_topbar("TV 2 Play");
        draw_keybar(SIDE_W + 60, UI_H - 62, T("Play"), NULL, NULL, T("Search"), T("Menu"));
        break;
    case V_SEARCH:
        if (v->search_home) {
            draw_search_home(v);
            draw_keybar(SIDE_W + 60, UI_H - 62, T("Search"), T("Clear"), NULL, T("Search"), T("Menu"));
        } else {
            draw_search_filter(v);
            if (v->nsec) draw_sections(v);
            else
                text_draw_wrap(F_REG, TS_BODY, SIDE_W + 60, rows_top(v) - v->scroll,
                               UI_W - SIDE_W - 120, 4, 30, TV2_TEXT2,
                               v->msg[0] ? v->msg : T("No results"));
            draw_keybar(SIDE_W + 60, UI_H - 62, T("Play"), T("Filter"), NULL, T("Search"), T("Menu"));
        }
        draw_tv2_topbar(T("Search"));
        break;
    case V_PROFILE:
        draw_profile(v);
        draw_tv2_topbar(T("My page"));
        draw_keybar(SIDE_W + 60, UI_H - 62, T("Select"), NULL, NULL, NULL, T("Menu"));
        break;
    case V_WHO:
        draw_who(v);
        draw_keybar(SIDE_W + 60, UI_H - 62, T("Select"), NULL, NULL, NULL, NULL);
        break;
    default:
        if (v->nsec) draw_sections(v);
        else draw_empty(v->heading[0] ? v->heading : "TV 2 Play",
                        v->msg[0] ? v->msg : T("Nothing here"));
        draw_tv2_topbar("TV 2 Play");
        draw_keybar(SIDE_W + 60, UI_H - 62, T("Select"), NULL, NULL, T("Search"), T("Menu"));
        break;
    }
    tv2_side_draw(v);
}

/* ---------- input / screen ---------- */
static bool tv2_input(Screen *s, const InputEvent *ev) {
    Tv2 *v = s->st;
    int act;
    if (side_input(&v->side, ev, &act)) {
        if (act >= 0) side_activate(v, act);
        return true;
    }
    if (v->state != S_READY) {
        if ((ev->a == A_OK || ev->a == A_PLAYPAUSE) && !ev->repeat &&
            (v->state == S_LOGIN || v->state == S_ERROR)) {
            if (v->state == S_ERROR) retry_view(v);
            else start_login(v);
            return true;
        }
        if (ev->a == A_LEFT || ev->a == A_MENU) { side_open(&v->side); return true; }
        return false;
    }

    if (v->view == V_WHO) {
        if (ev->a == A_LEFT && v->who_sel > 0) v->who_sel--;
        else if (ev->a == A_RIGHT && v->who_sel + 1 < v->nprof) v->who_sel++;
        else if ((ev->a == A_OK || ev->a == A_PLAYPAUSE || ev->a == A_PLAY) && !ev->repeat) {
            profile_use(v, v->who_sel);
            v->gen++;
            home_load(v);
        } else if (ev->a == A_BACK) {
            if (v->profile[0]) profile_open(v); /* "Skift profil" cancelled: keep the current one */
            else return false;                 /* no profile chosen yet: leave the app */
        }
        return true;
    }

    if (v->view == V_PROFILE) {
        if (ev->a == A_LEFT && v->profile_sel > 0) v->profile_sel--;
        else if (ev->a == A_RIGHT && v->profile_sel < 1) v->profile_sel++;
        else if ((ev->a == A_OK || ev->a == A_PLAYPAUSE || ev->a == A_PLAY) && !ev->repeat) {
            if (v->profile_sel == 1) tv2_logout();
            else {
                v->force_pick = true;
                v->gen++;
                set_loading(v, T("Getting profiles…"));
                load_profiles(v);
            }
        } else if (ev->a == A_BACK) back_home(v);
        else if (ev->a == A_MENU || (ev->a == A_LEFT && v->profile_sel == 0)) side_open(&v->side);
        return true;
    }

    if (v->view == V_SEARCH && v->search_home) {
        if (ev->a == A_UP && v->hist_sel > 0) v->hist_sel--;
        else if (ev->a == A_DOWN && v->hist_sel + 1 < v->nhist) v->hist_sel++;
        else if ((ev->a == A_OK || ev->a == A_PLAYPAUSE) && v->nhist > 0) {
            snprintf(v->query, sizeof v->query, "%s", v->hist[v->hist_sel]);
            v->search_home = false;
            search_submit(v, v->query);
        } else if (ev->a == A_INFO) {
            v->nhist = 0;
            v->hist_sel = 0;
            history_save(v);
        } else if (ev->a == A_SEARCH || ev->a == A_CHAR) {
            char init[8] = {0};
            if (ev->a == A_CHAR) { init[0] = (char)ev->ch; init[1] = 0; }
            keyboard_open(T("Search TV 2 Play"), init, false, search_done, v);
        } else if (ev->a == A_BACK) back_home(v);
        else if (ev->a == A_MENU || ev->a == A_LEFT) side_open(&v->side);
        return true;
    }

    if (v->view == V_HOME && v->hero_focus) {
        if (ev->a == A_LEFT && v->nhero > 1) v->hero_i = (v->hero_i + v->nhero - 1) % v->nhero;
        else if (ev->a == A_RIGHT && v->nhero > 1) v->hero_i = (v->hero_i + 1) % v->nhero;
        else if (ev->a == A_DOWN && v->nsec > 0) { v->hero_focus = false; v->row = 0; v->col = 0; }
        else if (ev->a == A_OK || ev->a == A_PLAYPAUSE || ev->a == A_PLAY) hero_open(v);
        else if (ev->a == A_SEARCH) search_open(v);
        else if (ev->a == A_MENU) side_open(&v->side);
        else if (ev->a == A_BACK) return false;
        else if (ev->a == A_CHAR) {
            char init[8] = {(char)ev->ch, 0};
            keyboard_open(T("Search TV 2 Play"), init, false, search_done, v);
        }
        return true;
    }

    if (v->view == V_DETAIL && v->detail_focus) {
        if (ev->a == A_DOWN && v->nsec > 0) { v->detail_focus = false; v->row = 0; v->col = 0; }
        else if (ev->a == A_OK || ev->a == A_PLAYPAUSE || ev->a == A_PLAY) detail_cta_open(v);
        else if (ev->a == A_BACK) back_home(v);
        else if (ev->a == A_MENU || ev->a == A_LEFT) side_open(&v->side);
        else if (ev->a == A_SEARCH) search_open(v);
        return true;
    }

    if (v->view == V_SEARCH && ev->a == A_INFO) {
        v->search_filter = (v->search_filter + 1) % 3;
        if (v->query[0]) search_submit(v, v->query);
        return true;
    }

    if (v->nsec == 0) {
        if (ev->a == A_BACK && v->view != V_HOME) { back_home(v); return true; }
        if (ev->a == A_SEARCH || ev->a == A_CHAR) { search_open(v); return true; }
        if (ev->a == A_MENU || ev->a == A_LEFT) { side_open(&v->side); return true; }
        return false;
    }
    Section *sec = &v->sec[v->row];
    switch (ev->a) {
    case A_UP:
        if (v->row > 0) {
            v->row--;
            if (v->col >= v->sec[v->row].n) v->col = v->sec[v->row].n - 1;
        } else if (v->view == V_HOME && v->nhero) v->hero_focus = true;
        else if (v->view == V_DETAIL) v->detail_focus = true;
        return true;
    case A_DOWN:
        if (v->row + 1 >= v->nsec) return false;
        v->row++;
        if (v->col >= v->sec[v->row].n) v->col = v->sec[v->row].n - 1;
        return true;
    case A_LEFT:
        if (v->col > 0) v->col--;
        else side_open(&v->side);
        return true;
    case A_RIGHT:
        if (v->col + 1 < sec->n) v->col++;
        return true;
    case A_OK: case A_PLAYPAUSE: case A_PLAY:
        if (ev->repeat || v->col >= sec->n) return true;
        open_card(v, &sec->cards[v->col]);
        return true;
    case A_MENU: side_open(&v->side); return true;
    case A_SEARCH: search_open(v); return true;
    case A_CHAR: {
        char init[8] = {(char)ev->ch, 0};
        keyboard_open(T("Search TV 2 Play"), init, false, search_done, v);
        return true;
    }
    case A_BACK:
        if (v->view == V_SEARCH) {
            v->search_home = true;
            v->query[0] = 0;
            v->nsec = 0;
            v->state = S_READY;
            clear_sections(v);
            return true;
        }
        if (v->view != V_HOME) { back_home(v); return true; }
        return false;
    default:
        return false;
    }
}

static void tv2_update(Screen *s, float dt) {
    Tv2 *v = s->st;
    side_update(&v->side, dt);
    if (v->state == S_CODE && !v->polling && app_time() >= v->poll_next) {
        if (app_time() > v->code_expires) {
            set_error(v, T("The code has expired - press OK for a new one"));
            return;
        }
        v->polling = true;
        v->poll_next = app_time() + v->interval;
        char *gt = url_encode("urn:ietf:params:oauth:grant-type:device_code");
        char *body = str_printf("grant_type=%s&device_code=%s&client_id=%s", gt, v->device_code, TV2_CLIENT_ID);
        free(gt);
        tv2_form(AUTH_TOKEN_URL, body, token_done, v);
        free(body);
    }
    if (v->state == S_READY) {
        float target = scroll_target(v);
        v->scroll = anim_to(v->scroll, target, dt, 12);
    }
}

static void tv2_enter(Screen *s) {
    Tv2 *v = s->st;
    live = v;
    side_init(&v->side, "TV 2 Play", ICON_FILM, TV2_RED);
    build_side(v);
    v->gen++;
    v->token[0] = 0;
    v->refresh[0] = 0;
    v->profile[0] = 0;
    v->profile_name[0] = 0;
    v->auth_retried = false;
    read_token_file(v);
    snprintf(v->refresh, sizeof v->refresh, "%s", cfg_get("tv2_refresh", ""));
    snprintf(v->profile, sizeof v->profile, "%s", cfg_get("tv2_profile", ""));
    snprintf(v->profile_name, sizeof v->profile_name, "%s", cfg_get("tv2_profile_name", ""));
    snprintf(v->profile_avatar, sizeof v->profile_avatar, "%s", cfg_get("tv2_profile_avatar", ""));
    side_profile(v);
    v->token_exp = atof(cfg_get("tv2_token_exp", "0"));
    v->view = V_HOME;
    v->nsec = 0;
    v->nhero = 0;
    v->hero_i = 0;
    v->hero_focus = false;
    v->detail_focus = false;
    v->search_home = false;
    v->search_filter = SF_ALL;
    v->hist_sel = 0;
    v->nhist = 0;
    v->profile_sel = 0;
    v->path[0] = 0;
    v->detail_desc[0] = 0;
    v->detail_img[0] = 0;
    v->detail_cta[0] = 0;
    v->detail_cta_path[0] = 0;
    v->detail_cta_ent = 0;
    v->row = v->col = 0;
    v->scroll = 0;
    v->msg[0] = 0;
    history_load(v);

    if (token_valid(v)) {
        if (v->profile[0]) {
            home_load(v);
            v->bg_profiles = true; /* fresh name, picture and token for the chosen profile */
            load_profiles(v);
        } else {
            v->gen++;
            set_loading(v, T("Getting your profile…"));
            load_profiles(v);
        }
    } else if (v->refresh[0]) {
        refresh_async(v);
    } else {
        v->state = S_LOGIN;
        snprintf(v->msg, sizeof v->msg, T("Press OK to sign in to TV 2 Play."));
    }
}

static void tv2_resume(Screen *s) {
    Tv2 *v = s->st;
    live = v;
}

static void tv2_destroy(Screen *s) {
    Tv2 *v = s->st;
    clear_hero(v);
    clear_sections(v);
    if (live == v) live = NULL;
    v->gen++;
    free(v);
    free(s);
}

Screen *tv2_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    Tv2 *v = calloc(1, sizeof *v);
    s->name = "tv2";
    s->st = v;
    s->enter = tv2_enter;
    s->resume = tv2_resume;
    s->update = tv2_update;
    s->draw = tv2_draw;
    s->input = tv2_input;
    s->destroy = tv2_destroy;
    return s;
}

/* ---------- sign out / app ---------- */
static bool has_login(void) {
    char path[600];
    token_path(path, sizeof path);
    return file_exists(path) || *cfg_get("tv2_refresh", "");
}
static const char *tv2_signed_out(void) { return has_login() ? NULL : ITEM_HIDDEN; }

static void tv2_forget(void) {
    run_cmd("kill $(pidof wvserve) 2>/dev/null");
    clear_token_file();
    char *p = app_file("progress");
    unlink(p);
    free(p);
    p = app_dir();
    run_cmd("rm -f '%s'/progress-* 2>/dev/null", p); /* one per profile */
    free(p);
    p = app_file("serve.log");
    unlink(p);
    free(p);
    p = app_file("search_history");
    unlink(p);
    free(p);
    p = app_file("wvpos");
    unlink(p);
    free(p);
    p = app_file("profile"); /* legacy file from an earlier variant */
    unlink(p);
    free(p);
    cfg_set("tv2_refresh", "");
    cfg_set("tv2_token_exp", "0");
    cfg_set("tv2_profile", "");
    cfg_set("tv2_profile_name", "");
    cfg_set("tv2_profile_id", "");
    cfg_set("tv2_profile_avatar", "");
    if (http_cookies_forget) http_cookies_forget();
    wipe_session_traces();
    if (live) {
        live->gen++;
        live->token[0] = 0;
        live->refresh[0] = 0;
        live->profile[0] = 0;
        live->profile_name[0] = 0;
        live->profile_avatar[0] = 0;
        side_profile(live); /* back to "Min side" */
        live->nhist = 0;
        live->nprof = 0;
        clear_hero(live);
        live->token_exp = 0;
        live->auth_retried = false;
        clear_sections(live);
        live->state = S_LOGIN;
        snprintf(live->msg, sizeof live->msg, T("You are signed out of TV 2 Play."));
    }
}

static void tv2_logout(void) { app_ask_signout("TV 2 Play", tv2_forget); }

static void tv2_mode_changed(void) {
    Tv2 *v = live;
    if (!v || v->state != S_READY) return;
    if (v->view == V_SEARCH && v->query[0]) search_submit(v, v->query);
    else if (v->view == V_DETAIL && v->path[0]) details_load(v, v->path, v->heading, v->detail_img);
    else if (v->view == V_BROWSE && v->path[0]) page_by_path(v, v->path, v->heading);
    else if (v->view == V_LIVE) live_load(v);
    else if (v->view == V_CATEGORIES) categories_load(v);
    else if (v->view == V_PROFILE) profile_open(v);
    else home_load(v);
}

static const char *const tv2_locked_o[] = {"0", "1", "2", NULL};
static const char *const tv2_locked_l[] = {
    N_("Show with a lock"), N_("Hide on the front page"), N_("Hide everywhere"), NULL
};
static const Item tv2_items[] = {
    {N_("Buy/add-on"), IT_CYCLE, "tv2_locked", "1", tv2_locked_o, tv2_locked_l,
     .help = N_("TV 2 titles that must be bought or added on: show them with a lock, hide them on the front page, or hide them everywhere. Search results show them with a lock in the first two choices."),
     .changed = tv2_mode_changed},
    {N_("Sign out of TV 2 Play"), IT_ACTION, .action = tv2_logout,
     .help = N_("Deletes the sign-in and profile from this TV"), .locked = tv2_signed_out},
};

/* Not installed from the start, and the App Store only lists it when "closed software" is ticked
 * (manifest "nonfree": playback needs Google's Widevine module). */
const NotaApp nota_app = {
    NOTA_API, "tv2", "TV 2 Play", N_("Streaming from TV 2 Play (Widevine)"), APP_VERSION, 35, APP_DEFAULT_OFF,
    ICON_FILM, {214, 0, 28, 255}, {140, 0, 18, 255}, {255, 255, 255, 255},
    .open = tv2_screen,
    .forget = tv2_forget,
    .settings = tv2_items,
    .n_settings = sizeof tv2_items / sizeof tv2_items[0],
};
