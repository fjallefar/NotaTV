/* NotaTV app package "drtv" - DRTV: DR's live channels, the front page, categories, A-Z, kids and search.
 * Played with mpv: DR sends plain HLS (the "encrypted" streams are HLS AES-128, which ffmpeg opens itself), so no
 * DRM module is needed. Works without an account (DR's anonymous token). Signing in with a DR account (e-mail and
 * password at login.dr.dk, the same login as dr.dk) adds DR's own "Continue watching" and "My list".
 * API: production.dr-massive.com, the API DR's web player uses. Built as app.so (see src/apps/README.md). */
#include "tv.h"
#include "vendor/cJSON.h"
#include <curl/curl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef DR_CLIENT_ID
#define DR_CLIENT_ID "" /* see app.mk */
#endif
#define DR_API "https://production.dr-massive.com/api"
#define DR_LOGIN "https://login.dr.dk"
#define DR_CALLBACK "https://www.dr.dk/drtv/callback"
#define DR_UA "Mozilla/5.0 (X11; Linux aarch64) NotaTV/" TV_VERSION
#define DR_RED RGB(214, 22, 52)
#define LIVE_M3U "/tmp/notatv-drtv.m3u"
#define tlog(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))

#define MAX_ROWS 48
#define TILE_W 384
#define TILE_H 216
#define BIG_W 560
#define BIG_H 315
#define GAP 28
#define DETAIL_TOP 610

static bool can_login(void) { return DR_CLIENT_ID[0] != 0; }
static const char *dr_user(void) { return cfg_get("drtv_user", ""); }
static bool signed_in(void) { return *dr_user() != 0; }

/* ---------- small helpers ---------- */
static char *js(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? str_dup(v->valuestring) : NULL;
}
static const char *jv(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static double jnum(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}
static char *jid(cJSON *o, const char *k) { /* ids come as strings or numbers */
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v) && v->valuestring[0]) return str_dup(v->valuestring);
    if (cJSON_IsNumber(v)) return str_printf("%.0f", v->valuedouble);
    return NULL;
}
static time_t iso_time(const char *s) {
    struct tm tm = {0};
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 5)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon--;
    return timegm(&tm);
}
static bool id_ok(const char *s) { /* item ids go into URLs: digits only */
    if (!s || !*s || strlen(s) > 20) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}
/* DR's images have Width=..&Height=.. in the address: ask for the size it is drawn in (big images cost the Pi) */
static char *param_set(char *s, const char *key, int v) {
    char *p = strstr(s, key);
    if (!p) return s;
    char *q = p + strlen(key);
    while (*q >= '0' && *q <= '9') q++;
    char *n = str_printf("%.*s%s%d%s", (int)(p - s), s, key, v, q);
    free(s);
    return n;
}
static char *img_sized(const char *u, int w, int h) {
    if (!u || !*u) return NULL;
    char *o = str_dup(u);
    o = param_set(o, "Width=", w);
    return param_set(o, "Height=", h);
}
static char *app_file(const char *name) { return str_printf("%s/apps/drtv/%s", data_dir(), name); }
static void ensure_dir(void) {
    char *b = str_printf("%s/apps", data_dir());
    mkdir(b, 0700);
    free(b);
    b = str_printf("%s/apps/drtv", data_dir());
    mkdir(b, 0700);
    free(b);
}

/* ---------- SHA-256 + base64url (PKCE for DR's login) ---------- */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
static void sha256(const void *data, size_t n, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t *p = data;
    size_t left = n;
    for (; left >= 64; p += 64, left -= 64) sha256_block(h, p);
    uint8_t blk[128] = {0};
    memcpy(blk, p, left);
    blk[left] = 0x80;
    size_t tot = left + 9 > 64 ? 128 : 64;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) blk[tot - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, blk);
    if (tot == 128) sha256_block(h, blk + 64);
    for (int i = 0; i < 8; i++) { out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i]; }
}
static void b64url(const uint8_t *in, size_t n, char *out) {
    static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = tb[(v >> 18) & 63];
        out[o++] = tb[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = tb[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = tb[v & 63];
    }
    out[o] = 0;
}
static void rand_bytes(uint8_t *b, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(b + got, n - got, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }
        got += (size_t)r;
    }
    for (; got < n; got++) b[got] = (uint8_t)rand(); /* never happens on Linux */
}

/* ---------- tokens ----------
 * DR's API wants two tokens: "UserAccount" (playback) and "UserProfile" (search, live, lists). Without an account
 * they come from anonymous-sso; with an account from the login (refresh token in <data>/apps/drtv/login). They
 * last about 12 hours and live in memory only. Fetched by the worker threads under tok_mu. */
static SDL_Mutex *tok_mu;
static char tok_account[4096], tok_profile[4096];
static time_t tok_exp;
static SDL_AtomicInt login_lost; /* a worker found the saved login rejected by DR */
static SDL_AtomicInt signout_a;  /* increased on sign-out: a login that finishes afterwards is not saved */
static char dev_id[40];

static bool tokens_parse(cJSON *arr) {
    if (!cJSON_IsArray(arr)) return false;
    char acc[4096] = "", prof[4096] = "";
    time_t exp = 0;
    cJSON *t;
    cJSON_ArrayForEach(t, arr) {
        const char *ty = jv(t, "type"), *val = jv(t, "value");
        if (!ty || !val) continue;
        if (!strcmp(ty, "UserAccount")) snprintf(acc, sizeof acc, "%s", val);
        else if (!strcmp(ty, "UserProfile")) snprintf(prof, sizeof prof, "%s", val);
        time_t e = iso_time(jv(t, "expirationDate"));
        if (e && (!exp || e < exp)) exp = e;
    }
    if (!acc[0]) return false;
    snprintf(tok_account, sizeof tok_account, "%s", acc);
    snprintf(tok_profile, sizeof tok_profile, "%s", prof[0] ? prof : acc);
    tok_exp = exp ? exp : time(NULL) + 3600;
    return true;
}

static bool tokens_anonymous(void) {
    char *body = str_printf("{\"deviceId\":\"%s\",\"scopes\":[\"Catalog\"],\"optout\":false}", dev_id);
    static const char *const h[] = {"Content-Type: application/json", "Accept: application/json", NULL};
    HttpBuf b;
    memset(&b, 0, sizeof b);
    bool ok = false;
    if (http_request("POST", DR_API "/authorization/anonymous-sso?device=web_browser&ff=idp,ldp,rpt&lang=da&supportFallbackToken=true",
                     h, body, &b, 15) && b.status == 200 && b.data) {
        cJSON *j = cJSON_Parse(b.data);
        ok = tokens_parse(j);
        cJSON_Delete(j);
    }
    if (!ok) tlog("drtv: anonymous token failed (%ld)", b.status);
    http_free(&b);
    free(body);
    return ok;
}

/* DR's login tokens (OIDC) -> the API's tokens */
static bool tokens_exchange(cJSON *oidc) {
    const char *at = jv(oidc, "access_token"), *it = jv(oidc, "id_token");
    if (!at || !it) return false;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "accessToken", at);
    cJSON_AddStringToObject(o, "identityToken", it);
    cJSON *sc = cJSON_AddArrayToObject(o, "scopes");
    cJSON_AddItemToArray(sc, cJSON_CreateString("Catalog"));
    cJSON_AddStringToObject(o, "device", "web_browser");
    cJSON_AddBoolToObject(o, "optout", false);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    static const char *const h[] = {"Content-Type: application/json", "Accept: application/json", NULL};
    HttpBuf b;
    memset(&b, 0, sizeof b);
    bool ok = false;
    if (http_request("POST", DR_API "/authorization/exchange", h, body, &b, 15) && b.status == 200 && b.data) {
        cJSON *j = cJSON_Parse(b.data);
        ok = tokens_parse(j);
        cJSON_Delete(j);
    }
    if (!ok) tlog("drtv: token exchange failed (%ld)", b.status);
    http_free(&b);
    free(body);
    return ok;
}

static cJSON *oidc_token(const char *form, long *status) {
    static const char *const h[] = {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json", NULL};
    HttpBuf b;
    memset(&b, 0, sizeof b);
    cJSON *j = NULL;
    if (http_request("POST", DR_LOGIN "/oidc/token", h, form, &b, 15)) {
        *status = b.status;
        if (b.status == 200 && b.data) j = cJSON_Parse(b.data);
    }
    http_free(&b);
    return j;
}

static char *login_read(void) {
    char *p = app_file("login"), *s = file_read_all(p, 16384);
    free(p);
    if (s) str_trim(s);
    if (s && !*s) { free(s); s = NULL; }
    return s;
}
static void login_write(const char *rt) {
    ensure_dir();
    char *p = app_file("login"), *tmp = str_printf("%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fchmod(fileno(f), 0600);
        fprintf(f, "%s\n", rt);
        fclose(f);
        rename(tmp, p);
    }
    free(tmp);
    free(p);
}

/* 1 = signed in again, 0 = no connection (try later), -1 = DR no longer accepts the login */
static int tokens_login_refresh(void) {
    char *rt = login_read();
    if (!rt) return -1;
    int sg = SDL_GetAtomicInt(&signout_a);
    char *enc = url_encode(rt), *form = str_printf("client_id=%s&refresh_token=%s&grant_type=refresh_token", DR_CLIENT_ID, enc);
    long st = 0;
    cJSON *t = oidc_token(form, &st);
    free(enc);
    free(form);
    free(rt);
    if (!t) return st >= 400 && st < 500 ? -1 : 0;
    const char *nrt = jv(t, "refresh_token");
    if (nrt && *nrt && sg == SDL_GetAtomicInt(&signout_a)) login_write(nrt); /* it may be replaced on every use */
    int r = tokens_exchange(t) ? 1 : 0;
    cJSON_Delete(t);
    return r;
}

/* kind 1 = UserAccount, 2 = UserProfile. Worker threads only (may make network calls). */
static bool tokens_get(int kind, char *out, size_t n) {
    SDL_LockMutex(tok_mu);
    if (!tok_account[0] || time(NULL) > tok_exp - 600) {
        tok_account[0] = tok_profile[0] = 0;
        char *saved = can_login() ? login_read() : NULL;
        if (saved) {
            free(saved);
            int r = tokens_login_refresh();
            if (r < 0) { SDL_SetAtomicInt(&login_lost, 1); tokens_anonymous(); }
            /* r == 0: no connection - do not quietly continue without the account */
        } else {
            tokens_anonymous();
        }
    }
    snprintf(out, n, "%s", kind == 2 ? tok_profile : tok_account);
    SDL_UnlockMutex(tok_mu);
    return out[0] != 0;
}
static void tokens_clear(void) {
    SDL_LockMutex(tok_mu);
    tok_account[0] = tok_profile[0] = 0;
    tok_exp = 0;
    SDL_UnlockMutex(tok_mu);
}

/* ---------- API requests (background) ---------- */
typedef struct DReq DReq;
struct DReq {
    char *url;
    int auth; /* 0 = none, 1 = UserAccount, 2 = UserProfile */
    cJSON *json;
    long status;
    int gen, signout;
    bool own_ud; /* ud is freed here when the reply is dropped (old view, sign-out) */
    void (*done)(void *ud, cJSON *json, long status);
    void *ud;
};
static int dr_gen;     /* increased when the view changes: replies for old views are dropped */
static int dr_signout; /* increased on sign-out: replies from before are dropped */

static void dreq_work(void *arg) {
    DReq *r = arg;
    for (int attempt = 0; attempt < 2; attempt++) {
        char tok[4200] = "", hdr[4300];
        const char *h[3] = {"Accept: application/json", NULL, NULL};
        if (r->auth && tokens_get(r->auth, tok, sizeof tok)) {
            snprintf(hdr, sizeof hdr, "X-Authorization: Bearer %s", tok);
            h[1] = hdr;
        }
        HttpBuf b;
        memset(&b, 0, sizeof b);
        cJSON_Delete(r->json);
        r->json = NULL;
        r->status = 0;
        if (http_request("GET", r->url, h, NULL, &b, 20)) {
            r->status = b.status;
            if (b.data) r->json = cJSON_Parse(b.data);
        }
        http_free(&b);
        if (r->status != 401 || !r->auth) break;
        tokens_clear(); /* the token was turned down: get a new one and try once more */
    }
}
static void dreq_done(void *arg) {
    DReq *r = arg;
    if (r->signout == dr_signout && r->gen == dr_gen && r->done) r->done(r->ud, r->json, r->status);
    else if (r->own_ud) free(r->ud);
    cJSON_Delete(r->json);
    free(r->url);
    free(r);
}
static void dr_get_ex(char *url, int auth, void (*done)(void *, cJSON *, long), void *ud, bool own_ud) { /* takes url */
    DReq *r = calloc(1, sizeof *r);
    r->own_ud = own_ud;
    r->url = url;
    r->auth = auth;
    r->done = done;
    r->ud = ud;
    r->gen = dr_gen;
    r->signout = dr_signout;
    job_submit(dreq_work, dreq_done, r);
}
static void dr_get(char *url, int auth, void (*done)(void *, cJSON *, long), void *ud) { dr_get_ex(url, auth, done, ud, false); }

/* waiting with a spinner for work in the background (playback addresses, login). The job and the waiting screen
   both hold the struct; whoever is last frees it. */
typedef struct Blk Blk;
struct Blk {
    SDL_AtomicInt fin;
    bool waiter_left, done_ran;
    void (*run)(Blk *b);
    void (*release)(Blk *b);
};
static void blk_work(void *a) { Blk *b = a; b->run(b); SDL_SetAtomicInt(&b->fin, 1); }
static void blk_done(void *a) { Blk *b = a; b->done_ran = true; if (b->waiter_left) b->release(b); }
static void blk_release(Blk *b) { b->waiter_left = true; if (b->done_ran) b->release(b); }
/* true = finished (read the result, then blk_release); false = Back was pressed (do not touch it again) */
static bool blk_wait(Blk *b, const char *msg) {
    job_submit(blk_work, blk_done, b);
    while (!SDL_GetAtomicInt(&b->fin))
        if (app_wait_frame(msg)) { blk_release(b); return false; }
    return true;
}

/* ---------- items ---------- */
enum { K_EPISODE, K_MOVIE, K_SEASON, K_SHOW, K_CHANNEL, K_LINK };
typedef struct {
    int kind;
    char *id, *path, *title, *sub, *img, *desc, *next; /* next: live - what comes after */
    int dur, num;
    double prog; /* 0..1 watched (live: how far the programme is) */
} DItem;

static void ditem_free(DItem *t) {
    free(t->id); free(t->path); free(t->title); free(t->sub); free(t->img); free(t->desc); free(t->next);
    memset(t, 0, sizeof *t);
}
static void ditem_copy(DItem *d, const DItem *s) {
    *d = *s;
    d->id = str_dup(s->id); d->path = str_dup(s->path); d->title = str_dup(s->title); d->sub = str_dup(s->sub);
    d->img = str_dup(s->img); d->desc = str_dup(s->desc); d->next = str_dup(s->next);
}

/* local resume positions: <data>/apps/drtv/progress.json {id: {pos, dur, t, title, sub, img}} (main thread) */
static cJSON *prog;
static void prog_load(void) {
    if (prog) return;
    char *p = app_file("progress.json"), *s = file_read_all(p, 1 << 20);
    free(p);
    prog = s ? cJSON_Parse(s) : NULL;
    free(s);
    if (!cJSON_IsObject(prog)) { cJSON_Delete(prog); prog = cJSON_CreateObject(); }
}
static void prog_save(void) {
    if (!prog) return;
    ensure_dir();
    char *p = app_file("progress.json"), *tmp = str_printf("%s.tmp", p), *s = cJSON_PrintUnformatted(prog);
    FILE *f = fopen(tmp, "w");
    if (f && s) { fputs(s, f); fclose(f); rename(tmp, p); }
    else if (f) fclose(f);
    free(s); free(tmp); free(p);
}
static double prog_pos(const char *id) {
    prog_load();
    cJSON *e = id ? cJSON_GetObjectItem(prog, id) : NULL;
    return e ? jnum(e, "pos") : 0;
}
static double prog_frac(const char *id) {
    prog_load();
    cJSON *e = id ? cJSON_GetObjectItem(prog, id) : NULL;
    double d = e ? jnum(e, "dur") : 0;
    return d > 0 ? jnum(e, "pos") / d : 0;
}
static void prog_set(const DItem *t, double pos, double dur) {
    if (!t->id) return;
    prog_load();
    cJSON_DeleteItemFromObject(prog, t->id);
    if (pos < 30 || (dur > 0 && pos > dur - 60)) return; /* barely started or watched to the end: no resume */
    cJSON *e = cJSON_AddObjectToObject(prog, t->id);
    cJSON_AddNumberToObject(e, "pos", (int)pos);
    cJSON_AddNumberToObject(e, "dur", (int)dur);
    cJSON_AddNumberToObject(e, "t", (double)time(NULL));
    if (t->title) cJSON_AddStringToObject(e, "title", t->title);
    if (t->sub) cJSON_AddStringToObject(e, "sub", t->sub);
    if (t->img) cJSON_AddStringToObject(e, "img", t->img);
    /* keep it small: the 40 newest */
    while (cJSON_GetArraySize(prog) > 40) {
        cJSON *old = NULL, *x;
        cJSON_ArrayForEach(x, prog) if (!old || jnum(x, "t") < jnum(old, "t")) old = x;
        cJSON_DeleteItemFromObject(prog, old->string);
    }
}

/* the saved positions, newest first (main thread; the array is malloc'ed, the items belong to prog) */
static int prog_sorted(cJSON ***out) {
    prog_load();
    int n = cJSON_GetArraySize(prog), k = 0;
    cJSON **e = calloc((size_t)n + 1, sizeof *e), *x;
    cJSON_ArrayForEach(x, prog) if (id_ok(x->string)) e[k++] = x;
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && jnum(e[j], "t") > jnum(e[j - 1], "t"); j--) { cJSON *t = e[j]; e[j] = e[j - 1]; e[j - 1] = t; }
    *out = e;
    return k;
}

static bool item_parse(cJSON *o, DItem *t, bool big) {
    memset(t, 0, sizeof *t);
    const char *ty = jv(o, "type");
    if (!ty) return false;
    if (!strcmp(ty, "episode")) t->kind = K_EPISODE;
    else if (!strcmp(ty, "movie")) t->kind = K_MOVIE;
    else if (!strcmp(ty, "season")) t->kind = K_SEASON;
    else if (!strcmp(ty, "show")) t->kind = K_SHOW;
    else if (!strcmp(ty, "channel")) t->kind = K_CHANNEL;
    else if (!strcmp(ty, "link")) t->kind = K_LINK;
    else return false;
    t->id = jid(o, "id");
    t->path = js(o, "path");
    t->title = js(o, "title");
    t->desc = js(o, "shortDescription");
    if (!t->desc) t->desc = js(o, "description");
    t->dur = (int)jnum(o, "duration");
    t->num = (int)jnum(o, "episodeNumber");
    if (t->kind == K_LINK && t->path && !strncmp(t->path, "/kanal/", 7)) { /* a link to a live channel */
        t->kind = K_CHANNEL;
        free(t->id);
        t->id = str_dup(t->path + 7);
    }
    cJSON *im = cJSON_GetObjectItem(o, "images");
    const char *u = jv(im, "tile");
    if (!u || !*u) u = jv(im, "wallpaper");
    t->img = img_sized(u, big ? 960 : 640, big ? 540 : 360);
    cJSON *cf = cJSON_GetObjectItem(o, "customFields");
    const char *chan = jv(cf, "BrandingChannelShort"), *extra = jv(cf, "ExtraDetails");
    char sub[200] = "";
    if (t->kind == K_EPISODE || t->kind == K_MOVIE) {
        if (chan && *chan && t->dur > 0) snprintf(sub, sizeof sub, T("%s · %d min"), chan, (t->dur + 59) / 60);
        else if (t->dur > 0) snprintf(sub, sizeof sub, T("%d min"), (t->dur + 59) / 60);
        else if (chan) snprintf(sub, sizeof sub, "%s", chan);
    } else if (t->kind == K_SEASON || t->kind == K_SHOW) {
        if (extra && *extra) snprintf(sub, sizeof sub, "%s", extra);
        else if (chan) snprintf(sub, sizeof sub, "%s", chan);
    } else if (t->kind == K_CHANNEL) {
        snprintf(sub, sizeof sub, "%s", T("Live"));
    }
    t->sub = sub[0] ? str_dup(sub) : NULL;
    if (t->kind == K_EPISODE || t->kind == K_MOVIE) t->prog = prog_frac(t->id);
    if ((t->kind == K_EPISODE || t->kind == K_MOVIE) && !id_ok(t->id)) { ditem_free(t); return false; }
    if (!t->path && t->kind != K_CHANNEL && t->kind != K_EPISODE && t->kind != K_MOVIE) { ditem_free(t); return false; }
    return true;
}

/* ---------- rows ---------- */
typedef struct {
    char *title;
    char *list_id; /* the items are fetched when the row comes near (DR only sends the first lists with items) */
    DItem *it;
    int n, cap, off;
    bool big, loading, episodes;
} Row;

enum { V_HOME, V_LIVE, V_BROWSE, V_DETAIL, V_SEARCH, V_MYLIST };
typedef struct {
    int view;
    char path[512], heading[200];
    int row, col;
} BackStep;

typedef struct {
    int view;
    char heading[200], path[512], msg[240];
    bool loading;
    Row rows[MAX_ROWS];
    int nrows, row, col, want_row, want_col;
    float scroll;
    /* detail page */
    bool d_focus; /* focus on the buttons */
    int d_btn, d_nbtn;
    char *d_title, *d_desc, *d_meta, *d_img;
    DItem d_play;  /* what Play starts */
    bool d_has_play;
    SideMenu side;
    int side_map[16];
    BackStep back[10];
    int nback;
    /* live */
    double live_loaded;
} Dr;

static Dr *live;
static void home_clear(void);
static void build_side(Dr *v);
static void load_view(Dr *v);

static void row_free(Row *r) {
    for (int i = 0; i < r->n; i++) ditem_free(&r->it[i]);
    free(r->it);
    free(r->title);
    free(r->list_id);
    memset(r, 0, sizeof *r);
}
static void detail_clear(Dr *v) {
    free(v->d_title); free(v->d_desc); free(v->d_meta); free(v->d_img);
    v->d_title = v->d_desc = v->d_meta = v->d_img = NULL;
    ditem_free(&v->d_play);
    v->d_has_play = false;
    v->d_focus = false;
    v->d_btn = v->d_nbtn = 0;
}
static void rows_clear(Dr *v) {
    for (int i = 0; i < v->nrows; i++) row_free(&v->rows[i]);
    v->nrows = 0;
    v->row = v->col = 0;
    v->scroll = 0;
    detail_clear(v);
}
static Row *row_add(Dr *v, const char *title) {
    if (v->nrows >= MAX_ROWS) return NULL;
    Row *r = &v->rows[v->nrows++];
    memset(r, 0, sizeof *r);
    r->title = str_dup(title ? title : "");
    return r;
}
static void row_remove(Dr *v, int i) {
    row_free(&v->rows[i]);
    memmove(&v->rows[i], &v->rows[i + 1], sizeof(Row) * (v->nrows - i - 1));
    v->nrows--;
    if (v->row > i || v->row >= v->nrows) { v->row = v->row > 0 ? v->row - 1 : 0; v->col = 0; }
    else if (v->row == i) v->col = 0; /* the focused row went: the next one takes its place */
}
static DItem *row_push(Row *r) {
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 24; r->it = realloc(r->it, sizeof(DItem) * r->cap); }
    DItem *t = &r->it[r->n++];
    memset(t, 0, sizeof *t);
    return t;
}
static void row_items(Row *r, cJSON *items) {
    cJSON *o;
    cJSON_ArrayForEach(o, items) {
        DItem t;
        if (item_parse(o, &t, r->big)) *row_push(r) = t;
    }
}
static bool row_empty(const Row *r) { return r->n == 0 && !r->list_id; }

/* a ListEntry from a page: its items now, or the list id to fetch them later */
static void page_entries(Dr *v, cJSON *page) {
    cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(page, "entries")) {
        const char *ty = jv(e, "type");
        if (!ty || strcmp(ty, "ListEntry")) continue; /* Image/Text/Custom entries are web layout */
        cJSON *l = cJSON_GetObjectItem(e, "list");
        if (!l) continue;
        const char *title = jv(e, "title");
        if (!title || !*title) title = jv(l, "title");
        if (title && !strncmp(title, "Top text links", 14)) continue; /* web shortcuts */
        bool hero = title && strstr(title, "Hero");
        Row *r = row_add(v, hero ? T("Recommended") : title);
        if (!r) break;
        r->big = hero;
        row_items(r, cJSON_GetObjectItem(l, "items"));
        if (!r->n) r->list_id = jid(l, "id");
        if (row_empty(r)) row_remove(v, v->nrows - 1);
    }
}

/* lazy rows: fetch the next few as they come near the focus */
typedef struct { Dr *v; char list_id[24]; } LazyCtx;
static void lazy_done(void *ud, cJSON *json, long status) {
    LazyCtx *c = ud;
    Dr *v = c->v;
    char id[24];
    snprintf(id, sizeof id, "%s", c->list_id);
    free(c);
    if (v != live) return;
    for (int i = 0; i < v->nrows; i++) {
        Row *r = &v->rows[i];
        if (!r->list_id || strcmp(r->list_id, id)) continue;
        r->loading = false;
        free(r->list_id);
        r->list_id = NULL;
        if (status == 200) row_items(r, cJSON_GetObjectItem(json, "items"));
        if (!r->n) row_remove(v, i);
        break;
    }
}
static void lazy_tick(Dr *v) {
    int busy = 0;
    for (int i = 0; i < v->nrows; i++) if (v->rows[i].loading) busy++;
    for (int i = 0; i < v->nrows && busy < 3; i++) {
        Row *r = &v->rows[i];
        if (!r->list_id || r->loading || i > v->row + 4) continue;
        if (!id_ok(r->list_id)) { free(r->list_id); r->list_id = NULL; continue; }
        r->loading = true;
        busy++;
        LazyCtx *c = calloc(1, sizeof *c);
        c->v = v;
        snprintf(c->list_id, sizeof c->list_id, "%s", r->list_id);
        dr_get_ex(str_printf(DR_API "/lists/%s?page_size=24", r->list_id), 0, lazy_done, c, true);
    }
}

/* ---------- views ---------- */
static void continue_row(Dr *v) { /* local resume positions, newest first */
    cJSON **e;
    int k = prog_sorted(&e);
    if (k) {
        /* the row goes first */
        if (v->nrows < MAX_ROWS) {
            memmove(&v->rows[1], &v->rows[0], sizeof(Row) * v->nrows);
            v->nrows++;
            Row *r = &v->rows[0];
            memset(r, 0, sizeof *r);
            r->title = str_dup(T("Continue watching"));
            for (int i = 0; i < k && i < 20; i++) {
                DItem *t = row_push(r);
                t->kind = K_EPISODE;
                t->id = str_dup(e[i]->string);
                t->title = js(e[i], "title");
                t->sub = js(e[i], "sub");
                t->img = js(e[i], "img");
                double d = jnum(e[i], "dur");
                t->prog = d > 0 ? jnum(e[i], "pos") / d : 0;
                t->dur = (int)d;
            }
        }
    }
    free(e);
}

static void page_done(void *ud, cJSON *json, long status) {
    Dr *v = ud;
    if (v != live) return;
    v->loading = false;
    if (status != 200 || !json) {
        snprintf(v->msg, sizeof v->msg, status ? T("DR responded with error %ld") : T("Cannot reach DR - check the network"), status);
        return;
    }
    v->msg[0] = 0;
    page_entries(v, json);
    if (v->view == V_HOME && !signed_in()) continue_row(v);
    if (!v->nrows) snprintf(v->msg, sizeof v->msg, "%s", T("Nothing here right now"));
    if (v->want_row >= 0 && v->want_row < v->nrows) { v->row = v->want_row; v->col = v->want_col; }
    v->want_row = -1;
}

/* DR's own lists when signed in (Continue watching, My list) */
typedef struct { Dr *v; bool mylist; } AccCtx;
static void account_done(void *ud, cJSON *json, long status) {
    AccCtx *c = ud;
    Dr *v = c->v;
    bool mylist = c->mylist;
    free(c);
    if (v != live) return;
    if (v->view == V_MYLIST) v->loading = false;
    if (status != 200 || !json) {
        if (v->view == V_MYLIST) snprintf(v->msg, sizeof v->msg, T("Could not load your list from DR (%ld)"), status);
        return;
    }
    cJSON *items = cJSON_GetObjectItem(json, "items");
    if (!cJSON_IsArray(items) && cJSON_IsArray(json)) items = json;
    if (!cJSON_GetArraySize(items)) {
        if (v->view == V_MYLIST) snprintf(v->msg, sizeof v->msg, "%s", T("Your list is empty - add programmes on dr.dk or in the DRTV app"));
        return;
    }
    if (v->nrows >= MAX_ROWS) return;
    int at = mylist && v->view == V_HOME && v->nrows && !strcmp(v->rows[0].title, T("Continue watching")) ? 1 : 0;
    memmove(&v->rows[at + 1], &v->rows[at], sizeof(Row) * (v->nrows - at));
    v->nrows++;
    Row *r = &v->rows[at];
    memset(r, 0, sizeof *r);
    r->title = str_dup(mylist ? T("My list") : T("Continue watching"));
    row_items(r, items);
    if (!r->n) { row_remove(v, at); return; }
    /* keep the focus on what it was on - unless it has not moved yet (then the new row gets it) */
    if (v->nrows > 1 && (v->row > 0 || v->col > 0) && v->row >= at) v->row++;
}
static void account_rows(Dr *v, bool cont, bool mylist) {
    if (mylist) {
        AccCtx *c = calloc(1, sizeof *c);
        c->v = v;
        c->mylist = true;
        dr_get_ex(str_dup(DR_API "/account/profile/bookmarks/list?page_size=48&lang=da"), 2, account_done, c, true);
    }
    if (cont) {
        AccCtx *c = calloc(1, sizeof *c);
        c->v = v;
        dr_get_ex(str_dup(DR_API "/account/profile/continue-watching/list?page_size=24&lang=da"), 2, account_done, c, true);
    }
}

static void load_page(Dr *v, const char *path) {
    char *p = url_encode(path);
    const char *seg = v->view == V_HOME ? "&segments=drtv,optedin" : "";
    dr_get(str_printf(DR_API "/page?path=%s&item_detail_expand=all&list_page_size=24&max_list_prefetch=3%s", p, seg), 0,
           page_done, v);
    free(p);
}

/* live: the five channels with what is on now and next */
static const struct { const char *id, *name; } CHANNELS[] = {
    {"20875", "DR1"}, {"20876", "DR2"}, {"20892", "DR Ramasjang"}, {"192100", "DRTV Ekstra"}, {"192099", "TVA Live"},
};
#define N_CHANNELS ((int)(sizeof CHANNELS / sizeof CHANNELS[0]))

static void live_done(void *ud, cJSON *json, long status) {
    Dr *v = ud;
    if (v != live) return;
    v->loading = false;
    v->live_loaded = app_time();
    int keep_col = v->nrows ? v->col : 0;
    for (int i = 0; i < v->nrows; i++) row_free(&v->rows[i]);
    v->nrows = 0;
    Row *r = row_add(v, T("Live now"));
    r->big = true;
    time_t now = time(NULL);
    for (int c = 0; c < N_CHANNELS; c++) {
        DItem *t = row_push(r);
        t->kind = K_CHANNEL;
        t->id = str_dup(CHANNELS[c].id);
        t->title = str_dup(CHANNELS[c].name);
        cJSON *ch;
        cJSON_ArrayForEach(ch, json) {
            char *cid = jid(ch, "channelId");
            bool match = cid && !strcmp(cid, CHANNELS[c].id);
            free(cid);
            if (!match) continue;
            cJSON *s;
            bool got_now = false;
            cJSON_ArrayForEach(s, cJSON_GetObjectItem(ch, "schedules")) {
                time_t a = iso_time(jv(s, "startDate")), b = iso_time(jv(s, "endDate"));
                cJSON *it = cJSON_GetObjectItem(s, "item");
                const char *title = jv(it, "title");
                char hm[16];
                struct tm lt;
                localtime_r(&a, &lt);
                strftime(hm, sizeof hm, "%H:%M", &lt);
                if (!got_now && a <= now && now < b) {
                    got_now = true;
                    free(t->sub);
                    t->sub = str_printf(T("Now: %s"), title ? title : "");
                    t->prog = b > a ? (double)(now - a) / (double)(b - a) : 0;
                    const char *u = jv(cJSON_GetObjectItem(it, "images"), "tile");
                    if (!u) u = jv(cJSON_GetObjectItem(it, "images"), "wallpaper");
                    free(t->img);
                    t->img = img_sized(u, 960, 540);
                    t->desc = js(it, "shortDescription");
                } else if (got_now && a >= now && !t->next) {
                    t->next = str_printf(T("Next %s: %s"), hm, title ? title : "");
                }
            }
        }
        if (!t->sub) t->sub = str_dup(T("Live"));
    }
    if (status != 200) snprintf(v->msg, sizeof v->msg, "%s", T("The programme guide could not be loaded"));
    v->col = keep_col < r->n ? keep_col : 0;
}
static void load_live(Dr *v) {
    time_t now = time(NULL);
    struct tm g;
    gmtime_r(&now, &g);
    char date[16];
    strftime(date, sizeof date, "%Y-%m-%d", &g);
    char ch[200] = "";
    for (int c = 0; c < N_CHANNELS; c++) { size_t l = strlen(ch); snprintf(ch + l, sizeof ch - l, "&channels=%s", CHANNELS[c].id); }
    /* from an hour back, so the programme that started before this hour is there too */
    int hour = g.tm_hour > 0 ? g.tm_hour - 1 : 0;
    dr_get(str_printf(DR_API "/schedules?date=%s&hour=%d&duration=7%s", date, hour, ch), 0, live_done, v);
}

/* detail page of a series (season), film or episode */
static void detail_buttons(Dr *v) {
    v->d_nbtn = 0;
    if (!v->d_has_play) return;
    v->d_nbtn = prog_pos(v->d_play.id) > 30 ? 2 : 1; /* Resume + Start over, or Play */
}
static void detail_done(void *ud, cJSON *json, long status) {
    Dr *v = ud;
    if (v != live) return;
    v->loading = false;
    if (status != 200 || !json) {
        snprintf(v->msg, sizeof v->msg, status ? T("DR responded with error %ld") : T("Cannot reach DR - check the network"), status);
        return;
    }
    cJSON *e = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "entries"), 0);
    cJSON *it = cJSON_GetObjectItem(e, "item");
    const char *ty = jv(it, "type");
    if (!it || !ty) { snprintf(v->msg, sizeof v->msg, "%s", T("Nothing here right now")); return; }
    cJSON *show = cJSON_GetObjectItem(it, "show");
    v->d_title = js(show, "title");
    if (!v->d_title) v->d_title = js(it, "title");
    v->d_desc = js(it, "description");
    if (!v->d_desc) v->d_desc = js(show, "description");
    cJSON *im = cJSON_GetObjectItem(it, "images");
    const char *u = jv(im, "wallpaper");
    if (!u) u = jv(im, "tile");
    v->d_img = img_sized(u, 1280, 720);
    char meta[240] = "";
    int year = (int)jnum(it, "releaseYear"), neps = (int)jnum(it, "availableEpisodeCount");
    const char *extra = jv(cJSON_GetObjectItem(it, "customFields"), "ExtraDetails");
    if (year > 0) snprintf(meta, sizeof meta, "%d", year);
    if (!strcmp(ty, "season") && neps > 0) {
        char ep[64];
        if (neps == 1) snprintf(ep, sizeof ep, "%s", T("1 episode"));
        else snprintf(ep, sizeof ep, T("%d episodes"), neps);
        size_t l = strlen(meta);
        snprintf(meta + l, sizeof meta - l, "%s%s", l ? "  ·  " : "", ep);
    }
    if (extra && *extra) { size_t l = strlen(meta); snprintf(meta + l, sizeof meta - l, "%s%s", l ? "  ·  " : "", extra); }
    int dur = (int)jnum(it, "duration");
    if (dur > 0 && strcmp(ty, "season")) { size_t l = strlen(meta); snprintf(meta + l, sizeof meta - l, T("%s%d min"), l ? "  ·  " : "", (dur + 59) / 60); }
    v->d_meta = str_dup(meta);

    if (!strcmp(ty, "season") || !strcmp(ty, "show")) {
        Row *r = row_add(v, T("Episodes"));
        if (r) {
            r->episodes = true;
            row_items(r, cJSON_GetObjectItem(cJSON_GetObjectItem(it, "episodes"), "items"));
            for (int i = 0; i < r->n; i++) { /* "Matador: Den rejsende" -> "1. Den rejsende" */
                DItem *t = &r->it[i];
                const char *c = t->title ? strstr(t->title, ": ") : NULL;
                char *nt = c ? str_printf("%d. %s", t->num > 0 ? t->num : i + 1, c + 2)
                             : str_printf(T("Episode %d"), t->num > 0 ? t->num : i + 1); /* only the series' name */
                if (nt) { free(t->title); t->title = nt; }
            }
            /* Play: the first episode that is started and not finished, otherwise the first one */
            int pick = 0;
            for (int i = 0; i < r->n; i++) if (prog_pos(r->it[i].id) > 30) { pick = i; break; }
            if (r->n) { ditem_copy(&v->d_play, &r->it[pick]); v->d_has_play = true; }
            if (!r->n) row_remove(v, v->nrows - 1);
        }
        cJSON *seasons = cJSON_GetObjectItem(cJSON_GetObjectItem(show, "seasons"), "items");
        if (cJSON_GetArraySize(seasons) > 1) {
            Row *s = row_add(v, T("Seasons"));
            if (s) {
                cJSON *o;
                cJSON_ArrayForEach(o, seasons) {
                    DItem t;
                    if (!item_parse(o, &t, false)) continue;
                    int sn = (int)jnum(o, "seasonNumber");
                    free(t.sub);
                    t.sub = sn > 0 ? str_printf(T("Season %d"), sn) : NULL;
                    char *cur = jid(it, "id");
                    if (cur && t.id && !strcmp(cur, t.id)) { free(t.sub); t.sub = str_dup(T("You are here")); }
                    free(cur);
                    *row_push(s) = t;
                }
                if (!s->n) row_remove(v, v->nrows - 1);
            }
        }
    } else if (!strcmp(ty, "movie") || !strcmp(ty, "episode")) {
        DItem t;
        if (item_parse(it, &t, false)) { v->d_play = t; v->d_has_play = true; }
    }
    detail_buttons(v);
    v->d_focus = v->d_nbtn > 0;
    if (!v->d_nbtn && !v->nrows) snprintf(v->msg, sizeof v->msg, "%s", T("This cannot be played right now"));
    if (v->want_row >= 0 && v->want_row < v->nrows) { v->row = v->want_row; v->col = v->want_col; v->d_focus = false; }
    v->want_row = -1;
}

/* search: series, films and episodes */
static void search_done_http(void *ud, cJSON *json, long status) {
    Dr *v = ud;
    if (v != live) return;
    v->loading = false;
    if (status != 200 || !json) {
        snprintf(v->msg, sizeof v->msg, status ? T("DR responded with error %ld") : T("Cannot reach DR - check the network"), status);
        return;
    }
    static const struct { const char *key, *title; } groups[] = {{"series", N_("Series")}, {"movies", N_("Films")}, {"playable", N_("Episodes")}};
    for (int g = 0; g < 3; g++) {
        Row *r = row_add(v, T(groups[g].title));
        if (!r) break;
        row_items(r, cJSON_GetObjectItem(cJSON_GetObjectItem(json, groups[g].key), "items"));
        if (!r->n) row_remove(v, v->nrows - 1);
    }
    if (!v->nrows) snprintf(v->msg, sizeof v->msg, "%s", T("Nothing found"));
}

static void load_view(Dr *v) {
    dr_gen++;
    rows_clear(v);
    v->msg[0] = 0;
    v->loading = true;
    switch (v->view) {
    case V_HOME:
        load_page(v, "/");
        if (signed_in()) account_rows(v, true, false);
        break;
    case V_LIVE: load_live(v); break;
    case V_BROWSE: load_page(v, v->path); break;
    case V_DETAIL: {
        char *p = url_encode(v->path);
        dr_get(str_printf(DR_API "/page?path=%s&item_detail_expand=all&list_page_size=24&max_list_prefetch=3", p), 0, detail_done, v);
        free(p);
        break;
    }
    case V_SEARCH: {
        char *q = url_encode(v->path);
        dr_get(str_printf(DR_API "/search?term=%s&group=true&list_page_size=24&item_detail_expand=all", q), 2, search_done_http, v);
        free(q);
        break;
    }
    case V_MYLIST: account_rows(v, false, true); break;
    }
}

static void go(Dr *v, int view, const char *path, const char *heading, bool push) {
    if (push && v->nback < (int)(sizeof v->back / sizeof v->back[0])) {
        BackStep *b = &v->back[v->nback++];
        b->view = v->view;
        snprintf(b->path, sizeof b->path, "%s", v->path);
        snprintf(b->heading, sizeof b->heading, "%s", v->heading);
        b->row = v->row;
        b->col = v->col;
    } else if (!push) {
        v->nback = 0;
    }
    v->view = view;
    snprintf(v->path, sizeof v->path, "%s", path ? path : "");
    snprintf(v->heading, sizeof v->heading, "%s", heading ? heading : "");
    v->want_row = -1;
    load_view(v);
}
static bool go_back(Dr *v) {
    if (!v->nback) return false;
    BackStep *b = &v->back[--v->nback];
    v->view = b->view;
    snprintf(v->path, sizeof v->path, "%s", b->path);
    snprintf(v->heading, sizeof v->heading, "%s", b->heading);
    load_view(v);
    v->want_row = b->row;
    v->want_col = b->col;
    return true;
}

/* ---------- playback ---------- */
typedef struct {
    Blk b;
    char id[24];
    bool registered;
    char *url;
    PlaySkip skip;
    long status;
} VodJob;
static void vod_run(Blk *bb) {
    VodJob *j = (VodJob *)bb;
    char tok[4200], hdr[4300];
    if (!tokens_get(1, tok, sizeof tok)) return;
    snprintf(hdr, sizeof hdr, "X-Authorization: Bearer %s", tok);
    const char *h[] = {"Accept: application/json", hdr, NULL};
    for (int attempt = 0; attempt < 2 && !j->url; attempt++) {
        char *url = str_printf(DR_API "/account/items/%s/videos?delivery=stream&device=web_browser&ff=idp,ldp,rpt&lang=da&resolution=HD-1080%s",
                               j->id, attempt ? "" : j->registered ? "&sub=Registered" : "&sub=Anonymous");
        HttpBuf b;
        memset(&b, 0, sizeof b);
        if (http_request("GET", url, h, NULL, &b, 20)) {
            j->status = b.status;
            cJSON *arr = b.status == 200 && b.data ? cJSON_Parse(b.data) : NULL;
            cJSON *s, *pick = NULL;
            cJSON_ArrayForEach(s, arr) {
                const char *as = jv(s, "accessService"), *fmt = jv(s, "format");
                if (!jv(s, "url") || (fmt && !strstr(fmt, "hls"))) continue;
                if (as && !strcmp(as, "StandardVideo")) { pick = s; break; }
                if (!pick) pick = s;
            }
            if (pick) {
                j->url = js(pick, "url");
                cJSON *tc;
                cJSON_ArrayForEach(tc, cJSON_GetObjectItem(pick, "timeCodes")) {
                    const char *ty = jv(tc, "timeCodeType");
                    double a = jnum(tc, "startTime"), e = jnum(tc, "endTime");
                    if (!ty || e <= a || j->skip.n >= 6) continue;
                    bool cred = !strcmp(ty, "EndCredit") || !strcmp(ty, "EndCredits");
                    if (!cred && strcmp(ty, "Intro") && strcmp(ty, "Recap")) continue;
                    j->skip.m[j->skip.n++] = (PlayMarker){a, e, cred};
                }
            }
            cJSON_Delete(arr);
        }
        http_free(&b);
        free(url);
    }
}
static void vod_release(Blk *bb) { VodJob *j = (VodJob *)bb; free(j->url); free(j); }

typedef struct {
    const DItem *cur;
    PlaySkip skip;
    const DItem *next;
} PlayCtx;
static void play_skip_info(void *ud, PlaySkip *out) {
    PlayCtx *c = ud;
    *out = c->skip;
    if (c->next) {
        out->has_next = true;
        snprintf(out->next_title, sizeof out->next_title, "%s", c->next->title ? c->next->title : "");
    }
}
static void play_progress(void *ud, double pos, double dur, bool ended, bool paused) {
    PlayCtx *c = ud;
    if (pos > 0) prog_set(c->cur, ended ? dur : pos, dur);
}

/* the episode after t on the detail page (same series) */
static const DItem *next_after(Dr *v, const DItem *t) {
    if (!v || v->view != V_DETAIL || !t->id) return NULL;
    for (int i = 0; i < v->nrows; i++) {
        Row *r = &v->rows[i];
        if (!r->episodes) continue;
        for (int k = 0; k + 1 < r->n; k++)
            if (r->it[k].id && !strcmp(r->it[k].id, t->id)) return &r->it[k + 1];
    }
    return NULL;
}

/* returns true when the video was played to the end */
static bool play_vod(Dr *v, const DItem *t, bool from_start) {
    if (!id_ok(t->id)) { app_toast("%s", T("This cannot be played right now")); return false; }
    VodJob *j = calloc(1, sizeof *j);
    j->b.run = vod_run;
    j->b.release = vod_release;
    snprintf(j->id, sizeof j->id, "%s", t->id);
    j->registered = signed_in();
    if (!blk_wait(&j->b, T("Starting DRTV…"))) return false;
    if (!j->url) {
        if (j->status == 403 || j->status == 451) app_toast("%s", T("DR only shows this in Denmark"));
        else if (j->status) app_toast(T("DR responded with error %ld"), j->status);
        else app_toast("%s", T("Cannot reach DR - check the network"));
        blk_release(&j->b);
        return false;
    }
    PlayCtx pc = {t, j->skip, next_after(v, t)};
    PlayRequest rq = {0};
    rq.url = j->url;
    rq.title = t->title ? t->title : "DRTV";
    rq.subtitle = t->sub;
    rq.description = t->desc;
    rq.poster = t->img;
    rq.start = from_start ? 0 : prog_pos(t->id);
    rq.progress = play_progress;
    rq.skip_info = play_skip_info;
    rq.ud = &pc;
    PlayResult res = player_run(&rq);
    blk_release(&j->b);
    if (!res.cancelled && res.pos > 0) prog_set(t, res.eof ? res.dur : res.pos, res.dur);
    prog_save();
    return !res.cancelled && res.eof && res.dur > 0;
}

static void play_item(Dr *v, const DItem *start, bool from_start) {
    DItem cur;
    ditem_copy(&cur, start);
    for (int guard = 0; guard < 100; guard++) {
        bool ended = play_vod(v, &cur, from_start);
        const DItem *n = ended && v == live ? next_after(v, &cur) : NULL;
        ditem_free(&cur);
        if (!n) break;
        ditem_copy(&cur, n); /* the next episode */
        from_start = true;
    }
    ditem_free(&cur);
    if (v != live) return;
    /* show the new positions */
    for (int i = 0; i < v->nrows; i++)
        for (int k = 0; k < v->rows[i].n; k++) {
            DItem *t = &v->rows[i].it[k];
            if (t->kind == K_EPISODE || t->kind == K_MOVIE) t->prog = prog_frac(t->id);
        }
    if (v->view == V_DETAIL && v->d_has_play) {
        /* Play moves on to the episode you stopped in */
        for (int i = 0; i < v->nrows; i++) {
            Row *r = &v->rows[i];
            if (!r->episodes) continue;
            for (int k = 0; k < r->n; k++)
                if (prog_pos(r->it[k].id) > 30) { ditem_free(&v->d_play); ditem_copy(&v->d_play, &r->it[k]); break; }
        }
        detail_buttons(v);
        if (v->d_btn >= v->d_nbtn) v->d_btn = 0;
    }
}

/* live: all channels as one playlist, so up/down changes the channel */
typedef struct {
    Blk b;
    char *url[8];
} LiveJob;
static void live_run(Blk *bb) {
    LiveJob *j = (LiveJob *)bb;
    char tok[4200], hdr[4300];
    if (!tokens_get(2, tok, sizeof tok)) return;
    snprintf(hdr, sizeof hdr, "X-Authorization: Bearer %s", tok);
    const char *h[] = {"Accept: application/json", hdr, NULL};
    for (int c = 0; c < N_CHANNELS; c++) {
        char *url = str_printf(DR_API "/channels/%s/liveStreams", CHANNELS[c].id);
        HttpBuf b;
        memset(&b, 0, sizeof b);
        if (http_request("GET", url, h, NULL, &b, 15) && b.status == 200 && b.data) {
            cJSON *arr = cJSON_Parse(b.data), *s;
            const char *want[] = {"hlsURL", "hlsURLEu", "hlsAlternativeURL"};
            for (int w = 0; w < 3 && !j->url[c]; w++)
                cJSON_ArrayForEach(s, arr) {
                    const char *ty = jv(s, "type");
                    if (ty && !strcmp(ty, want[w]) && jv(s, "link")) { j->url[c] = js(s, "link"); break; }
                }
            cJSON_Delete(arr);
        }
        http_free(&b);
        free(url);
    }
}
static void live_release(Blk *bb) {
    LiveJob *j = (LiveJob *)bb;
    for (int c = 0; c < 8; c++) free(j->url[c]);
    free(j);
}
static void play_live(Dr *v, int idx) {
    LiveJob *j = calloc(1, sizeof *j);
    j->b.run = live_run;
    j->b.release = live_release;
    if (!blk_wait(&j->b, T("Starting DRTV…"))) return;
    /* only the channels that answered, in the usual order */
    int map[8], n = 0, start = 0;
    for (int c = 0; c < N_CHANNELS; c++) {
        if (!j->url[c] || strchr(j->url[c], '\n')) continue;
        if (c == idx) start = n;
        map[n++] = c;
    }
    if (!n || !j->url[idx]) {
        app_toast("%s", T("The channel could not be started"));
        blk_release(&j->b);
        return;
    }
    FILE *f = fopen(LIVE_M3U, "w");
    if (!f) { blk_release(&j->b); return; }
    fprintf(f, "#EXTM3U\n");
    for (int i = 0; i < n; i++) fprintf(f, "#EXTINF:-1,%d %s\n%s\n", i + 1, CHANNELS[map[i]].name, j->url[map[i]]);
    fclose(f);
    blk_release(&j->b);
    PlayRequest rq = {0};
    rq.url = LIVE_M3U;
    rq.subtitle = "DRTV";
    rq.is_live = true;
    rq.playlist_start = start;
    rq.playlist_n = n;
    static const char *const extra[] = {"--loop-playlist=inf", "--prefetch-playlist=no", NULL};
    rq.extra = extra;
    player_run(&rq);
    unlink(LIVE_M3U);
    if (v == live) load_view(v); /* what is on may have changed */
}

/* ---------- login (DR's own login: e-mail + password) ---------- */
typedef struct { char *d; size_t n; } CBuf;
static size_t cbuf_w(char *p, size_t s, size_t n, void *u) {
    CBuf *b = u;
    size_t l = s * n;
    if (b->n + l > (2u << 20)) return 0;
    char *d = realloc(b->d, b->n + l + 1);
    if (!d) return 0;
    memcpy(d + b->n, p, l);
    b->n += l;
    d[b->n] = 0;
    b->d = d;
    return l;
}
/* a request in the login session (cookies are kept in the handle). follow = false: *redir gets the Location */
static long lreq(CURL *c, const char *url, const char *ctype, const char *body, bool follow, CBuf *out, char **eff, char **redir) {
    curl_easy_reset(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, ""); /* cookies in memory only */
    curl_easy_setopt(c, CURLOPT_USERAGENT, DR_UA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    struct curl_slist *h = curl_slist_append(NULL, "Accept: application/json, text/html;q=0.9, */*;q=0.8");
    if (body) {
        char ct[96];
        snprintf(ct, sizeof ct, "Content-Type: %s", ctype);
        h = curl_slist_append(h, ct);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cbuf_w);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    long st = 0;
    if (curl_easy_perform(c) == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
        char *u = NULL;
        if (eff && curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &u) == CURLE_OK && u) *eff = str_dup(u);
        u = NULL;
        if (redir && curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &u) == CURLE_OK && u) *redir = str_dup(u);
    }
    curl_slist_free_all(h);
    return st;
}
static cJSON *graphql(CURL *c, const char *op, const char *query, cJSON *vars) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "operationName", op);
    cJSON_AddItemToObject(o, "variables", vars);
    cJSON_AddStringToObject(o, "query", query);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    CBuf b = {0};
    long st = lreq(c, DR_LOGIN "/graphql", "application/json", body, true, &b, NULL, NULL);
    /* the body has the password: overwrite it before it is freed */
    if (body) { memset(body, 0, strlen(body)); free(body); }
    cJSON *j = st == 200 && b.d ? cJSON_Parse(b.d) : NULL;
    free(b.d);
    return j;
}
static cJSON *gql_vars(const char *trans, const char *key, const char *val) {
    cJSON *v = cJSON_CreateObject();
    if (!key) { cJSON_AddStringToObject(v, "id", trans); return v; }
    cJSON *in = cJSON_AddObjectToObject(v, "input");
    cJSON_AddStringToObject(in, "transaction", trans);
    cJSON_AddStringToObject(in, key, val);
    return v;
}
static char *query_param(const char *url, const char *key) {
    const char *q = url ? strchr(url, '?') : NULL;
    size_t kl = strlen(key);
    for (const char *p = q; p && *p; p = strchr(p + 1, '&')) {
        if (!strncmp(p + 1, key, kl) && p[1 + kl] == '=') {
            const char *v = p + 2 + kl;
            size_t l = strcspn(v, "&#");
            return str_printf("%.*s", (int)l, v);
        }
    }
    return NULL;
}

typedef struct {
    Blk b;
    char email[256], pw[256];
    char err[240];
    bool ok;
    int signout;
} LoginJob;
static void login_fail(LoginJob *j, const char *msg) { if (!j->err[0]) snprintf(j->err, sizeof j->err, "%s", msg); }
static void login_run(Blk *bb) {
    LoginJob *j = (LoginJob *)bb;
    CURL *c = curl_easy_init();
    if (!c) { login_fail(j, T("Cannot reach DR - check the network")); return; }
    uint8_t rnd[48], dig[32];
    char ver[72], chal[48];
    rand_bytes(rnd, sizeof rnd);
    b64url(rnd, sizeof rnd, ver);
    sha256(ver, strlen(ver), dig);
    b64url(dig, sizeof dig, chal);
    char *state = str_printf("{\"code_verifier\":\"%s\",\"logonRedirectPath\":\"/\",\"optout\":false}", ver);
    char *es = url_encode(state), *er = url_encode(DR_CALLBACK), *esc = url_encode("openid roles tracking profile email offline_access");
    char *url = str_printf(DR_LOGIN "/oidc/authorize?client_id=%s&code_challenge=%s&code_challenge_method=S256&redirect_uri=%s&state=%s&response_type=code&scope=%s",
                           DR_CLIENT_ID, chal, er, es, esc);
    free(state); free(es); free(esc);
    CBuf b = {0};
    char *eff = NULL, *trans = NULL, *href = NULL, *code = NULL;
    cJSON *t = NULL;
    long st = lreq(c, url, NULL, NULL, true, &b, &eff, NULL);
    free(url);
    free(b.d);
    /* .../transactions/<id>: the login's transaction */
    if (st == 200 && eff) {
        char *q = strchr(eff, '?');
        if (q) *q = 0;
        char *sl = strrchr(eff, '/');
        if (sl && sl[1] && strstr(eff, "/transactions/")) trans = str_dup(sl + 1);
    }
    if (!trans) { login_fail(j, st ? T("DR's login did not answer as expected") : T("Cannot reach DR - check the network")); goto out; }

    cJSON *r = graphql(c, "T", "query T($id: ID!) { transaction(id: $id) { __typename } }", gql_vars(trans, NULL, NULL));
    cJSON_Delete(r);
    r = graphql(c, "I", "mutation I($input: IdentificationInput!) { identify(input: $input) { __typename ... on Error { code message } } }",
                gql_vars(trans, "email", j->email));
    const char *tn = jv(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "data"), "identify"), "__typename");
    if (tn && strstr(tn, "Unrecognized")) login_fail(j, T("There is no DR account with that e-mail"));
    else if (!tn) login_fail(j, T("DR's login did not answer as expected"));
    cJSON_Delete(r);
    if (j->err[0]) goto out;
    r = graphql(c, "A", "mutation A($input: AuthenticationInput!) { authenticate(input: $input) { __typename ... on Error { code message } ... on AuthenticatedAuthenticationTransaction { href } } }",
                gql_vars(trans, "password", j->pw));
    memset(j->pw, 0, sizeof j->pw); /* not needed any more */
    cJSON *au = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "data"), "authenticate");
    href = js(au, "href");
    if (!href) {
        const char *m = jv(au, "message");
        tlog("drtv: login refused (%s)", jv(au, "__typename") ? jv(au, "__typename") : "no reply");
        login_fail(j, m && *m ? m : T("Wrong e-mail or password"));
    }
    cJSON_Delete(r);
    if (!href) goto out;
    /* follow the redirects by hand until DR sends us to the callback with ?code= */
    char *next = str_dup(href);
    for (int i = 0; i < 12 && next && !code; i++) {
        if (!strncmp(next, DR_CALLBACK, strlen(DR_CALLBACK))) { code = query_param(next, "code"); break; }
        CBuf hb = {0};
        char *redir = NULL;
        lreq(c, next, NULL, NULL, false, &hb, NULL, &redir);
        free(hb.d);
        free(next);
        next = redir;
    }
    free(next);
    if (!code) { login_fail(j, T("DR's login did not answer as expected")); goto out; }
    char *ec = url_encode(code), *ev = url_encode(ver);
    char *form = str_printf("client_id=%s&redirect_uri=%s&code_verifier=%s&code=%s&grant_type=authorization_code", DR_CLIENT_ID, er, ev, ec);
    free(ec); free(ev);
    long ts = 0;
    t = oidc_token(form, &ts);
    free(form);
    if (!t) { login_fail(j, T("DR's login did not answer as expected")); goto out; }
    SDL_LockMutex(tok_mu);
    bool ok = tokens_exchange(t);
    SDL_UnlockMutex(tok_mu);
    const char *rt = jv(t, "refresh_token");
    if (!ok) { login_fail(j, T("DR's login did not answer as expected")); goto out; }
    if (j->signout != SDL_GetAtomicInt(&signout_a)) goto out; /* signed out meanwhile */
    if (rt && *rt) login_write(rt);
    j->ok = true;
out:
    memset(j->pw, 0, sizeof j->pw);
    cJSON_Delete(t);
    free(eff); free(trans); free(href); free(code); free(er);
    curl_easy_cleanup(c);
}
static void login_release(Blk *bb) {
    LoginJob *j = (LoginJob *)bb;
    memset(j, 0, sizeof *j);
    free(j);
}

typedef struct { Dr *v; char email[256]; } LoginAsk;
static void login_pw_done(void *ud, const char *pw) {
    LoginAsk *a = ud;
    Dr *v = a->v;
    if (!pw || !*pw || v != live) { free(a); return; }
    LoginJob *j = calloc(1, sizeof *j);
    j->b.run = login_run;
    j->b.release = login_release;
    snprintf(j->email, sizeof j->email, "%s", a->email);
    snprintf(j->pw, sizeof j->pw, "%s", pw);
    j->signout = SDL_GetAtomicInt(&signout_a);
    free(a);
    if (!blk_wait(&j->b, T("Signing in to DR…"))) return;
    if (j->ok) {
        cfg_set("drtv_user", j->email);
        SDL_SetAtomicInt(&login_lost, 0);
        app_toast("%s", T("Signed in to DR"));
        if (v == live) { build_side(v); go(v, V_HOME, "/", "", false); }
    } else {
        app_toast(T("Sign-in failed: %s"), j->err[0] ? j->err : T("unknown error"));
    }
    blk_release(&j->b);
}
static void login_email_done(void *ud, const char *email) {
    Dr *v = ud;
    if (!email || v != live) return;
    char *e = str_dup(email);
    str_trim(e);
    if (!*e || !strchr(e, '@')) {
        if (*e) app_toast("%s", T("That does not look like an e-mail address"));
        free(e);
        return;
    }
    LoginAsk *a = calloc(1, sizeof *a);
    a->v = v;
    snprintf(a->email, sizeof a->email, "%s", e);
    free(e);
    keyboard_open(T("Your DR password"), "", true, login_pw_done, a);
}
static void start_login(Dr *v) { keyboard_open(T("Your DR account's e-mail"), "", false, login_email_done, v); }

/* sign out / uninstall: everything about the account and what was watched goes */
static void drtv_forget(void) {
    dr_signout++;
    dr_gen++;
    SDL_AddAtomicInt(&signout_a, 1);
    char *p = app_file("login");
    wipe_file(p);
    free(p);
    p = app_file("progress.json");
    wipe_file(p);
    free(p);
    cJSON_Delete(prog);
    prog = NULL;
    home_clear();
    unlink(LIVE_M3U);
    if (tok_mu) tokens_clear();
    cfg_set("drtv_user", "");
    cfg_set("drtv_device", ""); /* a new anonymous device next time */
    dev_id[0] = 0;
    SDL_SetAtomicInt(&login_lost, 0);
    wipe_session_traces();
    if (live) { live->nback = 0; build_side(live); }
}
static void logout_cb(void *ud, bool yes) {
    Dr *v = ud;
    if (!yes) return;
    drtv_forget();
    app_toast("%s", T("Signed out of DR"));
    if (v == live) go(v, V_HOME, "/", "", false);
}

/* ---------- screen ---------- */
enum { M_HOME, M_LIVE, M_CATS, M_AZ, M_KIDS, M_SEARCH, M_MYLIST, M_LOGIN, M_LOGOUT };
static void build_side(Dr *v) {
    side_clear(&v->side);
    snprintf(v->side.subtitle, sizeof v->side.subtitle, "%s", signed_in() ? dr_user() : "");
    int n = 0;
#define ADD(label, icon, gap, id) (n = side_add(&v->side, label, icon, gap), v->side_map[n] = (id))
    ADD(T("Home"), ICON_STAR, false, M_HOME);
    ADD(T("Live TV"), ICON_TV, false, M_LIVE);
    ADD(T("Categories"), ICON_FOLDER, false, M_CATS);
    ADD(T("A-Z"), ICON_FILM, false, M_AZ);
    ADD(T("Kids"), ICON_PLAY, false, M_KIDS);
    ADD(T("Search"), ICON_SEARCH, false, M_SEARCH);
    if (signed_in()) {
        ADD(T("My list"), ICON_CHECK, false, M_MYLIST);
        ADD(T("Sign out"), ICON_LOCK, true, M_LOGOUT);
    } else if (can_login()) {
        ADD(T("Sign in"), ICON_LOCK, true, M_LOGIN);
    }
#undef ADD
    (void)n;
}

static void search_kb_done(void *ud, const char *q) {
    Dr *v = ud;
    if (!q || v != live) return;
    char *t = str_dup(q);
    str_trim(t);
    if (*t) {
        char *h = str_printf(T("Search: %s"), t);
        go(v, V_SEARCH, t, h, v->view != V_HOME && v->view != V_LIVE && v->view != V_SEARCH);
        free(h);
    }
    free(t);
}
static void open_search(Dr *v) { keyboard_open(T("Search DRTV"), "", false, search_kb_done, v); }

static void side_activate(Dr *v, int item) {
    int id = item >= 0 && item < 16 ? v->side_map[item] : M_HOME;
    if (id != M_SEARCH && id != M_LOGIN && id != M_LOGOUT) v->side.active = item;
    switch (id) {
    case M_HOME: go(v, V_HOME, "/", "", false); break;
    case M_LIVE: go(v, V_LIVE, "", T("Live TV"), false); break;
    case M_CATS: go(v, V_BROWSE, "/kategorier", T("Categories"), false); break;
    case M_AZ: go(v, V_BROWSE, "/kategorier/a-aa", T("A-Z"), false); break;
    case M_KIDS: go(v, V_BROWSE, "/ramasjang_a-aa", T("Kids"), false); break;
    case M_SEARCH: open_search(v); break;
    case M_MYLIST: go(v, V_MYLIST, "", T("My list"), false); break;
    case M_LOGIN: start_login(v); break;
    case M_LOGOUT:
        confirm_open(T("Sign out of DR?"), T("What you have watched on this box is forgotten too."), T("Sign out"), T("Cancel"), logout_cb, v);
        break;
    }
}

static void open_item(Dr *v, const DItem *t) {
    switch (t->kind) {
    case K_EPISODE:
    case K_MOVIE:
        if (t->kind == K_MOVIE && t->path && v->view != V_DETAIL) { go(v, V_DETAIL, t->path, t->title, true); return; }
        play_item(v, t, false);
        return;
    case K_SEASON:
    case K_SHOW:
        if (t->path) go(v, V_DETAIL, t->path, t->title, true);
        return;
    case K_CHANNEL:
        for (int c = 0; c < N_CHANNELS; c++)
            if (t->id && !strcmp(t->id, CHANNELS[c].id)) { play_live(v, c); return; }
        app_toast("%s", T("The channel could not be started"));
        return;
    case K_LINK:
        if (t->path) go(v, V_BROWSE, t->path, t->title, true);
        return;
    }
}

static void drtv_init(void);
static void dev_init(void) {
    if (!dev_id[0]) {
        const char *d = cfg_get("drtv_device", "");
        if (strlen(d) == 36) snprintf(dev_id, sizeof dev_id, "%s", d);
        else {
            uint8_t r[16];
            rand_bytes(r, sizeof r);
            r[6] = (r[6] & 0x0f) | 0x40;
            r[8] = (r[8] & 0x3f) | 0x80;
            snprintf(dev_id, sizeof dev_id, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
            cfg_set("drtv_device", dev_id);
        }
    }
}
static void dr_enter(Screen *s) {
    Dr *v = s->st;
    live = v;
    drtv_init();
    side_init(&v->side, "DRTV", ICON_TV, DR_RED);
    build_side(v);
    v->want_row = -1;
    go(v, V_HOME, "/", "", false);
}
static void dr_resume(Screen *s) { live = s->st; }
static void dr_destroy(Screen *s) {
    Dr *v = s->st;
    rows_clear(v);
    if (live == v) live = NULL;
    dr_gen++;
    prog_save();
    free(v);
    free(s);
}

static float row_card_w(const Row *r) { return r->big ? BIG_W : TILE_W; }
static float row_card_h(const Row *r) { return r->big ? BIG_H : TILE_H; }
static float row_h(const Row *r) { return 56 + row_card_h(r) + (r->big ? 120 : 96); }
static float rows_top(const Dr *v) { return v->view == V_DETAIL ? DETAIL_TOP : v->heading[0] ? 220 : 150; }
static float scroll_target(const Dr *v) {
    if (v->view == V_DETAIL && (v->d_focus || v->row == 0)) return 0; /* the first row fits under the header */
    float y = 0; /* the focused row where the first row starts (the heading shows on the first row) */
    for (int i = 0; i < v->row && i < v->nrows; i++) y += row_h(&v->rows[i]);
    if (v->view == V_DETAIL) y += rows_top(v) - 150;
    return y > 0 ? y : 0;
}

static void dr_update(Screen *s, float dt) {
    Dr *v = s->st;
    side_update(&v->side, dt);
    v->scroll = anim_to(v->scroll, scroll_target(v), dt, 12);
    lazy_tick(v);
    if (v->view == V_LIVE && !v->loading && app_time() - v->live_loaded > 60) { v->loading = true; dr_gen++; load_live(v); }
    if (SDL_GetAtomicInt(&login_lost)) {
        SDL_SetAtomicInt(&login_lost, 0);
        char *p = app_file("login");
        wipe_file(p);
        free(p);
        cfg_set("drtv_user", "");
        build_side(v);
        app_toast("%s", T("Your DR login has expired - sign in again"));
    }
}

static void draw_chip(const char *txt, float x, float y, Color bg) {
    float w = text_width(F_BOLD, 16, txt) + 18;
    fill_rrect(x, y, w, 28, 6, bg);
    text_draw(F_BOLD, 16, x + w / 2, y + 5, C_TEXT, AL_CENTER, txt);
}
static void draw_card(const Row *r, const DItem *t, float x, float y, bool f) {
    float w = row_card_w(r), h = row_card_h(r);
    float fa = ui_focus(t, f), sc = 1 + 0.07f * fa, ww = w * sc, hh = h * sc, xx = x - (ww - w) / 2, yy = y - (hh - h) / 2;
    ui_focus_under(xx, yy, ww, hh, 12, fa);
    fill_rrect(xx, yy, ww, hh, 12, C_SURFACE);
    Image *im = t->img ? img_get(t->img) : NULL;
    if (!im || !img_draw_cover(im, xx, yy, ww, hh, 12, 1)) {
        fill_rrect(xx, yy, ww, hh, 12, color_mix(C_SURFACE, DR_RED, 0.3f));
        text_draw_wrap(F_SEMI, 24, xx + 18, yy + 20, ww - 36, 3, 30, C_TEXT2, t->title ? t->title : "");
    }
    if (fa < 0.999f) fill_rrect(xx, yy, ww, hh, 12, RGBA(0, 0, 0, (unsigned char)(70 * (1 - fa))));
    if (t->kind == K_CHANNEL) {
        draw_chip("LIVE", xx + 12, yy + 12, DR_RED);
    } else if (t->kind == K_SEASON || t->kind == K_SHOW) {
        draw_chip(T("SERIES"), xx + 12, yy + 12, RGBA(0, 0, 0, 180));
    }
    if (t->prog > 0.01) {
        float bw = ww - 24;
        fill_rect(xx + 12, yy + hh - 12, bw, 5, RGBA(255, 255, 255, 80));
        fill_rect(xx + 12, yy + hh - 12, bw * (float)(t->prog > 1 ? 1 : t->prog), 5, DR_RED);
    }
    ui_focus_over(xx, yy, ww, hh, 12, fa);
    float ls = r->big ? 36 : 30;
    text_draw_clip(F_SEMI, r->big ? 26 : 22, x, y + h + 14, w, f ? C_TEXT : C_TEXT2, AL_LEFT, t->title ? t->title : "");
    if (t->sub) text_draw_clip(F_REG, r->big ? 21 : 19, x, y + h + 14 + ls, w, C_TEXT2, AL_LEFT, t->sub);
    if (t->next) text_draw_clip(F_REG, r->big ? 21 : 19, x, y + h + 14 + ls * 2 - 4, w, C_TEXT3, AL_LEFT, t->next);
}

static void draw_rows(Dr *v, bool focus_rows) {
    float x0 = SIDE_W + 60;
    float y = rows_top(v) - v->scroll;
    for (int i = 0; i < v->nrows; i++) {
        Row *r = &v->rows[i];
        float h = row_h(r);
        if (y + h >= 100 && y <= UI_H) {
            text_draw(F_BOLD, 32, x0, y, C_TEXT, AL_LEFT, r->title);
            float cw = row_card_w(r) + GAP;
            int vis = (int)((UI_W - x0) / cw);
            if (vis < 1) vis = 1;
            if (i == v->row) {
                if (v->col < r->off) r->off = v->col;
                if (v->col > r->off + vis - 2) r->off = v->col - (vis - 2);
                if (r->off > r->n - vis + 1) r->off = r->n - vis + 1;
                if (r->off < 0) r->off = 0;
            }
            if (!r->n && r->loading) draw_spinner(x0 + 40, y + 56 + row_card_h(r) / 2, 18, C_TEXT3);
            for (int k = 0; k <= vis && r->off + k < r->n; k++) {
                int idx = r->off + k;
                draw_card(r, &r->it[idx], x0 + k * cw, y + 56, focus_rows && i == v->row && idx == v->col);
            }
        }
        y += h;
    }
}

static void draw_detail(Dr *v) {
    float x0 = SIDE_W + 60;
    float off = v->scroll;
    if (v->d_img) {
        Image *im = img_get(v->d_img);
        float iw = UI_W * 0.62f, ih = iw * 9.0f / 16.0f, ix = UI_W - iw;
        if (im) img_draw_cover(im, ix, -off, iw, ih, 0, 1);
        fill_hgrad(ix, -off, iw * 0.45f, ih, C_BG, RGBA(C_BG.r, C_BG.g, C_BG.b, 0));
        fill_vgrad(ix, ih * 0.55f - off, iw, ih * 0.45f + 2, RGBA(C_BG.r, C_BG.g, C_BG.b, 0), C_BG);
    }
    float y = 160 - off;
    text_draw_clip(F_BOLD, 56, x0, y, UI_W * 0.55f, C_TEXT, AL_LEFT, v->d_title ? v->d_title : v->heading);
    if (v->d_meta && *v->d_meta) text_draw_clip(F_SEMI, 24, x0, y + 78, UI_W * 0.5f, C_TEXT2, AL_LEFT, v->d_meta);
    if (v->d_desc) text_draw_wrap(F_REG, 24, x0, y + 122, UI_W * 0.42f, 4, 34, C_TEXT2, v->d_desc);
    if (v->d_nbtn) {
        char resume[96];
        const char *labels[2];
        double pos = prog_pos(v->d_play.id);
        if (v->d_nbtn == 2) {
            char d[32];
            fmt_duration(pos, d, sizeof d);
            snprintf(resume, sizeof resume, T("Resume from %s"), d);
            labels[0] = resume;
            labels[1] = T("Start over");
        } else {
            labels[0] = T("Play");
        }
        float bw = 0;
        for (int i = 0; i < v->d_nbtn; i++) { /* the same widths as draw_buttons */
            float w = text_width(F_SEMI, 28, labels[i]) + 90;
            bw += (w < 260 ? 260 : w) + (i ? 30 : 0);
        }
        draw_buttons(x0 + bw / 2, y + 290, labels, v->d_nbtn, v->d_focus && !v->side.open ? v->d_btn : -1);
        /* which episode Play starts */
        if (v->d_play.title && v->d_play.kind == K_EPISODE)
            text_draw_clip(F_REG, 22, x0, y + 380, UI_W * 0.5f, C_TEXT3, AL_LEFT, v->d_play.title);
    }
}

static void dr_draw(Screen *s) {
    Dr *v = s->st;
    draw_background();
    draw_topbar(NULL);
    float x0 = SIDE_W + 60, cx = x0 + (UI_W - x0) / 2;
    bool cf = !v->side.open;
    if (v->view == V_DETAIL) {
        clip_push(SIDE_W, 0, UI_W, UI_H - 70);
        draw_detail(v);
        clip_pop();
    }
    clip_push(SIDE_W, 120, UI_W, UI_H - 190); /* under the top bar, above the key bar */
    if (v->view != V_DETAIL && v->heading[0]) text_draw(F_BOLD, 44, x0, 130 - v->scroll, C_TEXT, AL_LEFT, v->heading);
    draw_rows(v, cf && !(v->view == V_DETAIL && v->d_focus));
    clip_pop();
    if (v->loading && !v->nrows && !(v->view == V_DETAIL && v->d_title)) draw_spinner(cx, UI_H / 2, 30, C_TEXT);
    else if (!v->nrows && !v->d_nbtn && v->msg[0]) {
        draw_icon(ICON_TV, cx, UI_H / 2 - 90, 90, DR_RED);
        text_draw_wrap(F_REG, 28, cx - 480, UI_H / 2, 960, 4, 40, C_TEXT2, v->msg);
    }
    bool chan = v->nrows && v->row < v->nrows && v->col < v->rows[v->row].n && v->rows[v->row].it[v->col].kind == K_CHANNEL;
    draw_keybar(x0, UI_H - 62, chan ? T("Watch") : T("Open"), NULL, NULL, T("Search"), T("Menu"));
    side_draw(&v->side);
}

static bool dr_input(Screen *s, const InputEvent *ev) {
    Dr *v = s->st;
    int act;
    if (side_input(&v->side, ev, &act)) {
        if (act >= 0) side_activate(v, act);
        return true;
    }
    if (ev->a == A_MENU) { side_open(&v->side); return true; }
    if (ev->a == A_SEARCH) { open_search(v); return true; }
    if (ev->a == A_CHAR) {
        char init[8] = {(char)ev->ch, 0};
        keyboard_open(T("Search DRTV"), init, false, search_kb_done, v);
        return true;
    }
    if (ev->a == A_BACK) {
        if (go_back(v)) return true;
        if (v->view != V_HOME) { v->side.active = 0; go(v, V_HOME, "/", "", false); return true; }
        return false;
    }
    /* the buttons on the detail page */
    if (v->view == V_DETAIL && v->d_focus) {
        switch (ev->a) {
        case A_LEFT:
            if (v->d_btn > 0) v->d_btn--;
            else side_open(&v->side);
            return true;
        case A_RIGHT: if (v->d_btn + 1 < v->d_nbtn) v->d_btn++; return true;
        case A_DOWN:
            if (v->nrows) { v->d_focus = false; v->row = 0; v->col = 0; return true; }
            return false;
        case A_UP: return true;
        case A_OK: case A_PLAY: case A_PLAYPAUSE:
            if (!ev->repeat && v->d_has_play) {
                DItem t;
                ditem_copy(&t, &v->d_play);
                play_item(v, &t, v->d_btn == 1);
                ditem_free(&t);
            }
            return true;
        default: return false;
        }
    }
    if (!v->nrows) {
        if (ev->a == A_LEFT) { side_open(&v->side); return true; }
        return false;
    }
    Row *r = &v->rows[v->row];
    switch (ev->a) {
    case A_UP:
        if (v->row > 0) { v->row--; v->col = v->rows[v->row].off < v->rows[v->row].n ? v->rows[v->row].off : 0; }
        else if (v->view == V_DETAIL && v->d_nbtn) v->d_focus = true;
        return true;
    case A_DOWN:
        if (v->row + 1 >= v->nrows) return false; /* the key bar */
        v->row++;
        v->col = v->rows[v->row].off < v->rows[v->row].n ? v->rows[v->row].off : 0;
        return true;
    case A_LEFT:
        if (v->col > 0) v->col--;
        else side_open(&v->side);
        return true;
    case A_RIGHT: if (v->col + 1 < r->n) v->col++; return true;
    case A_OK: case A_PLAY: case A_PLAYPAUSE:
        if (!ev->repeat && v->col < r->n) {
            DItem t;
            ditem_copy(&t, &r->it[v->col]); /* the rows can change while it plays */
            open_item(v, &t);
            ditem_free(&t);
        }
        return true;
    default: return false;
    }
}

static Screen *drtv_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    Dr *v = calloc(1, sizeof *v);
    s->name = "drtv";
    s->st = v;
    s->enter = dr_enter;
    s->resume = dr_resume;
    s->update = dr_update;
    s->draw = dr_draw;
    s->input = dr_input;
    s->destroy = dr_destroy;
    return s;
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
static void drtv_init(void) {
    if (!tok_mu) tok_mu = SDL_CreateMutex();
    dev_init();
}

/* the row on the home screen: what you can continue (the positions saved on this box, newest first) */
#define HOME_MAX 12
static HomeItem home_rows[HOME_MAX];
static int n_home;
static void home_clear(void) {
    for (int i = 0; i < n_home; i++) {
        free(home_rows[i].title); free(home_rows[i].subtitle); free(home_rows[i].image); free(home_rows[i].key); free(home_rows[i].logo);
    }
    memset(home_rows, 0, sizeof home_rows);
    n_home = 0;
}
static void drtv_row_refresh(void) {
    home_clear();
    cJSON **e;
    int k = prog_sorted(&e);
    for (int i = 0; i < k && n_home < HOME_MAX; i++) {
        HomeItem *h = &home_rows[n_home++];
        h->title = js(e[i], "title");
        if (!h->title) h->title = str_dup("DRTV");
        h->subtitle = js(e[i], "sub");
        h->image = img_sized(jv(e[i], "img"), 1280, 720);
        h->key = str_printf("dr:%s", e[i]->string);
        double d = jnum(e[i], "dur");
        h->progress = d > 0 ? jnum(e[i], "pos") / d : 0;
    }
    free(e);
}
static int drtv_row(HomeItem **items) {
    *items = home_rows;
    return n_home;
}
static void drtv_row_open(const HomeItem *it, int idx) {
    (void)idx;
    if (!it->key || strncmp(it->key, "dr:", 3) || !id_ok(it->key + 3)) return;
    prog_load();
    cJSON *e = cJSON_GetObjectItem(prog, it->key + 3);
    DItem t = {0};
    t.kind = K_EPISODE;
    t.id = str_dup(it->key + 3);
    t.title = e ? js(e, "title") : str_dup(it->title);
    t.sub = e ? js(e, "sub") : NULL;
    t.img = e ? js(e, "img") : NULL;
    drtv_init();
    play_vod(NULL, &t, false); /* from where you stopped */
    ditem_free(&t);
    drtv_row_refresh();
}
static void act_logout(void) { app_ask_signout("DRTV", drtv_forget); }
static const char *dr_signed_out(void) { return signed_in() ? NULL : ITEM_HIDDEN; }
static const Item dr_items[] = {
    {N_("Sign out of DRTV"), IT_ACTION, .action = act_logout, .help = N_("Deletes the DR login and what you have watched from this box"),
     .locked = dr_signed_out},
};
const NotaApp nota_app = {
    NOTA_API, "drtv", "DRTV", N_("DR's live channels and programmes - free, no account needed"), APP_VERSION, 36, 0,
    ICON_TV, {214, 22, 52, 255}, {118, 10, 28, 255}, {255, 255, 255, 255},
    .open = drtv_screen, .init = drtv_init, .forget = drtv_forget,
    .settings = dr_items, .n_settings = sizeof dr_items / sizeof dr_items[0],
    .row_title = N_("Continue watching"), .shelf_label = N_("CONTINUE IN DRTV"),
    .row = drtv_row, .row_refresh = drtv_row_refresh, .row_open = drtv_row_open,
};
