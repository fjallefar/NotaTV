/* NotaTV app package "plex" - Plex: login (PIN), several servers, hubs, libraries, series, trailers, playback */
#include "plex.h"
#include <ctype.h>
#include <math.h>
#include "vendor/cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ---------- connection ---------- */
static char client_id[64];
static int plex_gen; /* increased on sign-out: replies from before are ignored */

const char *plex_client_id(void) {
    if (client_id[0]) return client_id;
    const char *c = cfg_get("plex_client_id", "");
    if (*c) { snprintf(client_id, sizeof client_id, "%s", c); return client_id; }
    unsigned r[4];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, sizeof r, 1, f) != 1) for (int i = 0; i < 4; i++) r[i] = (unsigned)rand() ^ (unsigned)time(NULL);
    if (f) fclose(f);
    snprintf(client_id, sizeof client_id, "notatv-%08x%08x%08x", r[0], r[1], r[2]);
    cfg_set("plex_client_id", client_id);
    return client_id;
}

/* build a header list; token may be NULL */
typedef struct { char h[8][400]; const char *p[9]; } Hdrs;
static const char *const *plex_headers(Hdrs *H, const char *token) {
    int n = 0;
    snprintf(H->h[n++], 400, "Accept: application/json");
    snprintf(H->h[n++], 400, "X-Plex-Product: NotaTV");
    snprintf(H->h[n++], 400, "X-Plex-Version: %s", TV_VERSION);
    snprintf(H->h[n++], 400, "X-Plex-Client-Identifier: %s", plex_client_id());
    snprintf(H->h[n++], 400, "X-Plex-Platform: Linux");
    /* the box's own name (as AirPlay and DLNA use) - control characters would break the header */
    char name[128];
    snprintf(name, sizeof name, "%s", cfg_get("device_name", "NotaTV"));
    for (char *c = name; *c; c++) if ((unsigned char)*c < 32) *c = ' ';
    snprintf(H->h[n++], 400, "X-Plex-Device-Name: %s", name[0] ? name : "NotaTV");
    if (token && *token) snprintf(H->h[n++], 400, "X-Plex-Token: %s", token);
    for (int i = 0; i < n; i++) H->p[i] = H->h[i];
    H->p[n] = NULL;
    return H->p;
}

/* ---------- servers (stored in plex-servers.json) ---------- */
#define MAX_SRV 16
typedef struct {
    char id[96], name[128], uri[300], token[300], owner[128];
    char hidden[256]; /* fravalgte biblioteker: ",1,4," */
    bool owned, enabled;
} PSrv;
static PSrv S[MAX_SRV];
static int nS;
static bool S_loaded;

static void srv_path(char *p, size_t n) { snprintf(p, n, "%s/plex-servers.json", data_dir()); }

static void srv_save(void) {
    cJSON *arr = cJSON_CreateArray();
    char names[400] = "";
    const char *first_uri = "";
    for (int i = 0; i < nS; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", S[i].id);
        cJSON_AddStringToObject(o, "name", S[i].name);
        cJSON_AddStringToObject(o, "uri", S[i].uri);
        cJSON_AddStringToObject(o, "token", S[i].token);
        cJSON_AddStringToObject(o, "owner", S[i].owner);
        cJSON_AddStringToObject(o, "hidden", S[i].hidden);
        cJSON_AddBoolToObject(o, "owned", S[i].owned);
        cJSON_AddBoolToObject(o, "enabled", S[i].enabled);
        cJSON_AddItemToArray(arr, o);
        if (S[i].enabled && S[i].uri[0]) {
            if (!*first_uri) first_uri = S[i].uri;
            if (names[0]) strncat(names, ", ", sizeof names - strlen(names) - 1);
            strncat(names, S[i].name, sizeof names - strlen(names) - 1);
        }
    }
    char *txt = cJSON_Print(arr);
    cJSON_Delete(arr);
    char path[512], tmp[520];
    srv_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) { fputs(txt, f); fclose(f); rename(tmp, path); }
    free(txt);
    /* for the Settings page */
    cfg_set("plex_server_name", names);
    cfg_set("plex_server_uri", first_uri);
}

static void srv_load(void) {
    if (S_loaded) return;
    S_loaded = true;
    nS = 0;
    char path[512];
    srv_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = calloc(1, n + 1);
        if (fread(buf, 1, n, f) == (size_t)n) {
            cJSON *arr = cJSON_Parse(buf), *o;
            cJSON_ArrayForEach(o, arr) {
                if (nS >= MAX_SRV) break;
                PSrv *s = &S[nS++];
                memset(s, 0, sizeof *s);
#define GS(k, dst) do { cJSON *v = cJSON_GetObjectItem(o, k); if (cJSON_IsString(v)) snprintf(dst, sizeof dst, "%s", v->valuestring); } while (0)
                GS("id", s->id); GS("name", s->name); GS("uri", s->uri); GS("token", s->token); GS("owner", s->owner);
                GS("hidden", s->hidden);
#undef GS
                s->owned = cJSON_IsTrue(cJSON_GetObjectItem(o, "owned"));
                s->enabled = cJSON_IsTrue(cJSON_GetObjectItem(o, "enabled"));
            }
            cJSON_Delete(arr);
        }
        free(buf);
        fclose(f);
    }
    /* migration from the single-server version */
    if (nS == 0 && *cfg_get("plex_server_uri", "") && *cfg_get("plex_token", "")) {
        PSrv *s = &S[nS++];
        memset(s, 0, sizeof *s);
        snprintf(s->id, sizeof s->id, "%s", cfg_get("plex_server_id", "ukendt"));
        snprintf(s->name, sizeof s->name, "%s", cfg_get("plex_server_name", "Plex"));
        snprintf(s->uri, sizeof s->uri, "%s", cfg_get("plex_server_uri", ""));
        snprintf(s->token, sizeof s->token, "%s", cfg_get("plex_server_token", cfg_get("plex_token", "")));
        s->owned = s->enabled = true;
        srv_save();
    }
}

static int n_enabled(void) {
    int n = 0;
    for (int i = 0; i < nS; i++) n += S[i].enabled && S[i].uri[0];
    return n;
}
static bool srv_ok(int si) { return si >= 0 && si < nS && S[si].uri[0]; }

static bool lib_hidden(int si, int section) {
    if (!srv_ok(si) || section <= 0) return false;
    char k[24];
    snprintf(k, sizeof k, ",%d,", section);
    return strstr(S[si].hidden, k) != NULL;
}

static void lib_set_hidden(int si, int section, bool hide) {
    char k[24];
    snprintf(k, sizeof k, ",%d,", section);
    char *p = strstr(S[si].hidden, k);
    if (hide && !p) {
        if (!S[si].hidden[0]) snprintf(S[si].hidden, sizeof S[si].hidden, ",");
        strncat(S[si].hidden, k + 1, sizeof S[si].hidden - strlen(S[si].hidden) - 1);
    } else if (!hide && p) memmove(p, p + strlen(k) - 1, strlen(p + strlen(k) - 1) + 1);
    if (!strcmp(S[si].hidden, ",")) S[si].hidden[0] = 0;
    srv_save();
}

static char *img_url(int si, const char *path, int w, int h);
/* title logos have transparency: PNG instead of JPEG */
static char *logo_url(int si, const char *path) {
    char *u = img_url(si, path, 600, 240);
    if (!u || strstr(u, "://") == NULL || !strstr(u, "/photo/:/transcode")) return u;
    char *r = str_printf("%s&format=png", u);
    free(u);
    return r;
}

/* image URLs are built for every tile in every frame: a cache (direct mapped) saves url_encode + printf.
   Only the main thread calls img_url. An entry is only valid if the server's address and token are the same. */
#define IMGU_N 512
static struct { int si, w, h; char *path, *url; } imgu[IMGU_N];
static char *img_url(int si, const char *path, int w, int h) {
    if (!path || !*path || !srv_ok(si)) return NULL;
    if (!strncmp(path, "http", 4)) return str_dup(path);
    unsigned long hs = 5381;
    for (const char *c = path; *c; c++) hs = hs * 33 + (unsigned char)*c;
    unsigned slot = (unsigned)((hs ^ ((unsigned long)si * 7919u) ^ ((unsigned long)w << 12) ^ (unsigned long)h) % IMGU_N);
    size_t ul = strlen(S[si].uri), tl = strlen(S[si].token);
    if (imgu[slot].url && imgu[slot].si == si && imgu[slot].w == w && imgu[slot].h == h && !strcmp(imgu[slot].path, path)) {
        const char *u = imgu[slot].url;
        size_t l = strlen(u);
        if (!strncmp(u, S[si].uri, ul) && l > tl && !strcmp(u + l - tl, S[si].token)) return str_dup(u);
    }
    char *e = url_encode(path);
    char *r = str_printf("%s/photo/:/transcode?width=%d&height=%d&minSize=1&upscale=1&url=%s&X-Plex-Token=%s",
                         S[si].uri, w, h, e, S[si].token);
    free(e);
    free(imgu[slot].path);
    free(imgu[slot].url);
    imgu[slot].si = si; imgu[slot].w = w; imgu[slot].h = h;
    imgu[slot].path = str_dup(path);
    imgu[slot].url = str_dup(r);
    return r;
}

/* ---------- elementer ---------- */
typedef struct {
    char *rating_key, *key, *type, *title, *subtitle, *summary, *thumb, *art, *part, *year, *rating, *subtype;
    char *sart; /* episodes: the series' backdrop (for "Continue watching") */
    char *clogo; /* title logo (Image type clearLogo, needs includeImages=1) */
    char **subs;
    int n_subs;
    int sid; /* subtitle track chosen in Plex (mpv's number among the subtitle tracks: embedded first, then the files), 0 = none */
    int aid; /* audio track chosen in Plex (mpv's number among the audio tracks, 1-based), 0 = mpv chooses */
    double duration, offset, last_viewed; /* seconds / epoch */
    int view_count, index, parent_index, leaves, viewed_leaves, extra_type, section;
    int rt_critic, rt_audience, imdb10; /* -1 = unknown */
    int tmdb;
    char *show; /* series title for episodes */
    char *grandparent_key; /* episodes and seasons: the series' rating key */
    char *parent_key; /* episodes: the season's rating key */
    int srv; /* server-indeks i S[] */
    bool hdr; /* the video track is HDR (PQ/HLG) */
    int width; /* video width (0 = unknown) */
    bool full; /* stream info is known (full metadata, not a list entry) */
} PItem;

static char *jstr(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v)) return str_dup(v->valuestring);
    if (cJSON_IsNumber(v)) return str_printf("%d", v->valueint);
    return NULL;
}
static double jnum(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void pitem_parse(PItem *it, cJSON *m, int si) {
    memset(it, 0, sizeof *it);
    it->srv = si;
    it->rating_key = jstr(m, "ratingKey");
    it->key = jstr(m, "key");
    it->type = jstr(m, "type");
    it->title = jstr(m, "title");
    it->summary = jstr(m, "summary");
    it->year = jstr(m, "year");
    it->rating = jstr(m, "contentRating");
    it->subtype = jstr(m, "subtype");
    it->duration = jnum(m, "duration") / 1000;
    it->offset = jnum(m, "viewOffset") / 1000;
    it->last_viewed = jnum(m, "lastViewedAt");
    it->view_count = (int)jnum(m, "viewCount");
    it->index = (int)jnum(m, "index");
    it->parent_index = (int)jnum(m, "parentIndex");
    it->leaves = (int)jnum(m, "leafCount");
    it->viewed_leaves = (int)jnum(m, "viewedLeafCount");
    it->extra_type = (int)jnum(m, "extraType");
    it->section = (int)jnum(m, "librarySectionID");
    cJSON *gu;
    cJSON_ArrayForEach(gu, cJSON_GetObjectItem(m, "Guid")) {
        cJSON *gid = cJSON_GetObjectItem(gu, "id");
        if (cJSON_IsString(gid) && !strncmp(gid->valuestring, "tmdb://", 7)) it->tmdb = atoi(gid->valuestring + 7);
    }
    /* ratings: the new "Rating" array (values 0-10), otherwise older fields */
    it->rt_critic = it->rt_audience = it->imdb10 = -1;
    cJSON *rr;
    cJSON_ArrayForEach(rr, cJSON_GetObjectItem(m, "Rating")) {
        cJSON *img = cJSON_GetObjectItem(rr, "image"), *val = cJSON_GetObjectItem(rr, "value"), *typ = cJSON_GetObjectItem(rr, "type");
        if (!cJSON_IsString(img) || !cJSON_IsNumber(val)) continue;
        bool aud = cJSON_IsString(typ) && !strcmp(typ->valuestring, "audience");
        if (strstr(img->valuestring, "imdb")) it->imdb10 = (int)(val->valuedouble * 10 + 0.5);
        else if (strstr(img->valuestring, "rottentomatoes")) {
            if (aud) it->rt_audience = (int)(val->valuedouble * 10 + 0.5);
            else it->rt_critic = (int)(val->valuedouble * 10 + 0.5);
        }
    }
    cJSON *ri = cJSON_GetObjectItem(m, "ratingImage"), *ari = cJSON_GetObjectItem(m, "audienceRatingImage");
    if (it->rt_critic < 0 && cJSON_IsString(ri) && strstr(ri->valuestring, "rottentomatoes"))
        it->rt_critic = (int)(jnum(m, "rating") * 10 + 0.5);
    if (cJSON_IsString(ari)) {
        if (strstr(ari->valuestring, "imdb") && it->imdb10 < 0) it->imdb10 = (int)(jnum(m, "audienceRating") * 10 + 0.5);
        else if (strstr(ari->valuestring, "rottentomatoes") && it->rt_audience < 0) it->rt_audience = (int)(jnum(m, "audienceRating") * 10 + 0.5);
    }
    bool ep = it->type && !strcmp(it->type, "episode");
    if (ep) {
        char *gp = jstr(m, "grandparentTitle");
        it->subtitle = str_printf(T("%s · S%d E%d"), gp ? gp : "", it->parent_index, it->index);
        it->show = gp;
        it->grandparent_key = jstr(m, "grandparentRatingKey");
        it->parent_key = jstr(m, "parentRatingKey");
        it->thumb = jstr(m, "grandparentThumb");
        it->art = jstr(m, "thumb"); /* afsnits-still (16:9) */
        if (!it->art) it->art = jstr(m, "art");
        it->sart = jstr(m, "grandparentArt");
    } else if (it->type && !strcmp(it->type, "season")) {
        it->subtitle = jstr(m, "parentTitle");
        it->grandparent_key = jstr(m, "parentRatingKey"); /* the series */
        it->thumb = jstr(m, "thumb");
        it->art = jstr(m, "art");
    } else if (it->type && !strcmp(it->type, "show") && jnum(m, "childCount") > 0) { /* "2024 · 2 seasons" */
        int ns = (int)jnum(m, "childCount");
        char sn[64];
        snprintf(sn, sizeof sn, ns == 1 ? T("1 season") : T("%d seasons"), ns);
        it->subtitle = it->year ? str_printf("%s · %s", it->year, sn) : str_dup(sn);
        it->thumb = jstr(m, "thumb");
        it->art = jstr(m, "art");
    } else {
        it->subtitle = str_dup(it->year);
        it->thumb = jstr(m, "thumb");
        it->art = jstr(m, "art");
    }
    cJSON *img;
    cJSON_ArrayForEach(img, cJSON_GetObjectItem(m, "Image")) {
        cJSON *ty = cJSON_GetObjectItem(img, "type");
        if (cJSON_IsString(ty) && !strcmp(ty->valuestring, "clearLogo")) { it->clogo = jstr(img, "url"); break; }
    }
    cJSON *media = cJSON_GetArrayItem(cJSON_GetObjectItem(m, "Media"), 0);
    cJSON *part = cJSON_GetArrayItem(cJSON_GetObjectItem(media, "Part"), 0);
    if (part) {
        it->part = jstr(part, "key");
        it->width = (int)jnum(media, "width");
        it->full = cJSON_GetObjectItem(part, "Stream") != NULL;
        cJSON *st;
        int n_audio = 0, n_emb = 0, n_ext = 0, sel_emb = 0, sel_ext = 0;
        cJSON_ArrayForEach(st, cJSON_GetObjectItem(part, "Stream")) {
            if ((int)jnum(st, "streamType") == 1) {
                cJSON *trc = cJSON_GetObjectItem(st, "colorTrc");
                if (cJSON_IsString(trc) && (!strcmp(trc->valuestring, "smpte2084") || !strcmp(trc->valuestring, "arib-std-b67")))
                    it->hdr = true;
            }
            if ((int)jnum(st, "streamType") == 2) { /* audio: Plex's choice is followed, so the dashboard matches */
                n_audio++;
                if (cJSON_IsTrue(cJSON_GetObjectItem(st, "selected"))) it->aid = n_audio;
            }
            if ((int)jnum(st, "streamType") == 3) {
                char *k = jstr(st, "key");
                bool sel = cJSON_IsTrue(cJSON_GetObjectItem(st, "selected"));
                if (!k) { n_emb++; if (sel) sel_emb = n_emb; }
                else if (sel) sel_ext = n_ext + 1;
                if (k) {
                    char **ns = realloc(it->subs, sizeof(char *) * (it->n_subs + 1));
                    if (!ns) { free(k); if (sel) sel_ext = 0; continue; } /* out of memory: skip this subtitle */
                    it->subs = ns;
                    it->subs[it->n_subs++] = k;
                    n_ext++;
                }
            }
        }
        it->sid = sel_emb ? sel_emb : sel_ext ? n_emb + sel_ext : 0;
    }
}

static void pitem_free(PItem *it) {
    free(it->rating_key); free(it->key); free(it->type); free(it->title); free(it->subtitle);
    free(it->summary); free(it->thumb); free(it->art); free(it->sart); free(it->clogo); free(it->part); free(it->year); free(it->rating);
    free(it->subtype);
    free(it->show);
    free(it->grandparent_key);
    free(it->parent_key);
    for (int i = 0; i < it->n_subs; i++) free(it->subs[i]);
    free(it->subs);
}

static PItem pitem_copy(const PItem *s) {
    PItem d = *s;
    d.rating_key = str_dup(s->rating_key); d.key = str_dup(s->key); d.type = str_dup(s->type);
    d.title = str_dup(s->title); d.subtitle = str_dup(s->subtitle); d.summary = str_dup(s->summary);
    d.thumb = str_dup(s->thumb); d.art = str_dup(s->art); d.sart = str_dup(s->sart); d.clogo = str_dup(s->clogo); d.part = str_dup(s->part);
    d.year = str_dup(s->year); d.rating = str_dup(s->rating); d.subtype = str_dup(s->subtype); d.show = str_dup(s->show);
    d.grandparent_key = str_dup(s->grandparent_key);
    d.parent_key = str_dup(s->parent_key);
    d.subs = calloc(s->n_subs + 1, sizeof(char *));
    for (int i = 0; i < s->n_subs; i++) d.subs[i] = str_dup(s->subs[i]);
    return d;
}

static int parse_items(cJSON *mc, PItem **out, int si) {
    cJSON *arr = cJSON_GetObjectItem(mc, "Metadata");
    if (!arr) arr = cJSON_GetObjectItem(mc, "Directory");
    int n = cJSON_GetArraySize(arr);
    *out = calloc(n + 1, sizeof(PItem));
    int k = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, arr) pitem_parse(&(*out)[k++], m, si);
    return k;
}

static int by_last_viewed(const void *a, const void *b) {
    double x = ((const PItem *)a)->last_viewed, y = ((const PItem *)b)->last_viewed;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* same content: same server+item, or the same movie/episode on another server */
static bool same_content(const PItem *a, const PItem *b) {
    if (a->srv == b->srv && a->rating_key && b->rating_key && !strcmp(a->rating_key, b->rating_key)) return true;
    if (!a->type || !b->type || strcmp(a->type, b->type) || !a->title || !b->title || strcasecmp(a->title, b->title)) return false;
    if (!strcmp(a->type, "episode"))
        return a->subtitle && b->subtitle && !strcasecmp(a->subtitle, b->subtitle);
    return (!a->year && !b->year) || (a->year && b->year && !strcmp(a->year, b->year));
}

/* sort by most recently watched and remove duplicates (the most recently watched is kept); returns the new count */
static int dedupe_sorted(PItem *items, int n) {
    qsort(items, n, sizeof(PItem), by_last_viewed);
    int w = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int k = 0; k < w && !dup; k++) dup = same_content(&items[k], &items[i]);
        if (dup) pitem_free(&items[i]);
        else items[w++] = items[i];
    }
    return w;
}

/* remove items from deselected libraries */
static int drop_hidden(PItem *items, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (lib_hidden(items[i].srv, items[i].section)) pitem_free(&items[i]);
        else items[w++] = items[i];
    }
    return w;
}

/* ---------- asynchronous requests ---------- */
typedef struct Req {
    char *url;
    const char *method;
    cJSON *json;
    long status;
    bool cancelled;
    int srv;
    void (*cb)(void *ud, struct Req *r);
    void *ud;
    char *token;
    int gen;
} Req;

static void req_work(void *arg) {
    Req *r = arg;
    Hdrs H;
    HttpBuf b;
    if (http_request(r->method ? r->method : "GET", r->url, plex_headers(&H, r->token), NULL, &b, 20)) {
        r->status = b.status;
        if (b.data) r->json = cJSON_Parse(b.data);
    }
    http_free(&b);
}

static void req_done(void *arg) {
    Req *r = arg;
    if (r->gen == plex_gen && !r->cancelled && r->cb) r->cb(r->ud, r);
    cJSON_Delete(r->json);
    free(r->url);
    free(r->token);
    free(r);
}

static Req *req_send(const char *method, const char *url, const char *token, void (*cb)(void *, Req *), void *ud) {
    Req *r = calloc(1, sizeof *r);
    r->url = str_dup(url);
    r->method = method;
    r->token = str_dup(token);
    r->cb = cb;
    r->ud = ud;
    r->srv = -1;
    r->gen = plex_gen;
    job_submit(req_work, req_done, r);
    return r;
}

static Req *srv_get(int si, const char *path, void (*cb)(void *, Req *), void *ud) {
    if (!srv_ok(si)) return NULL;
    char *url = str_printf("%s%s", S[si].uri, path);
    Req *r = req_send("GET", url, S[si].token, cb, ud);
    r->srv = si;
    free(url);
    return r;
}

static cJSON *mc_of(Req *r) { return cJSON_GetObjectItem(r->json, "MediaContainer"); }

/* fire-and-forget (timeline/scrobble) */
typedef struct { char *url, *token; } Bg;
static void bg_work(void *arg) {
    Bg *g = arg;
    Hdrs H;
    HttpBuf b;
    http_request("GET", g->url, plex_headers(&H, g->token), NULL, &b, 10);
    http_free(&b);
}
static void bg_done(void *arg) { Bg *g = arg; free(g->url); free(g->token); free(g); }
static void srv_ping(int si, const char *fmt, ...) {
    if (!srv_ok(si)) return;
    va_list ap;
    va_start(ap, fmt);
    char *path = NULL;
    if (vasprintf(&path, fmt, ap) < 0) { va_end(ap); return; }
    va_end(ap);
    Bg *g = calloc(1, sizeof *g);
    g->url = str_printf("%s%s", S[si].uri, path);
    g->token = str_dup(S[si].token);
    free(path);
    job_submit(bg_work, bg_done, g);
}

/* the final timeline/scrobble when playback ends: waits for the server (briefly), so the lists that are refreshed
   straight afterwards already show the new "watched"/"continue" state */
/* (sent in order in a thread; the menu waits at most ~1.5 s for them - a slow server does not freeze the box) */
typedef struct { char *url[2], *token; SDL_AtomicInt done, refs; } EndPing;
static void endping_unref(EndPing *e) {
    if (SDL_AddAtomicInt(&e->refs, -1) != 1) return;
    for (int i = 0; i < 2; i++) free(e->url[i]);
    free(e->token);
    free(e);
}
static int endping_thread(void *arg) {
    EndPing *e = arg;
    for (int i = 0; i < 2 && e->url[i]; i++) {
        Hdrs H;
        HttpBuf b;
        http_request("GET", e->url[i], plex_headers(&H, e->token), NULL, &b, 4);
        http_free(&b);
    }
    SDL_SetAtomicInt(&e->done, 1);
    endping_unref(e);
    return 0;
}
static void srv_ping_end(int si, const char *timeline_path, const char *scrobble_path) {
    if (!srv_ok(si)) return;
    EndPing *e = calloc(1, sizeof *e);
    if (!e) return;
    e->url[0] = str_printf("%s%s", S[si].uri, timeline_path);
    if (scrobble_path) e->url[1] = str_printf("%s%s", S[si].uri, scrobble_path);
    e->token = str_dup(S[si].token);
    SDL_SetAtomicInt(&e->refs, 2);
    SDL_Thread *t = SDL_CreateThread(endping_thread, "plex-end", e);
    if (!t) { endping_unref(e); endping_unref(e); return; }
    SDL_DetachThread(t);
    Uint64 until = SDL_GetTicks() + 1500;
    while (!SDL_GetAtomicInt(&e->done) && SDL_GetTicks() < until) SDL_Delay(20);
    endping_unref(e);
}

/* raised after every playback: the open lists reload themselves (see browse_update) */
static int play_gen;

/* ---------- "Continue" for the NotaTV home screen (all chosen servers) ---------- */
static PItem *deck_items;
static int n_deck_items, deck_pending, deck_gen;
static HomeItem deck[12];
static int n_deck;
static char *deck_strs[64];
static int n_deck_strs;

static void deck_rebuild(void) {
    for (int i = 0; i < n_deck_strs; i++) free(deck_strs[i]);
    n_deck_strs = 0;
    n_deck_items = drop_hidden(deck_items, n_deck_items);
    n_deck_items = dedupe_sorted(deck_items, n_deck_items);
    n_deck = 0;
    for (int i = 0; i < n_deck_items && n_deck < 12; i++) {
        PItem *it = &deck_items[i];
        HomeItem *h = &deck[n_deck++];
        bool ep = it->type && !strcmp(it->type, "episode");
        h->title = deck_strs[n_deck_strs++] = str_dup(ep ? it->subtitle : it->title);
        h->subtitle = deck_strs[n_deck_strs++] = n_enabled() > 1 ? str_printf("%s · %s", ep ? it->title : (it->year ? it->year : ""), S[it->srv].name)
                                                                   : str_dup(ep ? it->title : it->year);
        /* the series'/movie's artwork, not an image from the episode */
        h->image = deck_strs[n_deck_strs++] = img_url(it->srv, it->sart ? it->sart : it->art ? it->art : it->thumb, 1280, 720);
        h->key = deck_strs[n_deck_strs++] = str_printf("plex:%d:%s", it->srv, it->rating_key);
        h->progress = it->duration > 0 ? it->offset / it->duration : 0;
        h->logo = it->clogo ? deck_strs[n_deck_strs++] = logo_url(it->srv, it->clogo) : NULL;
    }
}

static void deck_cb(void *ud, Req *r) {
    if ((intptr_t)ud != deck_gen) return;
    deck_pending--;
    cJSON *mc = mc_of(r);
    if (!mc) return;
    PItem *items;
    int n = parse_items(mc, &items, r->srv);
    deck_items = realloc(deck_items, sizeof(PItem) * (n_deck_items + n + 1));
    memcpy(deck_items + n_deck_items, items, sizeof(PItem) * n);
    n_deck_items += n;
    free(items);
    deck_rebuild();
}

void plex_refresh_ondeck(void) {
    plex_client_id();
    srv_load();
    if (!*cfg_get("plex_token", "") || deck_pending > 0) return;
    for (int i = 0; i < n_deck_items; i++) pitem_free(&deck_items[i]);
    n_deck_items = 0;
    deck_gen++;
    for (int i = 0; i < nS; i++) {
        if (!S[i].enabled) continue;
        if (srv_get(i, "/library/onDeck?X-Plex-Container-Start=0&X-Plex-Container-Size=12&includeImages=1", deck_cb, (void *)(intptr_t)deck_gen))
            deck_pending++;
    }
    if (deck_pending == 0) deck_rebuild();
}

int plex_ondeck(HomeItem **items) {
    *items = deck;
    return *cfg_get("plex_token", "") ? n_deck : 0;
}

/* ---------- watched / unwatched / remove from "Continue watching" ----------
   Chosen with the green key (or a button on the detail and series pages). Works for films, episodes, seasons and
   whole series (Plex marks all the episodes). Afterwards play_gen is bumped, so every open list fetches again. */
enum { WA_SEEN, WA_UNSEEN, WA_DECK, WA_FIRST };
static struct { int srv; char rk[32]; int acts[4], n; void (*first)(void *ud); void *ud; } watch_t;

static void watch_cb(void *ud, Req *r) {
    int a = (int)(intptr_t)ud;
    if (r->status < 200 || r->status >= 300) { app_toast(T("The Plex server did not accept the change")); return; }
    app_toast(a == WA_SEEN ? T("Marked as watched") : a == WA_UNSEEN ? T("Marked as unwatched") : T("Removed from Continue watching"));
    play_gen++;
    plex_refresh_ondeck();
}

static void watch_do(int si, const char *rk, int a) {
    if (!srv_ok(si) || !rk || !*rk) return;
    char *e = url_encode(rk);
    char *url = a == WA_DECK ? str_printf("%s/actions/removeFromContinueWatching?ratingKey=%s", S[si].uri, e)
                             : str_printf("%s/:/%s?identifier=com.plexapp.plugins.library&key=%s", S[si].uri,
                                          a == WA_SEEN ? "scrobble" : "unscrobble", e);
    Req *r = req_send(a == WA_DECK ? "PUT" : "GET", url, S[si].token, watch_cb, (void *)(intptr_t)a);
    r->srv = si;
    free(url);
    free(e);
}

static bool is_group(const PItem *it) { return it->type && (!strcmp(it->type, "show") || !strcmp(it->type, "season")); }
static bool can_mark(const PItem *it) {
    return it->rating_key && srv_ok(it->srv) && it->type &&
           (is_group(it) || !strcmp(it->type, "movie") || !strcmp(it->type, "episode"));
}
/* fully watched (series/season: every episode) */
static bool is_seen(const PItem *it) {
    return is_group(it) ? it->leaves > 0 && it->viewed_leaves >= it->leaves : it->view_count > 0 && it->offset <= 0;
}

static void watch_menu_cb(void *ud, int idx) {
    (void)ud;
    if (idx < 0 || idx >= watch_t.n) return;
    if (watch_t.acts[idx] == WA_FIRST) { if (watch_t.first) watch_t.first(watch_t.ud); }
    else watch_do(watch_t.srv, watch_t.rk, watch_t.acts[idx]);
}

/* the green key's menu for an item. cont = it is shown in "Continue watching" */
/* the item's sheet: first (e.g. "Play") on top when given, then mark watched/unwatched and remove from Continue
   watching. cont = it is shown in "Continue watching". The same rule as everywhere: OK opens it, OK, OK = first. */
static void watch_sheet(const PItem *it, bool cont, const char *first, void (*first_cb)(void *ud), void *ud) {
    const char *items[4];
    int n = 0;
    watch_t.first = first_cb;
    watch_t.ud = ud;
    if (first && first_cb) { items[n] = first; watch_t.acts[n++] = WA_FIRST; }
    if (can_mark(it)) {
        watch_t.srv = it->srv;
        snprintf(watch_t.rk, sizeof watch_t.rk, "%s", it->rating_key);
        bool any = is_group(it) ? it->viewed_leaves > 0 : it->view_count > 0;
        if (!is_seen(it)) { items[n] = T("Mark as watched"); watch_t.acts[n++] = WA_SEEN; }
        if (any) { items[n] = T("Mark as unwatched"); watch_t.acts[n++] = WA_UNSEEN; }
        if (!is_group(it) && (cont || it->offset > 0)) { items[n] = T("Remove from Continue watching"); watch_t.acts[n++] = WA_DECK; }
    }
    watch_t.n = n;
    if (n == 1 && watch_t.acts[0] == WA_FIRST) { first_cb(ud); return; } /* nothing else to choose */
    if (n) choice_open(it->title ? it->title : "Plex", items, n, 0, watch_menu_cb, NULL);
}
/* the green key's menu (only the watched choices) */
static void watch_menu(const PItem *it, bool cont) { watch_sheet(it, cont, NULL, NULL, NULL); }

/* ---------- playback ---------- */
typedef struct {
    int srv;
    char *rating_key;
    double offset;
    PItem *it;
    double credits_start; /* from here the episode counts as watched (start of the final credits), 0 = 90 % */
    PItem next;           /* the next episode (fetched shortly after the start) */
    bool has_next;
} PlayUd;

static bool plex_watched(const PlayUd *p, double pos, double dur) {
    if (dur <= 0) return false;
    if (p->credits_start > 0 && pos >= p->credits_start) return true;
    return pos / dur > 0.9;
}

static void plex_progress(void *ud, double pos, double dur, bool ended, bool paused) {
    PlayUd *p = ud;
    if (!p->rating_key) return;
    pos += p->offset; /* a transcoded stream starts at offset */
    dur += p->offset > 0 ? p->offset : 0;
    if (p->it && p->it->duration > 0) dur = p->it->duration;
    if (ended) {
        char *tl = str_printf("/:/timeline?ratingKey=%s&key=/library/metadata/%s&state=stopped&time=%d&duration=%d",
                              p->rating_key, p->rating_key, (int)(pos * 1000), (int)(dur * 1000));
        char *sc = plex_watched(p, pos, dur) ? str_printf("/:/scrobble?identifier=com.plexapp.plugins.library&key=%s", p->rating_key) : NULL;
        srv_ping_end(p->srv, tl, sc);
        free(tl);
        free(sc);
        return;
    }
    srv_ping(p->srv, "/:/timeline?ratingKey=%s&key=/library/metadata/%s&state=%s&time=%d&duration=%d",
             p->rating_key, p->rating_key, paused ? "paused" : "playing", (int)(pos * 1000), (int)(dur * 1000));
}

static int plex_sub_search(void *ud, char ***labels) {
    PlayUd *p = ud;
    PItem *it = p->it;
    bool ep = it->type && !strcmp(it->type, "episode");
    if (ep) return subs_search(0, it->show, it->parent_index, it->index, labels);
    char q[300];
    snprintf(q, sizeof q, "%s %s", it->title ? it->title : "", it->year ? it->year : "");
    return subs_search(it->tmdb, q, 0, 0, labels);
}
static char *plex_sub_fetch(void *ud, int idx) { (void)ud; return subs_fetch(idx); }

static const char *const plex_qualities[] = {"Original", "1080p", "720p", "480p", NULL};

static bool plex_next_ep(const PItem *it, PItem *out);

/* intro/credits markers (Plex Pass) and the next episode, fetched once playback is running */
static void plex_skip_info(void *ud, PlaySkip *sk) {
    PlayUd *p = ud;
    PItem *it = p->it;
    if (!it->rating_key || !srv_ok(it->srv)) return;
    char *url = str_printf("%s/library/metadata/%s?includeMarkers=1", S[it->srv].uri, it->rating_key);
    Hdrs H;
    HttpBuf b;
    if (http_request("GET", url, plex_headers(&H, S[it->srv].token), NULL, &b, 6) && b.data) {
        cJSON *j = cJSON_Parse(b.data);
        cJSON *mc = j ? cJSON_GetObjectItem(j, "MediaContainer") : NULL;
        cJSON *m0 = mc ? cJSON_GetArrayItem(cJSON_GetObjectItem(mc, "Metadata"), 0) : NULL;
        cJSON *mk;
        cJSON_ArrayForEach(mk, cJSON_GetObjectItem(m0, "Marker")) {
            cJSON *ty = cJSON_GetObjectItem(mk, "type");
            if (!cJSON_IsString(ty) || sk->n >= (int)(sizeof sk->m / sizeof sk->m[0])) continue;
            bool credits = !strcmp(ty->valuestring, "credits");
            if (!credits && strcmp(ty->valuestring, "intro")) continue;
            double a = jnum(mk, "startTimeOffset") / 1000 - p->offset, e = jnum(mk, "endTimeOffset") / 1000 - p->offset;
            if (e <= 0 || e <= a) continue; /* before the point a transcoded stream starts at */
            sk->m[sk->n++] = (PlayMarker){a < 0 ? 0 : a, e, credits};
            if (credits && (p->credits_start <= 0 || a + p->offset > p->credits_start)) p->credits_start = a + p->offset;
        }
        cJSON_Delete(j);
    }
    http_free(&b);
    free(url);
    if (!p->has_next && plex_next_ep(it, &p->next)) p->has_next = true;
    if (p->has_next) {
        sk->has_next = true;
        snprintf(sk->next_title, sizeof sk->next_title, "%s", p->next.subtitle ? p->next.subtitle : "");
        if (p->next.title) {
            size_t l = strlen(sk->next_title);
            snprintf(sk->next_title + l, sizeof sk->next_title - l, "%s%s", l ? " - " : "", p->next.title);
        }
    }
}

/* List responses (the episode list behind "next episode") have no stream info. Fetch the
   full item before playback, so Plex's subtitle and audio choice, the subtitle files and HDR are known. */
typedef struct { char *url, *token; HttpBuf b; bool ok; SDL_AtomicInt done, refs; } FullJob;
static void fulljob_unref(FullJob *f) {
    if (SDL_AddAtomicInt(&f->refs, -1) != 1) return;
    http_free(&f->b);
    free(f->url);
    free(f->token);
    free(f);
}
static int fulljob_thread(void *arg) {
    FullJob *f = arg;
    Hdrs H;
    f->ok = http_request("GET", f->url, plex_headers(&H, f->token), NULL, &f->b, 6) && f->b.data;
    SDL_SetAtomicInt(&f->done, 1);
    fulljob_unref(f);
    return 0;
}
/* false = Back was pressed while waiting */
static bool plex_ensure_full(PItem *it) {
    if (it->full || !it->rating_key || !srv_ok(it->srv)) return true;
    char *url = str_printf("%s/library/metadata/%s", S[it->srv].uri, it->rating_key);
    /* fetched in a thread with the usual spinner ("Loading…" after 0.3 s); Back cancels */
    FullJob *f = calloc(1, sizeof *f);
    if (!f) { free(url); return true; }
    f->url = url;
    f->token = str_dup(S[it->srv].token);
    SDL_SetAtomicInt(&f->refs, 2);
    SDL_Thread *t = SDL_CreateThread(fulljob_thread, "plex-full", f);
    if (!t) { fulljob_unref(f); fulljob_unref(f); return true; }
    SDL_DetachThread(t);
    double t0 = app_time();
    bool cancelled = false;
    while (!SDL_GetAtomicInt(&f->done) && !cancelled) cancelled = app_wait_frame(app_time() - t0 > 0.3 ? T("Loading…") : NULL);
    if (cancelled) { fulljob_unref(f); return false; }
    HttpBuf b = f->b;
    bool ok = f->ok;
    memset(&f->b, 0, sizeof f->b);
    url = NULL;
    fulljob_unref(f);
    if (ok) {
        cJSON *j = cJSON_Parse(b.data);
        cJSON *mc = j ? cJSON_GetObjectItem(j, "MediaContainer") : NULL;
        PItem *items = NULL;
        int n = mc ? parse_items(mc, &items, it->srv) : 0;
        if (n > 0 && items[0].full && items[0].part) {
            PItem f = items[0];
            memset(&items[0], 0, sizeof items[0]);
            /* keep what the list entry had, if the full item lacks it (shown in the menus afterwards) */
            if (!f.sart) { f.sart = it->sart; it->sart = NULL; }
            if (!f.clogo) { f.clogo = it->clogo; it->clogo = NULL; }
            if (!f.show) { f.show = it->show; it->show = NULL; }
            if (!f.grandparent_key) { f.grandparent_key = it->grandparent_key; it->grandparent_key = NULL; }
            pitem_free(it);
            *it = f;
        }
        for (int i = 0; i < n; i++) pitem_free(&items[i]);
        free(items);
        cJSON_Delete(j);
    }
    http_free(&b);
    return true;
}

/* true = played to the end (or "Next episode" was pressed). watched (optional): counted as watched. next (optional):
   receives the next episode when it is known, also when playback was stopped early. */
static bool plex_play(PItem *it, double start, bool *watched, PItem *next) {
    if (!plex_ensure_full(it)) return false; /* Back while it was fetched */
    if (!it->part || !srv_ok(it->srv)) { app_toast(T("No playable file")); return false; }
    bool ended = false;
    PSrv *sv = &S[it->srv];
    PlayUd pu = {it->srv, NULL, 0, it, 0, {0}, false};
    bool is_extra = it->extra_type > 0 || (it->subtype && !strcmp(it->subtype, "trailer"));
    bool ep = it->type && !strcmp(it->type, "episode");
    char *title = ep ? str_dup(it->title) : str_dup(it->title);
    char *sub = ep ? str_dup(it->subtitle) : str_printf(T("%s%s%d min"), it->year ? it->year : "", it->year ? " · " : "", (int)(it->duration / 60));
    char *poster = img_url(it->srv, it->thumb, 400, 600);
    char quality[32];
    snprintf(quality, sizeof quality, "%s", cfg_get("plex_quality", "Original"));
    /* HDR and the TV gets no HDR signal: optionally let the server convert it to SDR (instead of the box's GPU) */
    if (!strcmp(quality, "Original") && it->hdr && !hdr_cfg("hdr", 1) && cfg_int("plex_hdr_server", 0))
        snprintf(quality, sizeof quality, "1080p");
    for (;;) {
        bool direct = !strcmp(quality, "Original");
        char *url;
        char **extra = calloc(it->n_subs + 3, sizeof(char *));
        int ne = 0;
        if (direct) {
            url = str_printf("%s%s?X-Plex-Token=%s", sv->uri, it->part, sv->token);
            if (it->aid > 0) extra[ne++] = str_printf("--aid=%d", it->aid);
            /* subtitles: the track chosen in Plex (otherwise none), or the player's language setting (mpv chooses) */
            const char *sm = cfg_get("plex_subs", "plex");
            if (!is_extra && strcmp(sm, "player")) {
                if (!strcmp(sm, "plex") && it->sid > 0) extra[ne++] = str_printf("--sid=%d", it->sid);
                else extra[ne++] = str_dup("--sid=no");
            }
            for (int i = 0; i < it->n_subs; i++)
                extra[ne++] = str_printf("--sub-file=%s%s?X-Plex-Token=%s", sv->uri, it->subs[i], sv->token);
        } else {
            /* transcoding on the server (HLS) from the start position */
            int h = atoi(quality), w = h * 16 / 9, kbps = h >= 1080 ? 10000 : h >= 720 ? 4000 : 1500;
            char path[96];
            snprintf(path, sizeof path, "/library/metadata/%s", it->rating_key);
            char *ep_ = url_encode(path);
            /* the server's HLS stream carries no subtitle track, so the server burns the subtitle chosen in Plex
               into the picture (also with "Language setting"); none when subtitles are off or none is chosen */
            bool burn = !is_extra && strcmp(cfg_get("plex_subs", "plex"), "off") && it->sid > 0;
            url = str_printf("%s/video/:/transcode/universal/start.m3u8?path=%s&offset=%d&protocol=hls&mediaIndex=0&partIndex=0"
                             "&directPlay=0&directStream=1&fastSeek=1&videoResolution=%dx%d&maxVideoBitrate=%d&subtitles=%s"
                             "&X-Plex-Platform=Chrome&X-Plex-Product=NotaTV&X-Plex-Client-Identifier=%s&session=notatv-%ld&X-Plex-Token=%s",
                             sv->uri, ep_, (int)start, w, h, kbps, burn ? "burn" : "none", plex_client_id(), (long)time(NULL), sv->token);
            free(ep_);
        }
        pu.rating_key = is_extra ? NULL : it->rating_key;
        pu.offset = direct ? 0 : start;
        PlayRequest rq = {0};
        rq.url = url;
        rq.title = title;
        rq.subtitle = sub;
        rq.description = it->summary;
        rq.poster = poster;
        rq.start = direct ? start : 0;
        rq.extra = (const char *const *)extra;
        rq.progress = plex_progress;
        rq.skip_info = is_extra ? NULL : plex_skip_info;
        rq.qualities = is_extra ? NULL : plex_qualities;
        rq.quality = quality;
        rq.sub_search = is_extra ? NULL : plex_sub_search;
        rq.sub_fetch = plex_sub_fetch;
        rq.ud = &pu;
        /* direct play: Plex already knows whether it is HDR (saves an ffprobe); a transcoded stream is SDR */
        if (it->full) rq.media_hdr = direct && it->hdr ? 2 : 1;
        rq.media_w = direct ? it->width : 0;
        PlayResult res = player_run(&rq);
        for (int i = 0; i < ne; i++) free(extra[i]);
        free(extra);
        free(url);
        double pos = res.pos + pu.offset;
        if (res.quality[0] && strcmp(res.quality, quality)) { /* new quality: continue at the same spot */
            snprintf(quality, sizeof quality, "%s", res.quality);
            cfg_set("plex_quality", quality);
            start = pos;
            app_toast(T("Quality: %s"), quality);
            continue;
        }
        if (res.dur > 0 && !is_extra) {
            double dur = it->duration > 0 ? it->duration : res.dur;
            bool w = res.eof || plex_watched(&pu, pos, dur);
            it->offset = w ? 0 : pos;
            if (w) { it->view_count++; if (watched) *watched = true; }
        }
        ended = res.dur > 0 && res.eof; /* a video that never started has not ended (no jump to the next episode) */
        break;
    }
    free(title);
    free(sub);
    free(poster);
    if (next && pu.has_next) { *next = pu.next; pu.has_next = false; }
    if (pu.has_next) pitem_free(&pu.next);
    play_gen++;
    plex_refresh_ondeck();
    return ended;
}

/* the next episode of the show (allLeaves, sorted by season/episode). The result is a full, playable copy
   (Media/Part is included, so it can be passed straight to plex_play). */
static bool plex_next_ep(const PItem *it, PItem *out) {
    if (!it->grandparent_key || !it->rating_key || !it->type || strcmp(it->type, "episode") || !srv_ok(it->srv))
        return false;
    char path[600];
    snprintf(path, sizeof path, "/library/metadata/%s/allLeaves", it->grandparent_key);
    char *url = str_printf("%s%s", S[it->srv].uri, path);
    Hdrs H;
    HttpBuf b;
    bool ok = false;
    if (http_request("GET", url, plex_headers(&H, S[it->srv].token), NULL, &b, 8) && b.data) {
        cJSON *j = cJSON_Parse(b.data);
        cJSON *mc = j ? cJSON_GetObjectItem(j, "MediaContainer") : NULL;
        PItem *items = NULL;
        int n = mc ? parse_items(mc, &items, it->srv) : 0;
        for (int i = 0; i + 1 < n && !ok; i++) {
            if (items[i].rating_key && !strcmp(items[i].rating_key, it->rating_key) && items[i + 1].part) {
                *out = items[i + 1];
                memset(&items[i + 1], 0, sizeof(PItem)); /* taken over by out */
                ok = true;
            }
        }
        for (int i = 0; i < n; i++) pitem_free(&items[i]);
        free(items);
        cJSON_Delete(j);
    }
    http_free(&b);
    free(url);
    return ok;
}

/* ---------- play chain: an episode, then the next one straight away (also from Continue watching and the library) ---------- */
typedef struct { PItem it; PItem *back; } PlexChain;

static void plex_chain_free(PlexChain *c) { pitem_free(&c->it); free(c); }
/* keep the caller's item in step with what was played (so progress and the last episode are shown) */
static void plex_chain_sync(PlexChain *c) {
    if (c->back) {
        pitem_free(c->back);
        *c->back = c->it;
        memset(&c->it, 0, sizeof c->it);
    }
    plex_chain_free(c);
    app_request_redraw();
}

/* plays the episode; when it ends (or "Next episode" is pressed in the player) the next one follows without a pause.
   Stopping early (Back) leaves the menu as it is, but an episode watched to the credits moves "Play" on to the next. */
static void plex_chain_run(PlexChain *c, double start) {
    for (;;) {
        PItem next;
        memset(&next, 0, sizeof next);
        bool watched = false;
        bool ended = plex_play(&c->it, start, &watched, &next);
        if (!next.rating_key) break;
        if (ended || watched) {
            pitem_free(&c->it);
            c->it = next;
            if (!ended) break; /* stopped in the credits: "Play" continues with the next episode */
            start = 0;
            continue;
        }
        pitem_free(&next);
        break;
    }
    plex_chain_sync(c);
}

/* Play an episode and offer the next one when it ends. back (optional) is where the item ends up,
   so the caller's "Play" continues with the next episode if the user declines. */
static void plex_play_chain(PItem *it, double start, PItem *back) {
    PlexChain *c = calloc(1, sizeof *c);
    c->it = pitem_copy(it);
    c->back = back;
    plex_chain_run(c, start);
}

typedef struct { PItem it; PItem *back; } ResumeCtx;
static void resume_choice(void *ud, int idx) {
    ResumeCtx *c = ud;
    if (idx >= 0) plex_play_chain(&c->it, idx == 0 ? c->it.offset : 0, c->back);
    pitem_free(&c->it);
    free(c);
}

static void play_with_resume(PItem *it, PItem *back) {
    if (it->offset > 30 && it->duration > 0) {
        char a[64], d[32];
        snprintf(a, sizeof a, T("Resume from %s"), fmt_duration(it->offset, d, sizeof d));
        const char *items[] = {a, T("Start over")};
        ResumeCtx *c = calloc(1, sizeof *c);
        c->it = pitem_copy(it);
        c->back = back;
        choice_open(it->title ? it->title : T("Play"), items, 2, 0, resume_choice, c);
    } else plex_play_chain(it, 0, back);
}

static void playkey_cb(void *ud, Req *r) {
    (void)ud;
    cJSON *mc = mc_of(r);
    PItem *items;
    int n = mc ? parse_items(mc, &items, r->srv) : 0;
    if (n > 0) play_with_resume(&items[0], NULL); /* the home row refreshes itself afterwards */
    else app_toast(T("Could not load from Plex"));
    for (int i = 0; i < n; i++) pitem_free(&items[i]);
    if (n) free(items);
}

void plex_play_key(const char *key) {
    int si = -1;
    char rk[64];
    if (sscanf(key, "plex:%d:%63s", &si, rk) != 2) return;
    srv_load();
    char *p = str_printf("/library/metadata/%s", rk);
    if (!srv_get(si, p, playkey_cb, NULL)) app_toast(T("The server is no longer selected"));
    free(p);
}

/* which episode "Play" on a series means (eps = allLeaves, in season/episode order):
   - the episode in progress, if it was watched after everything else (an old half-watched episode does not win)
   - otherwise the first unwatched episode after the one watched most recently
   - otherwise the first unwatched, otherwise the first one - specials (season 0) only when there is nothing else */
static int pick_next_ep(const PItem *e, int n) {
    int prog = -1, last = -1;
    for (int i = 0; i < n; i++) {
        if (!e[i].part) continue;
        if (e[i].offset > 30 && (prog < 0 || e[i].last_viewed > e[prog].last_viewed)) prog = i;
        if (e[i].view_count && (last < 0 || e[i].last_viewed >= e[last].last_viewed)) last = i;
    }
    if (prog >= 0 && (last < 0 || e[prog].last_viewed >= e[last].last_viewed)) return prog;
    for (int pass = 0; pass < 2; pass++) { /* pass 0 without specials */
        if (last >= 0)
            for (int i = last + 1; i < n; i++)
                if (e[i].part && !e[i].view_count && (pass || e[i].parent_index > 0)) return i;
        for (int i = 0; i < n; i++)
            if (e[i].part && !e[i].view_count && (pass || e[i].parent_index > 0)) return i;
    }
    for (int i = 0; i < n; i++) if (e[i].part && e[i].parent_index > 0) return i;
    for (int i = 0; i < n; i++) if (e[i].part) return i;
    return -1;
}

/* Play on a show or a season (hero, Play key on a tile): the episode pick_next_ep chooses */
static void showplay_cb(void *ud, Req *r) {
    (void)ud;
    cJSON *mc = mc_of(r);
    PItem *items = NULL;
    int n = mc ? parse_items(mc, &items, r->srv) : 0;
    int pick = pick_next_ep(items, n);
    double start = pick >= 0 && items[pick].offset > 30 ? items[pick].offset : 0;
    if (pick >= 0) plex_play_chain(&items[pick], start, NULL);
    else app_toast(T("No playable file"));
    for (int i = 0; i < n; i++) pitem_free(&items[i]);
    free(items);
}
static void play_show(PItem *h) {
    if (!h->rating_key || !srv_ok(h->srv)) return;
    char *p = str_printf("/library/metadata/%s/allLeaves", h->rating_key);
    if (!srv_get(h->srv, p, showplay_cb, NULL)) app_toast(T("The server is no longer selected"));
    free(p);
}

/* ================= serverfund ================= */
typedef struct { char *name, *token, *id, *owner, *uris[12]; int n_uri; bool owned; } Res;
/* Plex servers found on the local network (netdisc): their address is tried first for the account's servers */
static DiscServer plex_local[16];
static int n_plex_local;

typedef struct {
    Res r[MAX_SRV];
    int n;
    DiscServer local[16];
    int n_local;
    char want_id[96]; /* only test this one (when ticked) */
    bool auto_pick;   /* first login: choose your own servers */
    char *found_uri[MAX_SRV];
    bool ok;
    int gen;
} DiscJob;

static DiscJob *disc_new(void) {
    DiscJob *j = calloc(1, sizeof *j);
    memcpy(j->local, plex_local, sizeof plex_local);
    j->n_local = n_plex_local;
    j->gen = plex_gen;
    return j;
}

static void disc_free(DiscJob *j) {
    for (int i = 0; i < j->n; i++) {
        free(j->r[i].name); free(j->r[i].token); free(j->r[i].id); free(j->r[i].owner);
        for (int u = 0; u < j->r[i].n_uri; u++) free(j->r[i].uris[u]);
        free(j->found_uri[i]);
    }
    free(j);
}

static char *test_uris(Res *r) {
    Hdrs H;
    HttpBuf b;
    for (int u = 0; u < r->n_uri; u++) {
        char *url = str_printf("%s/identity", r->uris[u]);
        bool ok = http_request("GET", url, plex_headers(&H, r->token), NULL, &b, 5) && b.status == 200;
        http_free(&b);
        free(url);
        if (ok) return str_dup(r->uris[u]);
    }
    return NULL;
}

static void disc_work(void *arg) {
    DiscJob *j = arg;
    Hdrs H;
    HttpBuf b;
    if (!http_request("GET", "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1", plex_headers(&H, cfg_get("plex_token", "")), NULL, &b, 20) || !b.data) {
        http_free(&b);
        return;
    }
    cJSON *arr = cJSON_Parse(b.data);
    http_free(&b);
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        char *prov = jstr(it, "provides");
        bool is_srv = prov && strstr(prov, "server");
        free(prov);
        if (!is_srv || j->n >= MAX_SRV) continue;
        Res *s = &j->r[j->n++];
        s->name = jstr(it, "name");
        s->token = jstr(it, "accessToken");
        s->id = jstr(it, "clientIdentifier");
        s->owner = jstr(it, "sourceTitle");
        s->owned = cJSON_IsTrue(cJSON_GetObjectItem(it, "owned"));
        for (int pass = 0; pass < 3; pass++) { /* lokal, remote, relay */
            cJSON *c;
            cJSON_ArrayForEach(c, cJSON_GetObjectItem(it, "connections")) {
                bool local = cJSON_IsTrue(cJSON_GetObjectItem(c, "local"));
                bool relay = cJSON_IsTrue(cJSON_GetObjectItem(c, "relay"));
                int want = local ? 0 : relay ? 2 : 1;
                if (want != pass || s->n_uri >= 12) continue;
                s->uris[s->n_uri++] = jstr(c, "uri");
            }
        }
    }
    cJSON_Delete(arr);
    j->ok = true;
    /* locally found addresses first (same machine id as the account's server) */
    for (int i = 0; i < j->n; i++)
        for (int k = 0; k < j->n_local; k++) {
            Res *r = &j->r[i];
            if (!r->id || strcmp(r->id, j->local[k].id)) continue;
            if (r->n_uri == 12) free(r->uris[--r->n_uri]);
            memmove(&r->uris[1], &r->uris[0], sizeof r->uris[0] * (size_t)r->n_uri);
            r->uris[0] = str_dup(j->local[k].url);
            r->n_uri++;
        }
    /* test connections: the wanted server, or on first login all your own (otherwise the first shared one) */
    bool any = false;
    for (int i = 0; i < j->n; i++) {
        Res *r = &j->r[i];
        bool test = j->want_id[0] ? (r->id && !strcmp(r->id, j->want_id)) : (j->auto_pick && r->owned);
        if (test) { j->found_uri[i] = test_uris(r); any |= j->found_uri[i] != NULL; }
    }
    if (j->auto_pick && !any)
        for (int i = 0; i < j->n && !any; i++) { j->found_uri[i] = test_uris(&j->r[i]); any = j->found_uri[i] != NULL; }
}

/* merge found servers into S[] (keeps the ticks) */
static void disc_merge(DiscJob *j) {
    for (int i = 0; i < j->n; i++) {
        Res *r = &j->r[i];
        if (!r->id) continue;
        int k;
        for (k = 0; k < nS; k++) if (!strcmp(S[k].id, r->id)) break;
        if (k == nS) {
            if (nS >= MAX_SRV) continue;
            memset(&S[nS], 0, sizeof S[nS]);
            snprintf(S[nS].id, sizeof S[nS].id, "%s", r->id);
            nS++;
        }
        PSrv *s = &S[k];
        snprintf(s->name, sizeof s->name, "%s", r->name ? r->name : "Plex");
        snprintf(s->token, sizeof s->token, "%s", r->token ? r->token : cfg_get("plex_token", ""));
        snprintf(s->owner, sizeof s->owner, "%s", r->owner ? r->owner : "");
        s->owned = r->owned;
        if (j->found_uri[i]) {
            snprintf(s->uri, sizeof s->uri, "%s", j->found_uri[i]);
            if (j->auto_pick || j->want_id[0]) s->enabled = true;
        }
    }
    srv_save();
}

/* ================= screens ================= */
static Screen *browse_screen(const char *title, const char *path, bool grid, int srv);
static Screen *detail_screen(PItem *it);
static Screen *servers_screen(bool first_time);
static Screen *libsel_screen(int srv, bool *changed);

/* ---- Link (PIN) ---- */
typedef struct {
    char code[16];
    long pin_id;
    double last_poll;
    char status[200];
    int step; /* 0 pin, 1 finding servers, 2 done */
    Req *req;
    int btn;
    bool searching;
    char found[300]; /* result of the network search */
} Link;

static Link *live_link;

static void link_disc_done(void *arg) {
    DiscJob *j = arg;
    if (j->gen != plex_gen) { disc_free(j); return; } /* signed out */
    Link *l = live_link;
    if (j->ok) disc_merge(j);
    if (l) {
        if (n_enabled() > 0) l->step = 2;
        else if (!j->ok) snprintf(l->status, sizeof l->status, T("Cannot get servers from plex.tv - check the network"));
        else if (j->n == 0) snprintf(l->status, sizeof l->status, T("No Plex servers on the account"));
        else snprintf(l->status, sizeof l->status, T("Found %d server(s), but none responded. Are they turned on?"), j->n);
    }
    disc_free(j);
}

static void start_discovery(void) {
    DiscJob *j = disc_new();
    j->auto_pick = true;
    job_submit(disc_work, link_disc_done, j);
}

static void pin_cb(void *ud, Req *r) {
    Link *l = live_link;
    (void)ud;
    if (!l) return;
    l->req = NULL;
    if (!r->json) { snprintf(l->status, sizeof l->status, T("Cannot reach plex.tv - check the network")); return; }
    cJSON *code = cJSON_GetObjectItem(r->json, "code"), *id = cJSON_GetObjectItem(r->json, "id");
    cJSON *tok = cJSON_GetObjectItem(r->json, "authToken");
    if (cJSON_IsString(code) && !l->code[0]) snprintf(l->code, sizeof l->code, "%s", code->valuestring);
    if (cJSON_IsNumber(id)) l->pin_id = (long)id->valuedouble;
    if (cJSON_IsString(tok) && tok->valuestring[0]) {
        cfg_set("plex_token", tok->valuestring);
        l->step = 1;
        snprintf(l->status, sizeof l->status, T("Connected! Finding your servers…"));
        start_discovery();
    }
}

static void link_update(Screen *s, float dt) {
    (void)dt;
    Link *l = s->st;
    if (l->step == 0 && l->pin_id && !l->req && app_time() - l->last_poll > 2) {
        l->last_poll = app_time();
        char *u = str_printf("https://plex.tv/api/v2/pins/%ld", l->pin_id);
        l->req = req_send("GET", u, NULL, pin_cb, NULL);
        free(u);
    }
    if (l->step == 2) { /* replace s itself, also when e.g. the server list is on top */
        if (app_replace) { l->step = 3; app_replace(s, plex_screen()); }
        else if (app_top() == s) { l->step = 3; app_pop(); app_push(plex_screen()); } /* NotaTV 1.0.3 and older */
    }
}

static int link_buttons(Link *l, const char **b) {
    int n = 0;
    if (l->step == 1 && l->status[0]) b[n++] = T("Try again");
    b[n++] = T("Find servers on the network");
    return n;
}

static void link_found(void *ud, const DiscServer *srv, int n) {
    Link *l = live_link;
    (void)ud;
    if (!l) return;
    l->searching = false;
    n_plex_local = n < 16 ? n : 16;
    for (int i = 0; i < n_plex_local; i++) plex_local[i] = srv[i];
    if (!n) { snprintf(l->found, sizeof l->found, T("No Plex servers found on the network")); return; }
    char list[240] = "";
    for (int i = 0; i < n_plex_local; i++) {
        char one[200];
        snprintf(one, sizeof one, "%s%s (%s)", i ? ", " : "", srv[i].name, srv[i].url + 7);
        strncat(list, one, sizeof list - strlen(list) - 1);
    }
    if (l->step == 0) snprintf(l->found, sizeof l->found, T("Found: %s - sign in with the code to use it"), list);
    else {
        snprintf(l->found, sizeof l->found, T("Found: %s - trying to connect…"), list);
        l->status[0] = 0;
        start_discovery(); /* try the account's servers again with the local addresses first */
    }
}

static void link_draw(Screen *s) {
    Link *l = s->st;
    draw_background();
    draw_topbar("Plex");
    draw_icon(ICON_PLEX, UI_W / 2, 260, 110, RGB(229, 160, 13));
    if (l->step == 0) {
        text_draw(F_BOLD, 44, UI_W / 2, 360, C_TEXT, AL_CENTER, T("Link your Plex account"));
        text_draw(F_REG, 30, UI_W / 2, 430, C_TEXT2, AL_CENTER, T("Go to  plex.tv/link  on your phone or computer and enter the code:"));
        if (l->code[0]) {
            float w = 620;
            fill_rrect(UI_W / 2 - w / 2, 510, w, 170, 28, C_SURFACE2);
            char spaced[40] = "";
            for (int i = 0; l->code[i]; i++) { size_t n = strlen(spaced); spaced[n] = l->code[i]; spaced[n + 1] = ' '; spaced[n + 2] = 0; }
            text_draw(F_BOLD, 100, UI_W / 2, 535, C_TEXT, AL_CENTER, spaced);
        } else draw_spinner(UI_W / 2, 590, 30, C_TEXT);
    } else {
        text_draw(F_BOLD, 44, UI_W / 2, 360, C_TEXT, AL_CENTER, T("Connecting to Plex"));
        draw_spinner(UI_W / 2, 500, 30, C_TEXT);
    }
    text_draw(F_REG, 26, UI_W / 2, 740, C_TEXT3, AL_CENTER, l->status);
    if (l->found[0]) text_draw(F_REG, 24, UI_W / 2, 790, C_TEXT2, AL_CENTER, l->found);
    const char *b[2];
    int nb = link_buttons(l, b);
    if (l->searching) draw_spinner(UI_W / 2, 900, 24, C_TEXT);
    else draw_buttons(UI_W / 2, 860, b, nb, l->btn);
}

static void link_leave(Screen *s) {
    Link *l = s->st;
    if (l->req) l->req->cancelled = true;
    live_link = NULL;
}

static bool link_input(Screen *s, const InputEvent *ev) {
    Link *l = s->st;
    const char *b[2];
    int nb = link_buttons(l, b);
    if (buttons_nav(&l->btn, nb, ev)) return true;
    if (ev->a == A_OK && !ev->repeat && !l->searching) {
        if (!strcmp(b[l->btn], T("Try again"))) { l->status[0] = 0; start_discovery(); }
        else { l->searching = true; l->found[0] = 0; netdisc_start(DISC_PLEX, link_found, NULL); }
        l->btn = 0;
        return true;
    }
    if (ev->a == A_MENU && *cfg_get("plex_token", "")) { app_push(servers_screen(false)); return true; }
    return false;
}

static Screen *link_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    Link *l = calloc(1, sizeof *l);
    s->name = "plex-link";
    s->st = l;
    s->update = link_update;
    s->draw = link_draw;
    s->leave = link_leave;
    s->input = link_input;
    live_link = l;
    if (*cfg_get("plex_token", "")) {
        l->step = 1;
        snprintf(l->status, sizeof l->status, T("Finding your servers…"));
        start_discovery();
    } else {
        l->req = req_send("POST", "https://plex.tv/api/v2/pins?strong=false", NULL, pin_cb, NULL);
        l->last_poll = app_time();
    }
    return s;
}

/* ---- Servers (checkboxes) ---- */
typedef struct {
    int sel;
    bool loading, changed;
    char testing[96];
    char msg[160];
} Servers;
static Servers *live_servers;

static void srvlist_done(void *arg) {
    DiscJob *j = arg;
    if (j->gen != plex_gen) { disc_free(j); return; } /* signed out */
    Servers *v = live_servers;
    if (j->ok) disc_merge(j);
    if (v) {
        v->loading = false;
        if (!j->ok) snprintf(v->msg, sizeof v->msg, T("Could not get the server list from plex.tv"));
    }
    disc_free(j);
}

static void toggle_done(void *arg) {
    DiscJob *j = arg;
    if (j->gen != plex_gen) { disc_free(j); return; } /* signed out */
    Servers *v = live_servers;
    bool found = false;
    for (int i = 0; i < j->n; i++) found |= j->found_uri[i] != NULL;
    if (j->ok) disc_merge(j);
    if (v) {
        v->testing[0] = 0;
        if (!found) snprintf(v->msg, sizeof v->msg, T("The server is not responding - it cannot be selected right now"));
        else { v->msg[0] = 0; v->changed = true; }
    }
    disc_free(j);
}

static void servers_enter(Screen *s) {
    Servers *v = s->st;
    live_servers = v;
    v->loading = true;
    DiscJob *j = disc_new();
    job_submit(disc_work, srvlist_done, j);
}

static void servers_leave(Screen *s) {
    Servers *v = s->st;
    live_servers = NULL;
    if (v->changed) plex_refresh_ondeck();
}

static void servers_draw(Screen *s) {
    Servers *v = s->st;
    draw_background();
    draw_topbar(T("Plex servers"));
    text_draw(F_REG, 26, 120, 130, C_TEXT2, AL_LEFT, T("Tick the servers that should be shown in Plex and under Continue."));
    if (nS == 0) {
        if (v->loading) draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT);
        else text_draw(F_REG, 28, UI_W / 2, UI_H / 2, C_TEXT2, AL_CENTER, T("No servers on the account"));
        return;
    }
    for (int i = 0; i < nS; i++) {
        PSrv *sv = &S[i];
        float y = 200 + i * 104;
        bool f = v->sel == i;
        fill_rrect(120, y, UI_W - 240, 90, 18, f ? C_FOCUS : C_SURFACE);
        Color tc = f ? RGB(10, 10, 10) : C_TEXT, sc = f ? RGB(70, 70, 75) : C_TEXT3;
        /* afkrydsningsboks */
        float bx = 160, by = y + 25;
        if (sv->enabled) {
            fill_rrect(bx, by, 40, 40, 10, RGB(229, 160, 13));
            draw_icon(ICON_CHECK, bx + 20, by + 20, 28, RGB(20, 20, 20));
        } else stroke_rrect(bx, by, 40, 40, 10, 3, f ? RGB(60, 60, 60) : C_TEXT3);
        text_draw_clip(F_SEMI, 30, 230, y + 14, 1000, tc, AL_LEFT, sv->name);
        char sub[256];
        if (sv->owned) snprintf(sub, sizeof sub, "%s", T("Your server"));
        else snprintf(sub, sizeof sub, T("Shared by %s"), sv->owner[0] ? sv->owner : T("another user"));
        text_draw_clip(F_REG, 22, 230, y + 54, 1000, sc, AL_LEFT, sub);
        if (v->testing[0] && !strcmp(v->testing, sv->id)) draw_spinner(UI_W - 200, y + 45, 16, tc);
        else if (sv->enabled && !sv->uri[0]) text_draw(F_REG, 22, UI_W - 170, y + 32, C_DANGER, AL_RIGHT, "Offline");
        else if (sv->enabled) {
            text_draw(F_REG, 22, UI_W - 200, y + 32, sc, AL_RIGHT, sv->hidden[0] ? T("Selected libraries") : T("All libraries"));
            fill_tri(UI_W - 176, y + 34, UI_W - 164, y + 44, UI_W - 176, y + 54, sc);
        }
    }
    if (v->msg[0]) text_draw(F_REG, 24, UI_W / 2, UI_H - 140, C_DANGER, AL_CENTER, v->msg);
    draw_hint(120, UI_H - 70, "OK", T("Select / deselect"));
    draw_hint(420, UI_H - 70, T("Right"), T("Choose libraries"));
    draw_hint(760, UI_H - 70, T("Back"), T("Save"));
}

static bool servers_input(Screen *s, const InputEvent *ev) {
    Servers *v = s->st;
    switch (ev->a) {
    case A_UP: if (v->sel > 0) v->sel--; return true;
    case A_DOWN: if (v->sel < nS - 1) v->sel++; return true;
    case A_OK: {
        if (v->sel >= nS || v->testing[0]) return true;
        PSrv *sv = &S[v->sel];
        if (sv->enabled) {
            if (n_enabled() <= 1) { snprintf(v->msg, sizeof v->msg, T("At least one server must be selected")); return true; }
            sv->enabled = false;
            v->changed = true;
            srv_save();
        } else {
            /* find an address that works before the server is chosen */
            snprintf(v->testing, sizeof v->testing, "%s", sv->id);
            DiscJob *j = disc_new();
            snprintf(j->want_id, sizeof j->want_id, "%s", sv->id);
            job_submit(disc_work, toggle_done, j);
        }
        return true;
    }
    case A_RIGHT:
        if (v->sel < nS && S[v->sel].enabled && S[v->sel].uri[0]) app_push(libsel_screen(v->sel, &v->changed));
        else snprintf(v->msg, sizeof v->msg, T("Select the server first to choose its libraries"));
        return true;
    case A_BACK: {
        bool changed = v->changed;
        app_pop();
        if (changed) { app_home(); app_push(plex_screen()); app_toast(T("Servers updated")); }
        return true;
    }
    default: return false;
    }
}

/* ---- libraries on one server (checkboxes) ---- */
typedef struct { int srv, sel; PItem *libs; int n; bool loading; Req *req; bool *changed; } Libs;

static void libsel_cb(void *ud, Req *r) {
    Libs *l = ud;
    l->req = NULL;
    l->loading = false;
    cJSON *mc = mc_of(r);
    if (!mc) return;
    cJSON *arr = cJSON_GetObjectItem(mc, "Directory"), *d;
    l->libs = calloc(cJSON_GetArraySize(arr) + 1, sizeof(PItem));
    cJSON_ArrayForEach(d, arr) pitem_parse(&l->libs[l->n++], d, r->srv);
}

static void libsel_enter(Screen *s) {
    Libs *l = s->st;
    l->loading = true;
    l->req = srv_get(l->srv, "/library/sections", libsel_cb, l);
    if (!l->req) l->loading = false;
}
static void libsel_leave(Screen *s) { Libs *l = s->st; if (l->req) l->req->cancelled = true; }
static void libsel_destroy(Screen *s) {
    Libs *l = s->st;
    for (int i = 0; i < l->n; i++) pitem_free(&l->libs[i]);
    free(l->libs);
    free(l);
    free(s);
}

static void libsel_draw(Screen *s) {
    Libs *l = s->st;
    draw_background();
    char t[200];
    snprintf(t, sizeof t, T("Libraries · %s"), S[l->srv].name);
    draw_topbar(t);
    text_draw(F_REG, 26, 120, 130, C_TEXT2, AL_LEFT, T("Tick the libraries that should be shown in Plex."));
    if (l->loading) { draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT); return; }
    if (l->n == 0) { text_draw(F_REG, 28, UI_W / 2, UI_H / 2, C_TEXT2, AL_CENTER, T("No libraries (or the server is not responding)")); return; }
    for (int i = 0; i < l->n; i++) {
        float y = 200 + i * 96;
        bool f = l->sel == i, on = !lib_hidden(l->srv, atoi(l->libs[i].key ? l->libs[i].key : "0"));
        fill_rrect(120, y, UI_W - 240, 82, 18, f ? C_FOCUS : C_SURFACE);
        if (on) { fill_rrect(160, y + 21, 40, 40, 10, RGB(229, 160, 13)); draw_icon(ICON_CHECK, 180, y + 41, 28, RGB(20, 20, 20)); }
        else stroke_rrect(160, y + 21, 40, 40, 10, 3, f ? RGB(60, 60, 60) : C_TEXT3);
        PItem *b = &l->libs[i];
        IconId ic = b->type && !strcmp(b->type, "artist") ? ICON_MUSIC : b->type && !strcmp(b->type, "photo") ? ICON_FOLDER : ICON_FILM;
        draw_icon(ic, 250, y + 41, 30, f ? RGB(10, 10, 10) : C_TEXT2);
        text_draw(F_SEMI, 30, 290, y + 22, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, b->title ? b->title : T("Library"));
    }
}

static bool libsel_input(Screen *s, const InputEvent *ev) {
    Libs *l = s->st;
    switch (ev->a) {
    case A_UP: if (l->sel > 0) l->sel--; return true;
    case A_DOWN: if (l->sel < l->n - 1) l->sel++; return true;
    case A_OK:
        if (l->sel < l->n && l->libs[l->sel].key) {
            int sec = atoi(l->libs[l->sel].key);
            lib_set_hidden(l->srv, sec, !lib_hidden(l->srv, sec));
            if (l->changed) *l->changed = true;
        }
        return true;
    case A_LEFT: app_pop(); return true;
    default: return false;
    }
}

static Screen *libsel_screen(int srv, bool *changed) {
    Screen *s = calloc(1, sizeof *s);
    Libs *l = calloc(1, sizeof *l);
    l->srv = srv;
    l->changed = changed;
    s->name = "plex-libs";
    s->st = l;
    s->enter = libsel_enter;
    s->leave = libsel_leave;
    s->draw = libsel_draw;
    s->input = libsel_input;
    s->destroy = libsel_destroy;
    return s;
}

static Screen *servers_screen(bool first_time) {
    (void)first_time;
    Screen *s = calloc(1, sizeof *s);
    s->name = "plex-servers";
    s->st = calloc(1, sizeof(Servers));
    s->enter = servers_enter;
    s->leave = servers_leave;
    s->draw = servers_draw;
    s->input = servers_input;
    return s;
}

/* ---- browse (home / library / seasons / search) ---- */
typedef struct {
    char *title;
    PItem *items;
    int n;
    bool cont; /* "Continue watching" (gathered across servers) */
} Row;

#define MAX_REQS 40
typedef struct {
    char title[200], path[512];
    bool grid, is_home, failed;
    int srv;      /* -1 = all chosen servers */
    int pending;  /* requests in progress */
    Row *rows;
    int n_rows;
    PItem *libs;  /* home only */
    int n_libs;
    int row, col;
    float scroll_y, *scroll_x;
    Req *reqs[MAX_REQS];
    int n_reqs;
    SideMenu side; /* home only */
    float x0;
    /* the home screen's hero: a featured title at the top (cfg plex_home_style = "hero") */
    bool hero;         /* focus is on the hero */
    int hero_idx;
    double hero_t;     /* last switched (automatic rotation) */
    float hero_fade;   /* 0..1 for crossfade on change */
    char foc_key[64];  /* "focus" style: the title in focus and the previous backdrop (crossfade) */
    char *foc_art, *prev_art;
    Layer *bgl;        /* backdrop + gradients, only redrawn when they change */
    char bgl_key[80];
    float bgl_fade;
    /* search: all servers' results are gathered and ranked (best match first) */
    char *query;
    PItem *sres;
    int *sscore, n_sres;
    unsigned char *sgrp; /* index in SGROUPS; SG_TITLE set = the title matches the search */
    int gen;             /* play_gen when the rows were fetched: after a playback the rows are fetched again */
    bool restore;        /* after a reload: put the focus back on want_row/want_col */
    int want_row, want_col;
} Browse;

#define HERO_H 600
static bool hero_on(const Browse *b) { return b->is_home && !b->grid && !strcmp(cfg_get("plex_home_style", "hero"), "hero"); }
/* "focus": the backdrop and info at the top follow the title you are on */
static bool focus_on(const Browse *b) { return b->is_home && !b->grid && !strcmp(cfg_get("plex_home_style", "hero"), "focus"); }
#define FOCUS_H 345
#define BG_H 560 /* the backdrop's height in focus style (fades out completely before the rows) */

/* backdrop for hero/focus (episodes: the series') */
static char *bg_url(const PItem *h) {
    const char *art = h->sart ? h->sart : h->art ? h->art : h->thumb;
    return img_url(h->srv, art, 1280, 720);
}
/* fetch the backdrop (and title logo) in advance, so they are ready when focus moves there */
static void prefetch_bg(const PItem *h) {
    char *u = bg_url(h);
    img_get(u);
    free(u);
    if (h->clogo) { char *l = logo_url(h->srv, h->clogo); img_get(l); free(l); }
}

/* title logo/title, metadata, ratings and overview - shared by hero and focus. Returns the bottom */
static float draw_info(PItem *h, float hx, float hy, float a, int lines, bool compact) {
    bool ep = h->type && !strcmp(h->type, "episode");
    char *lu = h->clogo ? logo_url(h->srv, h->clogo) : NULL;
    const char *name = ep && h->show ? h->show : h->title;
    float lh = compact ? 110 : 170, my = compact ? 128 : 196; /* logo height and metadata line */
    if (!lu || !draw_title_logo(lu, hx, hy, compact ? 480 : 640, lh, a))
        text_draw_wrap(F_BOLD, compact ? 56 : 64, hx, hy + (compact ? 36 : 60), 1000, compact ? 1 : 2, 72, color_alpha(C_TEXT, a), name ? name : "");
    free(lu);
    char meta[300] = "";
    int dm = (int)(h->duration / 60);
    if (ep) snprintf(meta, sizeof meta, T("S%d E%d  ·  %s"), h->parent_index, h->index, h->title ? h->title : "");
    else snprintf(meta, sizeof meta, "%s%s%s", h->year ? h->year : "", h->rating ? "  ·  " : "", h->rating ? h->rating : "");
    if (dm >= 60) snprintf(meta + strlen(meta), sizeof meta - strlen(meta), T("  ·  %dh %02dm"), dm / 60, dm % 60);
    else if (dm > 0) snprintf(meta + strlen(meta), sizeof meta - strlen(meta), T("  ·  %dm"), dm);
    if (h->leaves > 0) snprintf(meta + strlen(meta), sizeof meta - strlen(meta), T("  ·  %d episodes"), h->leaves);
    text_draw_clip(F_SEMI, 27, hx, hy + my, 1100, color_alpha(C_TEXT2, a), AL_LEFT, meta);
    float mw = text_width(F_SEMI, 27, meta);
    if (mw < 1100) draw_ratings(hx + mw + 30, hy + my - 4, h->rt_critic, h->rt_audience, h->imdb10);
    float sy = hy + my + (compact ? 46 : 52), sl = compact ? 37 : 40;
    int n = text_draw_wrap(F_REG, compact ? 27 : 29, hx, sy, 1050, lines, sl, color_alpha(C_TEXT, a * 0.9f), h->summary ? h->summary : "");
    return sy + n * sl;
}

/* hero candidates: the first movies/series with a backdrop from the rows (not "continue") */
static int hero_items(Browse *b, PItem **out, int max) {
    int n = 0;
    for (int pass = 0; pass < 3 && n < max; pass++)
        for (int r = 0; r < b->n_rows && n < max; r++) {
            Row *row = &b->rows[r];
            if (row->cont || pass >= row->n) continue;
            PItem *it = &row->items[pass];
            if (!it->art || !it->type || (strcmp(it->type, "movie") && strcmp(it->type, "show"))) continue;
            bool dup = false;
            for (int k = 0; k < n && !dup; k++) dup = out[k]->rating_key && it->rating_key && !strcmp(out[k]->rating_key, it->rating_key);
            if (!dup) out[n++] = it;
        }
    return n;
}

static void build_side(Browse *b);

static void track(Browse *b, Req *r) {
    if (!r) return;
    b->pending++;
    if (b->n_reqs < MAX_REQS) b->reqs[b->n_reqs++] = r;
}
static void untrack(Browse *b, Req *r) {
    b->pending--;
    for (int i = 0; i < b->n_reqs; i++) if (b->reqs[i] == r) b->reqs[i] = NULL;
}

static void browse_free_rows(Browse *b) {
    for (int r = 0; r < b->n_rows; r++) {
        for (int i = 0; i < b->rows[r].n; i++) pitem_free(&b->rows[r].items[i]);
        free(b->rows[r].items);
        free(b->rows[r].title);
    }
    free(b->rows);
    b->rows = NULL;
    b->n_rows = 0;
    free(b->scroll_x);
    b->scroll_x = NULL;
}

static void add_row(Browse *b, const char *title, PItem *items, int n, bool front) {
    if (n <= 0) { free(items); return; }
    b->rows = realloc(b->rows, sizeof(Row) * (b->n_rows + 1));
    b->scroll_x = realloc(b->scroll_x, sizeof(float) * (b->n_rows + 1));
    Row nr = {str_dup(title), items, n, false};
    if (front) {
        memmove(b->rows + 1, b->rows, sizeof(Row) * b->n_rows);
        memmove(b->scroll_x + 1, b->scroll_x, sizeof(float) * b->n_rows);
        b->rows[0] = nr;
        b->scroll_x[0] = 0;
        if (b->n_rows > 0) b->row++; /* keep focus on the same row */
    } else {
        b->rows[b->n_rows] = nr;
        b->scroll_x[b->n_rows] = 0;
    }
    b->n_rows++;
}

static char *row_title(Browse *b, const char *t, int si) {
    (void)b;
    if (n_enabled() > 1 && srv_ok(si)) return str_printf("%s · %s", t ? t : "", S[si].name);
    return str_dup(t ? t : "");
}

/* merge "Continue" items from a server into row 0 */
static void merge_continue(Browse *b, PItem *items, int n) {
    if (n <= 0) { free(items); return; }
    Row *cr = b->n_rows && b->rows[0].cont ? &b->rows[0] : NULL;
    if (!cr) {
        add_row(b, T("Continue watching"), items, n, true);
        b->rows[0].cont = true;
        cr = &b->rows[0];
    } else {
        cr->items = realloc(cr->items, sizeof(PItem) * (cr->n + n));
        memcpy(cr->items + cr->n, items, sizeof(PItem) * n);
        cr->n += n;
        free(items);
    }
    cr->n = drop_hidden(cr->items, cr->n);
    cr->n = dedupe_sorted(cr->items, cr->n);
}

static bool is_continue_hub(cJSON *hub) {
    cJSON *id = cJSON_GetObjectItem(hub, "hubIdentifier");
    if (!cJSON_IsString(id)) return false;
    const char *s = id->valuestring;
    return strcasestr(s, "continue") || strcasestr(s, "ondeck");
}

static void hubs_cb(void *ud, Req *r) {
    Browse *b = ud;
    untrack(b, r);
    cJSON *mc = mc_of(r);
    if (!mc) { if (b->n_rows == 0 && b->pending == 0) b->failed = true; return; }
    cJSON *hub;
    cJSON_ArrayForEach(hub, cJSON_GetObjectItem(mc, "Hub")) {
        cJSON *sec = cJSON_GetObjectItem(hub, "librarySectionID");
        if (cJSON_IsNumber(sec) && lib_hidden(r->srv, sec->valueint)) continue;
        PItem *items;
        int n = parse_items(hub, &items, r->srv);
        if (b->is_home && is_continue_hub(hub)) { merge_continue(b, items, n); continue; }
        if (b->is_home) n = drop_hidden(items, n);
        char *t = jstr(hub, "title");
        char *rt = b->srv < 0 ? row_title(b, t, r->srv) : str_dup(t);
        add_row(b, rt, items, n, false);
        free(t);
        free(rt);
    }
}

static void libs_cb(void *ud, Req *r) {
    Browse *b = ud;
    untrack(b, r);
    cJSON *mc = mc_of(r);
    if (!mc) return;
    cJSON *arr = cJSON_GetObjectItem(mc, "Directory");
    int n = cJSON_GetArraySize(arr);
    b->libs = realloc(b->libs, sizeof(PItem) * (b->n_libs + n + 1));
    cJSON *d;
    cJSON_ArrayForEach(d, arr) {
        PItem *l = &b->libs[b->n_libs];
        pitem_parse(l, d, r->srv);
        if (l->key && lib_hidden(r->srv, atoi(l->key))) { pitem_free(l); continue; }
        b->n_libs++;
    }
    build_side(b);
}

/* ---------- search: ranking across servers ---------- */
static void lower_into(char *dst, size_t n, const char *src) { /* ASCII + Æ/Ø/Å/É etc. (UTF-8 C3 xx) */
    size_t i = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && i + 2 < n; p++) {
        if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E) { dst[i++] = (char)*p; dst[i++] = (char)(p[1] + 0x20); p++; }
        else dst[i++] = (char)tolower(*p);
    }
    dst[i] = 0;
}
static int title_score(const char *title, const char *q) { /* q is already lowercase */
    char t[256];
    lower_into(t, sizeof t, title);
    if (!*q || !*t) return 0;
    if (!strcmp(t, q)) return 1000;
    size_t ql = strlen(q);
    if (!strncmp(t, q, ql)) return 800;
    for (const char *f = strstr(t, q); f; f = strstr(f + 1, q))
        if (!isalnum((unsigned char)f[-1])) return 600; /* a word in the title starts with the search */
    return strstr(t, q) ? 400 : 0;
}
/* rows in the search result, by Plex's hub type; best = may appear in "Best match" */
static const struct { const char *type, *label; bool best; } SGROUPS[] = {
    {"movie", N_("Movies"), true}, {"show", N_("Shows"), true}, {"season", N_("Seasons"), true}, {"episode", N_("Episodes"), true},
    {"collection", N_("Collections"), true}, {"artist", N_("Artists"), true}, {"album", N_("Albums"), true}, {"track", N_("Tracks"), true},
    {"actor", N_("Actors"), false}, {"director", N_("Directors"), false}, {"genre", N_("Genres"), false},
    {NULL, N_("Other"), false}};
#define N_SGROUPS (int)(sizeof SGROUPS / sizeof SGROUPS[0])
#define SG_TITLE 0x80
static int sgroup_of(const char *hub_type, const char *item_type) {
    for (int pass = 0; pass < 2; pass++) {
        const char *t = pass ? item_type : hub_type;
        if (t) for (int g = 0; SGROUPS[g].type; g++) if (!strcmp(t, SGROUPS[g].type)) return g;
    }
    return N_SGROUPS - 1;
}
static int search_title_match(const PItem *it, const char *q) {
    int s = title_score(it->title, q);
    if (it->show) { int ss = title_score(it->show, q) - 150; if (ss > s) s = ss; } /* episodes: the series matches */
    return s;
}
static int search_score(const PItem *it, const char *q, int pos) {
    int s = search_title_match(it, q);
    if (s == 0) s = 100; /* Plex found it on something else (actor, description …) */
    const char *ty = it->type ? it->type : "";
    if (!strcmp(ty, "movie") || !strcmp(ty, "show")) s += 60;
    else if (!strcmp(ty, "season") || !strcmp(ty, "artist") || !strcmp(ty, "album")) s += 30;
    else if (strcmp(ty, "episode")) s -= 20;
    return s - (pos < 50 ? pos : 50); /* Plex's own order as a tiebreak */
}
static int sort_q(Browse *b, int i, int j) { return b->sscore[j] - b->sscore[i]; }
static bool same_title(const PItem *a, const PItem *c) {
    if (!a->type || !c->type || strcmp(a->type, c->type) || !a->title || !c->title) return false;
    if (strcasecmp(a->title, c->title)) return false;
    if (a->show || c->show) return a->show && c->show && !strcasecmp(a->show, c->show) && a->index == c->index && a->parent_index == c->parent_index;
    return !a->year || !c->year || !strcmp(a->year, c->year);
}

static void search_rebuild(Browse *b) {
    int n = b->n_sres, *ord = malloc(sizeof(int) * (n + 1)), m = 0;
    for (int i = 0; i < n; i++) ord[i] = i;
    for (int i = 1; i < n; i++) /* insertion sort: few results */
        for (int k = i; k > 0 && sort_q(b, ord[k - 1], ord[k]) > 0; k--) { int t = ord[k]; ord[k] = ord[k - 1]; ord[k - 1] = t; }
    for (int i = 0; i < n; i++) { /* the same title on several servers: keep the best placed one */
        bool dup = false;
        for (int k = 0; k < m && !dup; k++) dup = same_title(&b->sres[ord[k]], &b->sres[ord[i]]);
        if (!dup) ord[m++] = ord[i];
    }
    int keep_row = b->row, keep_col = b->col;
    browse_free_rows(b);
    /* best match: only titles that actually match (not people/genres) */
    PItem *best = calloc(13, sizeof(PItem));
    int nb = 0;
    for (int i = 0; i < m && nb < 12; i++) {
        unsigned char f = b->sgrp[ord[i]];
        if ((f & SG_TITLE) && SGROUPS[f & ~SG_TITLE].best) best[nb++] = pitem_copy(&b->sres[ord[i]]);
    }
    add_row(b, T("Top results"), best, nb, false);
    for (int g = 0; g < N_SGROUPS; g++) {
        PItem *items = calloc(m + 1, sizeof(PItem));
        int k = 0;
        for (int i = 0; i < m; i++)
            if ((b->sgrp[ord[i]] & ~SG_TITLE) == g) items[k++] = pitem_copy(&b->sres[ord[i]]);
        add_row(b, T(SGROUPS[g].label), items, k, false);
    }
    free(ord);
    b->row = keep_row < b->n_rows ? keep_row : (b->n_rows ? b->n_rows - 1 : 0);
    int rl = b->n_rows ? b->rows[b->row].n : 0;
    b->col = keep_col < rl ? keep_col : (rl ? rl - 1 : 0);
}

static void list_cb(void *ud, Req *r) {
    Browse *b = ud;
    untrack(b, r);
    cJSON *mc = mc_of(r);
    if (!mc) { if (b->n_rows == 0 && b->pending == 0) b->failed = true; return; }
    if (b->query && cJSON_GetObjectItem(mc, "Hub")) {
        cJSON *hub;
        cJSON_ArrayForEach(hub, cJSON_GetObjectItem(mc, "Hub")) {
            PItem *hi;
            int hn = parse_items(hub, &hi, r->srv);
            char *ht = jstr(hub, "type");
            b->sres = realloc(b->sres, sizeof(PItem) * (b->n_sres + hn + 1));
            b->sscore = realloc(b->sscore, sizeof(int) * (b->n_sres + hn + 1));
            b->sgrp = realloc(b->sgrp, b->n_sres + hn + 1);
            for (int i = 0; i < hn; i++) {
                b->sscore[b->n_sres] = search_score(&hi[i], b->query, i);
                b->sgrp[b->n_sres] = (unsigned char)(sgroup_of(ht, hi[i].type) | (search_title_match(&hi[i], b->query) >= 400 ? SG_TITLE : 0));
                b->sres[b->n_sres++] = hi[i];
            }
            free(ht);
            free(hi);
        }
        search_rebuild(b);
        return;
    }
    if (cJSON_GetObjectItem(mc, "Hub")) { /* hubs: one row per type and server */
        cJSON *hub;
        cJSON_ArrayForEach(hub, cJSON_GetObjectItem(mc, "Hub")) {
            PItem *hi;
            int hn = parse_items(hub, &hi, r->srv);
            char *t = jstr(hub, "title");
            char *rt = row_title(b, t, r->srv);
            add_row(b, rt, hi, hn, false);
            free(t);
            free(rt);
        }
        return;
    }
    PItem *items;
    int n = parse_items(mc, &items, r->srv);
    if (n == 0) { free(items); return; }
    if (!b->grid) { add_row(b, NULL, items, n, false); return; }
    for (int i = 0; i < n; i += 6) { /* rows of 6 */
        int k = n - i < 6 ? n - i : 6;
        PItem *chunk = calloc(k, sizeof(PItem));
        memcpy(chunk, items + i, sizeof(PItem) * k);
        add_row(b, NULL, chunk, k, false);
    }
    free(items);
}

static void browse_enter(Screen *s) {
    Browse *b = s->st;
    if (b->is_home) {
        for (int i = 0; i < nS; i++) {
            if (!S[i].enabled) continue;
            track(b, srv_get(i, "/hubs?count=14&includeImages=1", hubs_cb, b));
            track(b, srv_get(i, "/library/sections", libs_cb, b));
        }
    } else if (b->srv < 0) {
        for (int i = 0; i < nS; i++) if (S[i].enabled) track(b, srv_get(i, b->path, list_cb, b));
    } else track(b, srv_get(b->srv, b->path, list_cb, b));
    if (b->pending == 0) b->failed = true;
}

/* after a playback: fetch the rows again (progress, watched, and the next episode in "Continue watching"),
   and keep the focus where it was */
static void browse_reload(Browse *b) {
    b->gen = play_gen;
    if (b->query || (!b->is_home && !b->path[0])) return;
    for (int i = 0; i < b->n_reqs; i++) if (b->reqs[i]) b->reqs[i]->cancelled = true;
    b->n_reqs = 0;
    b->pending = 0;
    b->want_row = b->row;
    b->want_col = b->col;
    b->restore = true;
    browse_free_rows(b);
    b->failed = false;
    if (b->is_home) {
        for (int i = 0; i < nS; i++)
            if (S[i].enabled) track(b, srv_get(i, "/hubs?count=14&includeImages=1", hubs_cb, b));
    } else if (b->srv < 0) {
        for (int i = 0; i < nS; i++) if (S[i].enabled) track(b, srv_get(i, b->path, list_cb, b));
    } else track(b, srv_get(b->srv, b->path, list_cb, b));
    if (b->pending == 0) b->restore = false;
}

static void browse_leave(Screen *s) {
    Browse *b = s->st;
    for (int i = 0; i < b->n_reqs; i++) if (b->reqs[i]) b->reqs[i]->cancelled = true;
}

static void browse_destroy(Screen *s) {
    Browse *b = s->st;
    browse_free_rows(b);
    for (int i = 0; i < b->n_sres; i++) pitem_free(&b->sres[i]);
    free(b->sres);
    free(b->sscore);
    free(b->sgrp);
    free(b->query);
    for (int i = 0; i < b->n_libs; i++) pitem_free(&b->libs[i]);
    free(b->libs);
    free(b->foc_art);
    free(b->prev_art);
    layer_free(b->bgl);
    free(b);
    free(s);
}

static int row_len(Browse *b, int r) { return r < b->n_rows ? b->rows[r].n : 0; }

/* side menu: Home, Search, libraries..., Servers, Log out, Settings */
enum { RAIL_HOME = 0, RAIL_SEARCH = 1 };
static int rail_servers(Browse *b) { return 2 + b->n_libs; }
static int rail_logout(Browse *b) { return 3 + b->n_libs; }
static int rail_settings(Browse *b) { return 4 + b->n_libs; }

static void build_side(Browse *b) {
    SideMenu *m = &b->side;
    side_clear(m);
    snprintf(m->subtitle, sizeof m->subtitle, "%s", cfg_get("plex_server_name", ""));
    side_add(m, T("Home"), ICON_TV, false);
    side_add(m, T("Search"), ICON_SEARCH, false);
    bool multi = n_enabled() > 1;
    for (int i = 0; i < b->n_libs; i++) {
        PItem *l = &b->libs[i];
        char lbl[300];
        if (multi && srv_ok(l->srv)) snprintf(lbl, sizeof lbl, "%s · %s", l->title ? l->title : T("Library"), S[l->srv].name);
        else snprintf(lbl, sizeof lbl, "%s", l->title ? l->title : T("Library"));
        IconId ic = l->type && !strcmp(l->type, "artist") ? ICON_MUSIC : l->type && !strcmp(l->type, "photo") ? ICON_FOLDER : ICON_FILM;
        side_add(m, lbl, ic, i == 0);
    }
    side_add(m, T("Servers"), ICON_GEAR, true);
    side_add(m, T("Sign out"), ICON_POWER, false);
    if (app_settings_open) side_add(m, T("Settings"), ICON_GEAR, false); /* the app's settings, right here */
}

static bool is_landscape(PItem *it) { return it->type && (!strcmp(it->type, "episode") || !strcmp(it->type, "clip")); }

/* the height a row takes up in browse_draw (title + tiles + spacing) */
static float row_h(const Browse *b, int r) {
    const Row *row = &b->rows[r];
    bool land = row->n && (row->cont || is_landscape(&row->items[0]));
    return (row->title && row->title[0] ? 56 : 0) + (land ? 248 : 375) + (b->grid ? 90 : 110);
}

static void browse_update(Screen *s, float dt) {
    Browse *b = s->st;
    if (b->gen != play_gen) browse_reload(b);
    if (b->restore && b->pending == 0) {
        b->restore = false;
        b->row = b->want_row < b->n_rows ? b->want_row : (b->n_rows ? b->n_rows - 1 : 0);
        int rl = b->n_rows ? b->rows[b->row].n : 0;
        b->col = b->want_col < rl ? b->want_col : (rl ? rl - 1 : 0);
    }
    if (b->is_home) side_update(&b->side, dt);
    if (b->row >= b->n_rows) b->row = b->n_rows ? b->n_rows - 1 : 0;
    float rowh = b->grid ? 440 : 470;
    float ty = b->row * rowh - (b->grid ? rowh : 0);
    if (b->is_home && !b->grid) { /* the rows have different heights (wide/tall tiles) */
        ty = 0;
        for (int r = 0; r < b->row; r++) ty += row_h(b, r);
    }
    if (hero_on(b)) {
        ty = b->hero ? 0 : HERO_H + ty;
        if (b->hero && !b->side.open && app_time() - b->hero_t > 12) { b->hero_idx++; b->hero_t = app_time(); b->hero_fade = 0; }
        b->hero_fade = anim_to(b->hero_fade, 1, dt, 6);
    }
    if (focus_on(b)) b->hero_fade = anim_to(b->hero_fade, 1, dt, 7);
    if (ty < 0) ty = 0;
    b->scroll_y = anim_to(b->scroll_y, ty, dt, 12);
    float avail = UI_W - b->x0 - 140;
    for (int r = 0; r < b->n_rows; r++) {
        float iw = b->rows[r].n && (b->rows[r].cont || is_landscape(&b->rows[r].items[0])) ? 440 : 250;
        float target = 0;
        if (r == b->row && !b->grid) {
            float sel = b->col * (iw + 30);
            if (sel > avail - iw) target = sel - (avail - iw);
        }
        b->scroll_x[r] = anim_to(b->scroll_x[r], target, dt, 12);
    }
}

static void draw_item(PItem *it, float x, float y, float w, float h, bool f, bool landscape, bool show_srv, bool cont) {
    float fa = ui_focus(it, f), sc = 1 + 0.07f * fa;
    float ww = w * sc, hh = h * sc, xx = x - (ww - w) / 2, yy = y - (hh - h) / 2;
    ui_focus_under(xx, yy, ww, hh, 14, fa);
    fill_rrect(xx, yy, ww, hh, 14, C_SURFACE);
    const char *la = cont && it->sart ? it->sart : it->art ? it->art : it->thumb;
    char *u = img_url(it->srv, landscape ? la : it->thumb, landscape ? 640 : 400, landscape ? 360 : 600);
    Image *im = img_get(u);
    free(u);
    if (!img_draw_cover(im, xx, yy, ww, hh, 14, 1))
        text_draw_wrap(F_SEMI, 24, xx + 16, yy + 20, ww - 32, 4, 30, C_TEXT2, it->title);
    if (it->duration > 0 && it->offset > 0) {
        fill_rrect(xx + 12, yy + hh - 14, ww - 24, 6, 3, RGBA(0, 0, 0, 150));
        fill_rrect(xx + 12, yy + hh - 14, (ww - 24) * (float)(it->offset / it->duration), 6, 3, RGB(229, 160, 13));
    } else if (it->view_count == 0 && it->type && (!strcmp(it->type, "movie") || !strcmp(it->type, "episode"))) {
        fill_tri(xx + ww - 34, yy, xx + ww, yy, xx + ww, yy + 34, RGB(229, 160, 13));
    } else if (it->leaves > 0 && it->viewed_leaves < it->leaves) {
        char n[16];
        snprintf(n, sizeof n, "%d", it->leaves - it->viewed_leaves);
        float bw = text_width(F_BOLD, 20, n) + 18;
        fill_rrect(xx + ww - bw - 8, yy + 8, bw, 30, 15, RGB(229, 160, 13));
        text_draw(F_BOLD, 20, xx + ww - bw / 2 - 8, yy + 12, RGB(20, 20, 20), AL_CENTER, n);
    }
    ui_focus_over(xx, yy, ww, hh, 14, fa);
    bool ep = it->type && !strcmp(it->type, "episode");
    text_draw_clip(F_SEMI, 24, x, y + h + 14, w, f ? C_TEXT : C_TEXT2, AL_LEFT, ep && it->subtitle ? it->subtitle : it->title);
    char sub[300] = "";
    const char *s2 = ep ? it->title : it->subtitle;
    if (show_srv && srv_ok(it->srv)) snprintf(sub, sizeof sub, "%s%s%s", s2 ? s2 : "", s2 && *s2 ? " · " : "", S[it->srv].name);
    else if (s2) snprintf(sub, sizeof sub, "%s", s2);
    if (sub[0]) text_draw_clip(F_REG, 22, x, y + h + 44, w, C_TEXT3, AL_LEFT, sub);
}

static void browse_draw(Screen *s) {
    Browse *b = s->st;
    /* focus style: the whole backdrop is in the layer (bgl) - saves a full-screen draw */
    bool bg_in_layer = focus_on(b) && b->row < b->n_rows && b->col < b->rows[b->row].n;
    if (!bg_in_layer) draw_background();
    if (b->is_home) draw_topbar(NULL);
    else draw_topbar(b->title);
    float cx = b->is_home ? b->x0 + (UI_W - b->x0) / 2 : UI_W / 2;
    if (b->pending > 0 && b->n_rows == 0) { draw_spinner(cx, UI_H / 2, 34, C_TEXT); if (b->is_home) side_draw(&b->side); return; }
    if (b->n_rows == 0) {
        const char *t = b->failed ? T("Could not load from Plex") : T("Empty");
        const char *m = b->failed ? T("The server did not respond. Choose servers in the side menu (left).") : T("Nothing here yet.");
        text_draw(F_BOLD, 40, cx, 430, C_TEXT, AL_CENTER, t);
        text_draw(F_REG, 26, cx, 500, C_TEXT2, AL_CENTER, m);
        if (b->is_home) side_draw(&b->side);
        return;
    }
    bool multi = n_enabled() > 1;
    bool rail = b->is_home && b->side.open;
    float y = (b->is_home ? 150 : 170) - b->scroll_y;
    if (hero_on(b)) {
        PItem *hs[8];
        int nh = hero_items(b, hs, 8);
        if (nh && b->scroll_y < HERO_H + 100) {
            PItem *h = hs[((b->hero_idx % nh) + nh) % nh];
            float off = b->scroll_y, a = 1 - fminf(1, off / HERO_H);
            for (int i = 0; i < nh; i++) prefetch_bg(hs[i]);
            char *u = img_url(h->srv, h->art, 1280, 720);
            Image *him = img_get(u);
            if (img_state(him) != IMG_READY) b->hero_fade = 0; /* the fade only starts when the image is ready */
            /* full-width backdrop behind the hero, fading out to the left and down */
            if (img_draw_cover(him, 0, -off * 0.5f, UI_W, 820, 0, a * b->hero_fade)) {
                fill_hgrad(0, 0, UI_W * 0.75f, 820, RGBA(8, 9, 13, (uint8_t)(235 * a)), RGBA(8, 9, 13, 0));
                fill_vgrad(0, 420 - off * 0.5f, UI_W, 400, RGBA(8, 9, 13, 0), RGBA(8, 9, 13, 255));
            }
            free(u);
            float hx = b->x0, hy = 200 - off;
            draw_info(h, hx, hy, a, 3, false);
            /* button + dots */
            bool f = b->hero && !rail;
            const char *lbl = h->offset > 0 ? T("Resume") : T("Play");
            float bw = text_width(F_SEMI, 28, lbl) + 110, by = hy + 380;
            fill_rrect(hx, by, bw, 72, 36, f ? C_FOCUS : RGBA(255, 255, 255, (uint8_t)(50 * a)));
            draw_icon(ICON_PLAY, hx + 42, by + 36, 26, f ? RGB(10, 10, 10) : color_alpha(C_TEXT, a));
            text_draw(F_SEMI, 28, hx + 70, by + 18, f ? RGB(10, 10, 10) : color_alpha(C_TEXT, a), AL_LEFT, lbl);
            if (f) text_draw(F_REG, 22, hx + bw + 30, by + 24, C_TEXT3, AL_LEFT, T("OK: info   ·   Play button: play   ·   Left/Right: more"));
            for (int i = 0; i < nh; i++) {
                bool cur = i == ((b->hero_idx % nh) + nh) % nh;
                fill_rrect(hx + i * 28, by + 100, cur ? 20 : 10, 10, 5, color_alpha(cur ? C_TEXT : C_TEXT3, a));
            }
        }
        y += HERO_H;
    }
    if (focus_on(b) && b->row < b->n_rows && b->col < b->rows[b->row].n) {
        PItem *h = &b->rows[b->row].items[b->col];
        char key[64];
        snprintf(key, sizeof key, "%d:%s", h->srv, h->rating_key ? h->rating_key : "");
        if (strcmp(key, b->foc_key)) { /* new title in focus: the old backdrop fades out, the new one in */
            snprintf(b->foc_key, sizeof b->foc_key, "%s", key);
            free(b->prev_art);
            b->prev_art = b->foc_art;
            b->foc_art = bg_url(h);
            b->hero_fade = 0;
        }
        /* the neighbors in the row and the same position in the rows above/below */
        Row *cr = &b->rows[b->row];
        for (int d = -2; d <= 3; d++)
            if (d && b->col + d >= 0 && b->col + d < cr->n) prefetch_bg(&cr->items[b->col + d]);
        for (int d = -1; d <= 1; d += 2) {
            int r = b->row + d;
            if (r >= 0 && r < b->n_rows && b->rows[r].n) prefetch_bg(&b->rows[r].items[0]);
        }
        Image *ni = img_get(b->foc_art), *pi = img_get(b->prev_art);
        bool ready = img_state(ni) == IMG_READY;
        if (!ready) b->hero_fade = 0; /* the fade only starts when the image is ready */
        float fa = b->hero_fade;
        /* the backdrop ends (fully faded out) above the rows, so no image lies behind the tiles and their text.
           It is drawn into a layer and only redrawn when the title or the fade changes */
        if (!b->bgl) { b->bgl = layer_new(UI_W, UI_H); layer_set_opaque(b->bgl, true); }
        char lkey[80];
        snprintf(lkey, sizeof lkey, "%s|%d", b->foc_key, ready);
        bool settled = fa > 0.995f && b->bgl_fade > 0.995f;
        if ((!layer_valid(b->bgl) || strcmp(lkey, b->bgl_key) || !settled) && layer_begin(b->bgl)) {
            draw_background();
            if (b->prev_art && fa < 1) img_draw_cover(pi, 0, 0, UI_W, BG_H, 0, 1 - fa);
            img_draw_cover(ni, 0, 0, UI_W, BG_H, 0, fa);
            fill_hgrad(0, 0, UI_W * 0.75f, BG_H, RGBA(8, 9, 13, 235), RGBA(8, 9, 13, 0));
            /* faded down to the background's color at that height (draw_background is a gradient) */
            fill_vgrad(0, BG_H - 330, UI_W, 330, RGBA(12, 14, 20, 0), RGBA(12, 14, 20, 255));
            layer_end(b->bgl);
            snprintf(b->bgl_key, sizeof b->bgl_key, "%s", lkey);
            b->bgl_fade = fa;
        }
        layer_draw(b->bgl, 0, 0, 1);
        if (!rail) draw_info(h, b->x0, 130, 1, 2, true);
        y += FOCUS_H;
    }
    /* focus style: the rows must not scroll up over the info text */
    float clip_y = focus_on(b) ? 150 + FOCUS_H - 20 : 140;
    clip_push(b->is_home ? SIDE_W : 0, clip_y, UI_W, UI_H - clip_y);
    for (int r = 0; r < b->n_rows; r++) {
        Row *row = &b->rows[r];
        bool land = row->n && (row->cont || is_landscape(&row->items[0]));
        float iw = land ? 440 : 250, ih = land ? 248 : 375;
        if (row->title && row->title[0]) {
            text_draw(F_SEMI, 30, b->x0, y, r == b->row && !rail && !b->hero ? C_TEXT : C_TEXT2, AL_LEFT, row->title);
            y += 56;
        }
        if (y > UI_H + 50) break;
        if (y + ih > 0) {
            for (int i = 0; i < row->n; i++) {
                float x = b->x0 + i * (iw + 30) - b->scroll_x[r];
                if (x > UI_W || x + iw < 0) continue;
                draw_item(&row->items[i], x, y, iw, ih, !rail && !b->hero && r == b->row && b->col == i, land, row->cont && multi, row->cont);
            }
        }
        y += ih + (b->grid ? 90 : 110);
    }
    clip_pop();
    if (b->is_home) side_draw(&b->side);
}

static Screen *show_screen(int srv, const char *show_key, const char *focus_rk, int season);
static void open_item(PItem *it) {
    if (!it->type) return;
    if (!strcmp(it->type, "episode") && it->grandparent_key) { /* the series page, with this episode in focus */
        app_push(show_screen(it->srv, it->grandparent_key, it->rating_key, it->parent_index));
    } else if (!strcmp(it->type, "movie") || !strcmp(it->type, "episode") || !strcmp(it->type, "clip")) {
        app_push(detail_screen(it));
    } else if (!strcmp(it->type, "show") && it->rating_key) {
        app_push(show_screen(it->srv, it->rating_key, NULL, -1));
    } else if (!strcmp(it->type, "season") && it->grandparent_key) {
        app_push(show_screen(it->srv, it->grandparent_key, NULL, it->index));
    } else if (!strcmp(it->type, "artist") || !strcmp(it->type, "album")) {
        char *p = str_printf("/library/metadata/%s/children", it->rating_key);
        app_push(browse_screen(it->title, p, true, it->srv));
        free(p);
    } else if (!strcmp(it->type, "track")) {
        plex_play(it, 0, NULL, NULL);
    }
}

/* sign out: the login, the servers (with their tokens), what is shown on the home screen and the leftovers in memory and /tmp */
void plex_forget(void) {
    plex_gen++; /* replies from before the sign-out are ignored */
    cfg_set("plex_token", "");
    cfg_set("plex_server_uri", "");
    cfg_set("plex_server_token", "");
    cfg_set("plex_server_name", "");
    cfg_set("plex_server_id", "");
    char path[512];
    srv_path(path, sizeof path);
    wipe_file(path);
    memset(S, 0, sizeof S);
    nS = 0;
    for (int i = 0; i < n_deck_items; i++) pitem_free(&deck_items[i]);
    n_deck_items = n_deck = 0;
    deck_gen++; /* answers on their way are ignored */
    for (int i = 0; i < IMGU_N; i++) { free(imgu[i].path); free(imgu[i].url); }
    memset(imgu, 0, sizeof imgu);
    wipe_session_traces();
}

static void logout_cb(void *ud, bool yes) {
    (void)ud;
    if (!yes) return;
    plex_forget();
    app_toast(T("Signed out of Plex"));
    app_home();
}

static void search_cb(void *ud, const char *q) {
    (void)ud;
    if (!q) return;
    while (*q == ' ') q++;
    char qt[200];
    snprintf(qt, sizeof qt, "%s", q);
    for (size_t l = strlen(qt); l && qt[l - 1] == ' '; l--) qt[l - 1] = 0;
    if (!*qt) return;
    char *e = url_encode(qt);
    char *p = str_printf("/hubs/search?query=%s&limit=30", e);
    char *t = str_printf(T("Search: %s"), qt);
    Screen *s = browse_screen(t, p, false, -1);
    char ql[200];
    lower_into(ql, sizeof ql, qt);
    ((Browse *)s->st)->query = str_dup(ql); /* set before the first reply (browse_enter runs when shown) */
    app_push(s);
    free(e); free(p); free(t);
}

static void plex_menu_cb(void *ud, int idx) {
    (void)ud;
    if (idx == 0) app_push(servers_screen(false));
    else if (idx == 1) keyboard_open(T("Search Plex"), "", false, search_cb, NULL);
    else if (idx == 2) confirm_open(T("Sign out of Plex?"), T("You will need to link again via plex.tv/link."), T("Sign out"), T("Cancel"), logout_cb, NULL);
}

static void rail_activate(Browse *b, int i) {
    if (i == RAIL_HOME) { b->side.active = 0; b->row = 0; b->col = 0; }
    else if (i == RAIL_SEARCH) keyboard_open(T("Search Plex"), "", false, search_cb, NULL);
    else if (i == rail_servers(b)) app_push(servers_screen(false));
    else if (i == rail_settings(b)) app_settings_open("plex");
    else if (i == rail_logout(b)) confirm_open(T("Sign out of Plex?"), T("You will need to link again via plex.tv/link."), T("Sign out"), T("Cancel"), logout_cb, NULL);
    else {
        PItem *lib = &b->libs[i - 2];
        char *p = str_printf("/library/sections/%s/all?X-Plex-Container-Start=0&X-Plex-Container-Size=2000", lib->key);
        app_push(browse_screen(lib->title, p, true, lib->srv));
        free(p);
    }
}

static bool browse_input(Screen *s, const InputEvent *ev) {
    Browse *b = s->st;
    int nr = b->n_rows;
    if (b->is_home) {
        if (nr == 0 && !b->side.open && ev->a != A_BACK && ev->a != A_HOME) side_open(&b->side);
        int act;
        if (side_input(&b->side, ev, &act)) {
            if (act >= 0) rail_activate(b, act);
            if (nr == 0 && ev->a == A_RIGHT) b->side.open = true; /* no content to go to */
            return true;
        }
    }
    if (b->hero && hero_on(b)) { /* hero i fokus */
        PItem *hs[8];
        int nh = hero_items(b, hs, 8);
        PItem *h = nh ? hs[((b->hero_idx % nh) + nh) % nh] : NULL;
        switch (ev->a) {
        case A_DOWN: if (nr) { b->hero = false; b->row = 0; b->col = 0; } return true;
        case A_UP: return true;
        case A_RIGHT: b->hero_idx++; b->hero_t = app_time(); b->hero_fade = 0; return true;
        case A_LEFT:
            if (b->is_home && (b->hero_idx <= 0 || !nh)) side_open(&b->side);
            else { b->hero_idx--; b->hero_t = app_time(); b->hero_fade = 0; }
            return true;
        case A_OK: if (h && !ev->repeat) open_item(h); return true;
        case A_INFO: if (h && !ev->repeat) watch_menu(h, false); return true;
        case A_PLAYPAUSE: case A_PLAY:
            if (h && !ev->repeat) {
                if (h->part) play_with_resume(h, h);
                else if (h->type && (!strcmp(h->type, "show") || !strcmp(h->type, "season"))) play_show(h);
                else open_item(h);
            }
            return true;
        default: break;
        }
    }
    switch (ev->a) {
    case A_UP:
        if (b->row == 0 && hero_on(b)) { b->hero = true; b->hero_t = app_time(); return true; }
        if (b->row > 0) { b->row--; int l = row_len(b, b->row); if (!b->grid) b->col = 0; else if (b->col >= l) b->col = l - 1; }
        return true;
    case A_DOWN:
        if (b->row < nr - 1) { b->row++; int l = row_len(b, b->row); if (!b->grid) b->col = 0; else if (b->col >= l) b->col = l - 1; }
        return true;
    case A_LEFT:
        if (b->col > 0) b->col--;
        else if (b->is_home) side_open(&b->side);
        return true;
    case A_RIGHT: if (nr && b->col < row_len(b, b->row) - 1) b->col++; return true;
    case A_BACK:
        if (hero_on(b) && !b->hero) { b->hero = true; b->row = b->col = 0; return true; }
        if (b->is_home && (b->row > 0 || b->col > 0)) { b->row = b->col = 0; return true; }
        return false;
    case A_OK:
        if (!nr) return true;
        open_item(&b->rows[b->row].items[b->col]);
        return true;
    case A_PLAYPAUSE: case A_PLAY:
        if (nr) {
            PItem *it = &b->rows[b->row].items[b->col];
            if (it->part) play_with_resume(it, it);
            else if (it->type && (!strcmp(it->type, "show") || !strcmp(it->type, "season"))) play_show(it);
            else open_item(it);
        }
        return true;
    case A_MENU: {
        const char *items[] = {T("Choose servers"), T("Search"), T("Sign out of Plex")};
        choice_open("Plex", items, 3, 0, plex_menu_cb, NULL);
        return true;
    }
    case A_INFO:
        if (nr && !ev->repeat) watch_menu(&b->rows[b->row].items[b->col], b->rows[b->row].cont);
        return true;
    case A_SEARCH: keyboard_open(T("Search Plex"), "", false, search_cb, NULL); return true;
    default: return false;
    }
}

static Screen *browse_screen(const char *title, const char *path, bool grid, int srv) {
    Screen *s = calloc(1, sizeof *s);
    Browse *b = calloc(1, sizeof *b);
    snprintf(b->title, sizeof b->title, "%s", title ? title : "Plex");
    b->gen = play_gen;
    if (path) snprintf(b->path, sizeof b->path, "%s", path);
    b->grid = grid;
    b->is_home = path == NULL;
    b->hero = b->is_home && !strcmp(cfg_get("plex_home_style", "hero"), "hero"); /* the home screen starts on the hero */
    b->srv = srv;
    b->x0 = b->is_home ? SIDE_W + 60 : 120;
    if (b->is_home) { side_init(&b->side, "Plex", ICON_PLEX, RGB(229, 160, 13)); build_side(b); }
    s->name = "plex-browse";
    s->st = b;
    s->enter = browse_enter;
    s->leave = browse_leave;
    s->update = browse_update;
    s->draw = browse_draw;
    s->input = browse_input;
    s->destroy = browse_destroy;
    return s;
}

/* ---- detail page (with trailer) ---- */
typedef struct {
    PItem it;
    PItem trailer;
    bool has_trailer, extras_done, asked_seerr;
    int sel;
    int gen; /* play_gen when the item was fetched: fetched again after a playback or a watched mark */
    Req *req, *ereq;
} Detail;
static Detail *live_pdetail;

static void seerr_rat_cb(void *ud, int rc, int ra, int im) {
    Detail *d = ud;
    if (d != live_pdetail) return;
    if (d->it.rt_critic < 0) d->it.rt_critic = rc;
    if (d->it.rt_audience < 0) d->it.rt_audience = ra;
    if (d->it.imdb10 < 0) d->it.imdb10 = im;
}

static void detail_cb(void *ud, Req *r) {
    Detail *d = ud;
    d->req = NULL;
    cJSON *mc = mc_of(r);
    PItem *items;
    int n = mc ? parse_items(mc, &items, r->srv) : 0;
    if (n > 0) {
        pitem_free(&d->it);
        d->it = items[0];
        for (int i = 1; i < n; i++) pitem_free(&items[i]);
        free(items);
        /* the server has no IMDb/RT: ask Seerr (if it is installed and set up) */
        bool tv = d->it.type && strcmp(d->it.type, "movie");
        const SeerrIface *seerr = app_iface("seerr"); /* NULL when the Seerr app is not installed */
        if (seerr && !d->asked_seerr && d->it.tmdb > 0 && (d->it.imdb10 < 0 || d->it.rt_critic < 0) && !tv) {
            d->asked_seerr = true;
            seerr->ratings(false, d->it.tmdb, seerr_rat_cb, d);
        }
    } else if (n == 0 && mc) free(items);
}

static void extras_cb(void *ud, Req *r) {
    Detail *d = ud;
    d->ereq = NULL;
    d->extras_done = true;
    cJSON *mc = mc_of(r);
    if (!mc) return;
    PItem *items;
    int n = parse_items(mc, &items, r->srv);
    int pick = -1;
    for (int i = 0; i < n && pick < 0; i++)
        if (items[i].part && (items[i].extra_type == 1 || (items[i].subtype && !strcmp(items[i].subtype, "trailer")))) pick = i;
    for (int i = 0; i < n && pick < 0; i++) if (items[i].part) pick = i; /* andet ekstramateriale */
    if (pick >= 0) { d->trailer = items[pick]; d->has_trailer = true; }
    for (int i = 0; i < n; i++) if (i != pick) pitem_free(&items[i]);
    free(items);
}

static void detail_enter(Screen *s) {
    Detail *d = s->st;
    live_pdetail = d;
    d->gen = play_gen;
    char *p = str_printf("/library/metadata/%s?includeGuids=1", d->it.rating_key);
    d->req = srv_get(d->it.srv, p, detail_cb, d);
    free(p);
    p = str_printf("/library/metadata/%s/extras", d->it.rating_key);
    d->ereq = srv_get(d->it.srv, p, extras_cb, d);
    free(p);
}

static void detail_leave(Screen *s) {
    Detail *d = s->st;
    if (live_pdetail == d) live_pdetail = NULL;
    if (d->req) d->req->cancelled = true;
    if (d->ereq) d->ereq->cancelled = true;
}

static void detail_destroy(Screen *s) {
    Detail *d = s->st;
    pitem_free(&d->it);
    if (d->has_trailer) pitem_free(&d->trailer);
    free(d);
    free(s);
}


static void detail_update(Screen *s, float dt) {
    (void)dt;
    Detail *d = s->st;
    if (d->gen != play_gen && !d->req) { /* watched mark or progress changed: fetch the item again */
        d->gen = play_gen;
        char *p = str_printf("/library/metadata/%s?includeGuids=1", d->it.rating_key);
        d->req = srv_get(d->it.srv, p, detail_cb, d);
        free(p);
    }
}

/* buttons: [Resume] [Start over|Play] [Trailer] [Mark as watched|unwatched] [Remove from Continue] */
typedef enum { B_RESUME, B_PLAY, B_START, B_TRAILER, B_SEEN, B_DECK } BtnKind;
#define MAX_BTNS 6
static int detail_buttons(Detail *d, BtnKind *k) {
    int n = 0;
    if (d->it.offset > 30) { k[n++] = B_RESUME; k[n++] = B_START; }
    else k[n++] = B_PLAY;
    bool movie = d->it.type && !strcmp(d->it.type, "movie");
    if (d->has_trailer || (movie && d->extras_done)) k[n++] = B_TRAILER;
    if (can_mark(&d->it)) {
        k[n++] = B_SEEN;
        if (d->it.offset > 0) k[n++] = B_DECK;
    }
    return n;
}

/* append to a fixed buffer without letting the offset run past the end */
static int meta_add(char *m, size_t n, int o, const char *fmt, ...) {
    if (!n) return 0;
    size_t l = (size_t)o < n ? (size_t)o : n - 1;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(m + l, n - l, fmt, ap);
    va_end(ap);
    if (r > 0) l += (size_t)r;
    return (int)(l < n ? l : n - 1);
}

static void detail_draw(Screen *s) {
    Detail *d = s->st;
    PItem *it = &d->it;
    draw_background();
    char *u = img_url(it->srv, it->art, 1920, 1080);
    Image *bg = img_get(u);
    free(u);
    if (img_draw_cover(bg, 0, 0, UI_W, UI_H, 0, 0.55f)) {
        fill_hgrad(0, 0, UI_W * 0.8f, UI_H, RGBA(8, 9, 13, 250), RGBA(8, 9, 13, 0));
        fill_vgrad(0, UI_H * 0.5f, UI_W, UI_H * 0.5f, RGBA(8, 9, 13, 0), RGBA(8, 9, 13, 240));
    }
    float x = 120, y = 220;
    if (it->subtitle && it->type && !strcmp(it->type, "episode"))
        text_draw(F_SEMI, 30, x, y - 50, C_TEXT2, AL_LEFT, it->subtitle);
    text_draw_clip(F_BOLD, 68, x, y, 1300, C_TEXT, AL_LEFT, it->title);
    char meta[300] = "", dur[32];
    int o = 0;
    if (it->year) o = meta_add(meta, sizeof meta, o, "%s   ", it->year);
    if (it->duration > 0) o = meta_add(meta, sizeof meta, o, T("%d min   "), (int)(it->duration / 60));
    if (it->rating) o = meta_add(meta, sizeof meta, o, "%s   ", it->rating);
    if (it->offset > 0) o = meta_add(meta, sizeof meta, o, T("%s left   "), fmt_duration(it->duration - it->offset, dur, sizeof dur));
    if (n_enabled() > 1 && srv_ok(it->srv)) snprintf(meta + o, sizeof meta - o, "·  %s", S[it->srv].name);
    text_draw(F_REG, 28, x, y + 100, C_TEXT2, AL_LEFT, meta);
    float rx = draw_ratings(x, y + 156, it->rt_critic, it->rt_audience, it->imdb10);
    text_draw_wrap(F_REG, 28, x, rx > x ? y + 226 : y + 170, 1000, rx > x ? 6 : 7, 40, C_TEXT, it->summary ? it->summary : "");
    BtnKind k[MAX_BTNS];
    int nb = detail_buttons(d, k);
    if (d->sel >= nb) d->sel = nb - 1;
    float bx = x, by = 820;
    for (int i = 0; i < nb; i++) {
        char lbl[64];
        switch (k[i]) {
        case B_RESUME: snprintf(lbl, sizeof lbl, T("Continue (%s)"), fmt_duration(it->offset, dur, sizeof dur)); break;
        case B_PLAY: snprintf(lbl, sizeof lbl, T("Play")); break;
        case B_START: snprintf(lbl, sizeof lbl, T("Start over")); break;
        case B_TRAILER: snprintf(lbl, sizeof lbl, "%s", d->has_trailer ? T("Trailer") : T("Trailer (YouTube)")); break;
        case B_SEEN: snprintf(lbl, sizeof lbl, "%s", is_seen(it) ? T("Mark as unwatched") : T("Mark as watched")); break;
        case B_DECK: snprintf(lbl, sizeof lbl, "%s", T("Remove from Continue")); break;
        }
        bool f = d->sel == i;
        float w = text_width(F_SEMI, 28, lbl) + 110;
        fill_rrect(bx, by, w, 76, 38, f ? C_FOCUS : RGBA(255, 255, 255, 40));
        IconId ic = k[i] == B_TRAILER ? ICON_FILM : k[i] == B_SEEN ? ICON_CHECK : k[i] == B_DECK ? ICON_BACK : ICON_PLAY;
        draw_icon(ic, bx + 42, by + 38, 26, f ? RGB(10, 10, 10) : C_TEXT);
        text_draw(F_SEMI, 28, bx + 70, by + 20, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, lbl);
        bx += w + 24;
    }
}

static void play_youtube_trailer(PItem *it);

/* Plex trailers are often "indirect" (fetched from Plex's own servers) and cannot be played
   directly - the server's universal transcoder handles both kinds. */
static void play_plex_trailer(PItem *tr, PItem *movie) {
    if (!srv_ok(tr->srv)) return;
    PSrv *sv = &S[tr->srv];
    char path[96];
    snprintf(path, sizeof path, "/library/metadata/%s", tr->rating_key ? tr->rating_key : "");
    char *ep = url_encode(path);
    char *url = str_printf("%s/video/:/transcode/universal/start.m3u8?path=%s&protocol=hls&mediaIndex=0&partIndex=0"
                           "&directPlay=0&directStream=1&fastSeek=1&videoResolution=1920x1080&maxVideoBitrate=12000"
                           "&X-Plex-Platform=Chrome&X-Plex-Product=NotaTV&X-Plex-Client-Identifier=%s&session=notatv-%ld&X-Plex-Token=%s",
                           sv->uri, ep, plex_client_id(), (long)time(NULL), sv->token);
    free(ep);
    char *title = str_printf("%s - trailer", movie->title ? movie->title : "");
    PlayRequest rq = {0};
    rq.url = url;
    rq.title = title;
    PlayResult res = player_run(&rq);
    free(url);
    free(title);
    if (res.exit_code != 0 && res.dur <= 0) { app_toast(T("The Plex trailer could not be played - trying YouTube")); play_youtube_trailer(movie); }
}

static void play_youtube_trailer(PItem *it) {
    char q[400];
    snprintf(q, sizeof q, "ytdl://ytsearch1:%s %s trailer", it->title ? it->title : "", it->year ? it->year : "");
    char *t = str_printf("%s - trailer", it->title ? it->title : "");
    PlayRequest rq = {0};
    rq.url = q;
    rq.title = t;
    static const char *extra[] = {"--ytdl-format=bv*[height<=1080][vcodec!^=av01]+ba/b", NULL};
    rq.extra = extra;
    app_toast(T("Finding trailer…"));
    player_run(&rq);
    free(t);
}

static bool detail_input(Screen *s, const InputEvent *ev) {
    Detail *d = s->st;
    BtnKind k[MAX_BTNS];
    int nb = detail_buttons(d, k);
    switch (ev->a) {
    case A_LEFT: if (d->sel > 0) d->sel--; return true;
    case A_RIGHT: if (d->sel < nb - 1) d->sel++; return true;
    case A_OK: case A_PLAYPAUSE: case A_PLAY:
        switch (k[d->sel]) {
        case B_RESUME: plex_play_chain(&d->it, d->it.offset, &d->it); break;
        case B_PLAY: case B_START: plex_play_chain(&d->it, 0, &d->it); break;
        case B_TRAILER:
            if (d->has_trailer) play_plex_trailer(&d->trailer, &d->it);
            else play_youtube_trailer(&d->it);
            break;
        case B_SEEN:
            if (!ev->repeat) watch_do(d->it.srv, d->it.rating_key, is_seen(&d->it) ? WA_UNSEEN : WA_SEEN);
            return true; /* keep the focus on the button */
        case B_DECK:
            if (!ev->repeat) { watch_do(d->it.srv, d->it.rating_key, WA_DECK); d->sel = 0; }
            return true;
        }
        d->sel = 0;
        return true;
    case A_INFO: if (!ev->repeat) watch_menu(&d->it, false); return true;
    default: return false;
    }
}

static Screen *detail_screen(PItem *src) {
    Screen *s = calloc(1, sizeof *s);
    Detail *d = calloc(1, sizeof *d);
    d->it = pitem_copy(src);
    s->name = "plex-detail";
    s->st = d;
    s->enter = detail_enter;
    s->leave = detail_leave;
    s->update = detail_update;
    s->draw = detail_draw;
    s->input = detail_input;
    s->destroy = detail_destroy;
    return s;
}

/* ---- series page: the series at the top, Play/Continue, the seasons and the episodes of the chosen season ----
   Opened from a series, a season or an episode (e.g. "Continue watching" or "Recently added"). All episodes are
   fetched at once (allLeaves) and grouped by season, so switching season needs no new request. */
#define SHOW_MAX_SEASONS 128
typedef struct {
    int srv;
    char key[32], focus_rk[32];
    int want_season; /* -1 = the season of the next episode */
    PItem show;
    bool have_show, loaded, failed, placed;
    PItem *eps;
    int n_eps;
    int seasons[SHOW_MAX_SEASONS], n_seasons; /* season numbers, specials (0) last */
    int *sidx, n_sidx;                       /* eps of the chosen season */
    int row;                                 /* 0 = buttons, 1 = seasons, 2 = episodes */
    int bsel, ssel, esel, next;              /* next = index in eps that Play/Continue plays (-1 = none) */
    float sx_s, sx_e;
    Req *req, *ereq;
    int gen;
} Show;
static Show *sheet_show; /* the series page whose sheet is open (cleared when the page closes) */

static void show_pick_season(Show *sh, int si) {
    sh->ssel = si < 0 ? 0 : si >= sh->n_seasons ? sh->n_seasons - 1 : si;
    sh->n_sidx = 0;
    if (!sh->n_seasons) return;
    for (int i = 0; i < sh->n_eps; i++)
        if (sh->eps[i].parent_index == sh->seasons[sh->ssel]) sh->sidx[sh->n_sidx++] = i;
}
static int show_season_of(Show *sh, int season) {
    for (int i = 0; i < sh->n_seasons; i++) if (sh->seasons[i] == season) return i;
    return -1;
}
static int show_ep_pos(Show *sh, const char *rk) { /* position in the chosen season */
    for (int i = 0; rk && *rk && i < sh->n_sidx; i++)
        if (sh->eps[sh->sidx[i]].rating_key && !strcmp(sh->eps[sh->sidx[i]].rating_key, rk)) return i;
    return -1;
}

static void show_info_cb(void *ud, Req *r) {
    Show *sh = ud;
    sh->req = NULL;
    cJSON *mc = mc_of(r);
    PItem *items;
    int n = mc ? parse_items(mc, &items, r->srv) : 0;
    if (n > 0) {
        if (sh->have_show) pitem_free(&sh->show);
        sh->show = items[0];
        sh->have_show = true;
        for (int i = 1; i < n; i++) pitem_free(&items[i]);
    }
    if (mc) free(items);
}

static void show_eps_cb(void *ud, Req *r) {
    Show *sh = ud;
    sh->ereq = NULL;
    cJSON *mc = mc_of(r);
    if (!mc) { if (!sh->loaded) sh->failed = true; return; }
    /* keep the place after a reload (after a playback) */
    int keep_season = sh->n_seasons ? sh->seasons[sh->ssel] : -1;
    char keep_rk[32] = "";
    if (sh->esel < sh->n_sidx && sh->eps[sh->sidx[sh->esel]].rating_key)
        snprintf(keep_rk, sizeof keep_rk, "%s", sh->eps[sh->sidx[sh->esel]].rating_key);
    for (int i = 0; i < sh->n_eps; i++) pitem_free(&sh->eps[i]);
    free(sh->eps);
    free(sh->sidx);
    sh->n_eps = parse_items(mc, &sh->eps, r->srv);
    sh->sidx = calloc(sh->n_eps + 1, sizeof(int));
    sh->n_seasons = 0;
    for (int pass = 0; pass < 2; pass++) /* the seasons in order, specials last */
        for (int i = 0; i < sh->n_eps; i++) {
            int s = sh->eps[i].parent_index;
            if ((s == 0) != (pass == 1) || show_season_of(sh, s) >= 0 || sh->n_seasons >= SHOW_MAX_SEASONS) continue;
            sh->seasons[sh->n_seasons++] = s;
        }
    sh->next = pick_next_ep(sh->eps, sh->n_eps);
    sh->loaded = true;
    if (!sh->placed) { /* first time: the season asked for, else the opened episode's, else the next episode's */
        sh->placed = true;
        int season = sh->want_season >= 0 ? sh->want_season : sh->next >= 0 ? sh->eps[sh->next].parent_index : -1;
        int si = show_season_of(sh, season);
        show_pick_season(sh, si < 0 ? 0 : si);
        int pos = show_ep_pos(sh, sh->focus_rk);
        if (pos < 0 && sh->next >= 0) pos = show_ep_pos(sh, sh->eps[sh->next].rating_key);
        sh->esel = pos < 0 ? 0 : pos;
    } else {
        int si = show_season_of(sh, keep_season);
        show_pick_season(sh, si < 0 ? 0 : si);
        int pos = show_ep_pos(sh, keep_rk);
        sh->esel = pos < 0 ? 0 : pos;
    }
    if (sh->esel >= sh->n_sidx) sh->esel = sh->n_sidx ? sh->n_sidx - 1 : 0;
}

static void show_fetch(Show *sh) {
    sh->gen = play_gen;
    char *p = str_printf("/library/metadata/%s?includeImages=1", sh->key);
    if (!sh->req) sh->req = srv_get(sh->srv, p, show_info_cb, sh);
    free(p);
    p = str_printf("/library/metadata/%s/allLeaves", sh->key);
    if (!sh->ereq) sh->ereq = srv_get(sh->srv, p, show_eps_cb, sh);
    free(p);
    if (!sh->ereq && !sh->loaded) sh->failed = true;
}
static void show_enter(Screen *s) { show_fetch(s->st); }
static void show_leave(Screen *s) {
    Show *sh = s->st;
    if (sh->req) { sh->req->cancelled = true; sh->req = NULL; }
    if (sh->ereq) { sh->ereq->cancelled = true; sh->ereq = NULL; }
}
static void show_destroy(Screen *s) {
    Show *sh = s->st;
    if (sheet_show == sh) sheet_show = NULL;
    if (sh->have_show) pitem_free(&sh->show);
    for (int i = 0; i < sh->n_eps; i++) pitem_free(&sh->eps[i]);
    free(sh->eps);
    free(sh->sidx);
    free(sh);
    free(s);
}

static void season_label(int season, char *buf, size_t n) {
    if (season == 0) snprintf(buf, n, "%s", T("Specials"));
    else snprintf(buf, n, T("Season %d"), season);
}

/* every episode of the series (season = -1) or of one season is watched */
static bool show_seen(const Show *sh, int season, int *n_out, int *seen_out) {
    int n = 0, seen = 0;
    for (int i = 0; i < sh->n_eps; i++) {
        if (season >= 0 && sh->eps[i].parent_index != season) continue;
        n++;
        if (sh->eps[i].view_count > 0 && sh->eps[i].offset <= 0) seen++;
    }
    if (n_out) *n_out = n;
    if (seen_out) *seen_out = seen;
    return n > 0 && seen == n;
}

/* buttons: [Continue S8 E4 | Play S1 E1] [Start over] [Mark as watched|unwatched] */
static int show_buttons(Show *sh, BtnKind *k) {
    int n = 0;
    if (sh->next >= 0) {
        if (sh->eps[sh->next].offset > 30) { k[n++] = B_RESUME; k[n++] = B_START; }
        else k[n++] = B_PLAY;
    }
    if (sh->n_eps && sh->key[0]) k[n++] = B_SEEN; /* the whole series */
    return n;
}

/* the green key on the series page: the series (buttons), the chosen season (pills) or the episode in focus */
static void show_sheet_play(void *ud) { /* "Play" in an episode's sheet */
    Show *sh = ud;
    if (sh != sheet_show || sh->row != 2 || sh->esel >= sh->n_sidx) return;
    play_with_resume(&sh->eps[sh->sidx[sh->esel]], NULL);
}
static void show_sheet_eps(void *ud) { /* "Episodes" in a season's sheet */
    Show *sh = ud;
    if (sh == sheet_show && sh->n_sidx) sh->row = 2;
}
/* the sheet for what has focus: an episode (Play first), a season (Episodes first) or the series (only marks) */
static void show_sheet(Show *sh, bool with_first) {
    sheet_show = sh;
    if (sh->row == 2 && sh->esel < sh->n_sidx) {
        watch_sheet(&sh->eps[sh->sidx[sh->esel]], false, with_first ? T("Play") : NULL, show_sheet_play, sh);
        return;
    }
    PItem g = {0};
    char lbl[64];
    g.srv = sh->srv;
    if (sh->row == 1 && sh->ssel < sh->n_seasons && sh->n_sidx) {
        g.type = "season";
        g.rating_key = sh->eps[sh->sidx[0]].parent_key;
        season_label(sh->seasons[sh->ssel], lbl, sizeof lbl);
        g.title = lbl;
        show_seen(sh, sh->seasons[sh->ssel], &g.leaves, &g.viewed_leaves);
        watch_sheet(&g, false, with_first ? T("Episodes") : NULL, show_sheet_eps, sh); /* g only borrows strings */
        return;
    }
    g.type = "show";
    g.rating_key = sh->key;
    g.title = sh->have_show ? sh->show.title : NULL;
    show_seen(sh, -1, &g.leaves, &g.viewed_leaves);
    watch_menu(&g, false);
}
static void show_watch_menu(Show *sh) { show_sheet(sh, false); }

#define SHOW_PILL_Y 610.0f
#define SHOW_EP_Y 712.0f
#define SHOW_EP_W 384.0f
#define SHOW_EP_H 216.0f
#define SHOW_EP_GAP 28.0f

static float show_pill_w(int season) {
    char l[64];
    season_label(season, l, sizeof l);
    return text_width(F_SEMI, 26, l) + 60;
}

static void show_update(Screen *s, float dt) {
    Show *sh = s->st;
    if (sh->gen != play_gen) show_fetch(sh); /* after a playback: watched marks, progress and the next episode */
    float avail = UI_W - 240;
    float px = 0, target = 0;
    for (int i = 0; i < sh->n_seasons; i++) {
        float w = show_pill_w(sh->seasons[i]);
        if (i == sh->ssel && px + w > avail) target = px + w - avail;
        px += w + 16;
    }
    sh->sx_s = anim_to(sh->sx_s, target, dt, 12);
    float sel = sh->esel * (SHOW_EP_W + SHOW_EP_GAP);
    target = sel > avail - SHOW_EP_W ? sel - (avail - SHOW_EP_W) : 0;
    sh->sx_e = anim_to(sh->sx_e, target, dt, 12);
}

static void show_draw(Screen *s) {
    Show *sh = s->st;
    PItem *info = sh->have_show ? &sh->show : NULL;
    PItem *fe = sh->row == 2 && sh->esel < sh->n_sidx ? &sh->eps[sh->sidx[sh->esel]] : NULL;
    draw_background();
    const PItem *bgi = info ? info : sh->n_eps ? &sh->eps[0] : NULL;
    if (bgi) {
        const char *art = bgi == info ? bgi->art : bgi->sart;
        char *u = img_url(sh->srv, art, 1920, 1080);
        if (img_draw_cover(img_get(u), 0, 0, UI_W, UI_H, 0, 0.5f)) {
            fill_hgrad(0, 0, UI_W * 0.8f, UI_H, RGBA(8, 9, 13, 250), RGBA(8, 9, 13, 0));
            fill_vgrad(0, UI_H * 0.4f, UI_W, UI_H * 0.6f, RGBA(8, 9, 13, 0), RGBA(8, 9, 13, 245));
        }
        free(u);
    }
    float x = 120;
    /* top: the series, or the episode in focus */
    if (fe) {
        PItem tmp = *fe;
        if (info) tmp.clogo = info->clogo;
        draw_info(&tmp, x, 90, 1, 3, false);
    } else if (info) {
        if (sh->n_seasons > 0) { /* "2024 · 3 seasons" in the metadata line */
            char *y0 = info->year, sn[80];
            int ns = 0;
            for (int i = 0; i < sh->n_seasons; i++) if (sh->seasons[i] > 0) ns++;
            snprintf(sn, sizeof sn, "%s%s%s", y0 ? y0 : "", y0 ? "  ·  " : "", "");
            snprintf(sn + strlen(sn), sizeof sn - strlen(sn), ns == 1 ? T("1 season") : T("%d seasons"), ns);
            info->year = sn;
            int lv = info->leaves;
            info->leaves = 0;
            draw_info(info, x, 90, 1, 3, false);
            info->year = y0;
            info->leaves = lv;
        } else draw_info(info, x, 90, 1, 3, false);
    }
    if (!sh->loaded) {
        if (sh->failed) text_draw(F_REG, 30, x, SHOW_PILL_Y, C_TEXT2, AL_LEFT, T("Could not load from Plex"));
        else draw_spinner(UI_W / 2, SHOW_EP_Y + 60, 30, C_TEXT2);
        return;
    }
    /* buttons */
    BtnKind k[MAX_BTNS];
    int nb = show_buttons(sh, k);
    if (sh->bsel >= nb) sh->bsel = nb ? nb - 1 : 0;
    float bx = x, by = 500;
    for (int i = 0; i < nb; i++) {
        const PItem *ne = sh->next >= 0 ? &sh->eps[sh->next] : NULL;
        char lbl[96], dur[32];
        if (k[i] == B_SEEN) snprintf(lbl, sizeof lbl, "%s", show_seen(sh, -1, NULL, NULL) ? T("Mark as unwatched") : T("Mark as watched"));
        else if (!ne) continue;
        else if (k[i] == B_RESUME) snprintf(lbl, sizeof lbl, T("Continue S%d E%d (%s)"), ne->parent_index, ne->index, fmt_duration(ne->offset, dur, sizeof dur));
        else if (k[i] == B_PLAY) snprintf(lbl, sizeof lbl, T("Play S%d E%d"), ne->parent_index, ne->index);
        else snprintf(lbl, sizeof lbl, "%s", T("Start over"));
        bool f = sh->row == 0 && sh->bsel == i;
        float w = text_width(F_SEMI, 28, lbl) + 110;
        fill_rrect(bx, by, w, 76, 38, f ? C_FOCUS : RGBA(255, 255, 255, 40));
        draw_icon(k[i] == B_SEEN ? ICON_CHECK : ICON_PLAY, bx + 42, by + 38, 26, f ? RGB(10, 10, 10) : C_TEXT);
        text_draw(F_SEMI, 28, bx + 70, by + 20, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, lbl);
        bx += w + 24;
    }
    /* seasons */
    clip_push(0, SHOW_PILL_Y - 10, UI_W, 80);
    float px = x - sh->sx_s;
    for (int i = 0; i < sh->n_seasons; i++) {
        char l[64];
        season_label(sh->seasons[i], l, sizeof l);
        float w = show_pill_w(sh->seasons[i]);
        bool chosen = i == sh->ssel, f = sh->row == 1 && chosen;
        if (px + w > 0 && px < UI_W) {
            fill_rrect(px, SHOW_PILL_Y, w, 58, 29, f ? C_FOCUS : chosen ? RGBA(255, 255, 255, 70) : RGBA(255, 255, 255, 22));
            text_draw(F_SEMI, 26, px + w / 2, SHOW_PILL_Y + 14, f ? RGB(10, 10, 10) : chosen ? C_TEXT : C_TEXT2, AL_CENTER, l);
        }
        px += w + 16;
    }
    clip_pop();
    /* episodes of the chosen season */
    clip_push(0, SHOW_EP_Y - 30, UI_W, UI_H - SHOW_EP_Y + 30);
    for (int i = 0; i < sh->n_sidx; i++) {
        PItem *e = &sh->eps[sh->sidx[i]];
        float ex = x + i * (SHOW_EP_W + SHOW_EP_GAP) - sh->sx_e, ey = SHOW_EP_Y;
        if (ex > UI_W || ex + SHOW_EP_W < 0) continue;
        bool f = sh->row == 2 && sh->esel == i;
        float fa = ui_focus(e, f), sc = 1 + 0.06f * fa, w = SHOW_EP_W * sc, h = SHOW_EP_H * sc;
        float xx = ex - (w - SHOW_EP_W) / 2, yy = ey - (h - SHOW_EP_H) / 2;
        ui_focus_under(xx, yy, w, h, 14, fa);
        fill_rrect(xx, yy, w, h, 14, C_SURFACE);
        char *u = img_url(e->srv, e->art, 640, 360);
        if (!img_draw_cover(img_get(u), xx, yy, w, h, 14, 1)) draw_icon(ICON_TV, xx + w / 2, yy + h / 2, 60, C_TEXT3);
        free(u);
        if (e->duration > 0 && e->offset > 0) {
            fill_rrect(xx + 12, yy + h - 14, w - 24, 6, 3, RGBA(0, 0, 0, 150));
            fill_rrect(xx + 12, yy + h - 14, (w - 24) * (float)(e->offset / e->duration), 6, 3, RGB(229, 160, 13));
        } else if (!e->view_count) fill_tri(xx + w - 34, yy, xx + w, yy, xx + w, yy + 34, RGB(229, 160, 13));
        ui_focus_over(xx, yy, w, h, 14, fa);
        char t[300];
        snprintf(t, sizeof t, "%d. %s", e->index, e->title ? e->title : "");
        text_draw_clip(F_SEMI, 24, ex, ey + SHOW_EP_H + 14, SHOW_EP_W, f ? C_TEXT : C_TEXT2, AL_LEFT, t);
        if (e->duration > 0) {
            char d[32];
            snprintf(d, sizeof d, T("%d min"), (int)(e->duration / 60 + 0.5));
            text_draw(F_REG, 22, ex, ey + SHOW_EP_H + 46, C_TEXT3, AL_LEFT, d);
        }
    }
    clip_pop();
    if (sh->row > 0) draw_hint(x, UI_H - 62, "OK", sh->row == 1 ? T("Episodes, mark the season as watched") : T("Play, mark as watched"));
}

static bool show_input(Screen *s, const InputEvent *ev) {
    Show *sh = s->st;
    if (!sh->loaded) return false;
    BtnKind k[MAX_BTNS];
    int nb = show_buttons(sh, k);
    switch (ev->a) {
    case A_UP:
        if (sh->row > 0) sh->row--;
        if (sh->row == 0 && !nb) sh->row = 1;
        return true;
    case A_DOWN:
        if (sh->row < 2 && (sh->row < 1 || sh->n_sidx)) { sh->row++; return true; }
        return true;
    case A_LEFT:
        if (sh->row == 0 && sh->bsel > 0) sh->bsel--;
        else if (sh->row == 1 && sh->ssel > 0) { show_pick_season(sh, sh->ssel - 1); sh->esel = 0; sh->sx_e = 0; }
        else if (sh->row == 2 && sh->esel > 0) sh->esel--;
        return true;
    case A_RIGHT:
        if (sh->row == 0 && sh->bsel < nb - 1) sh->bsel++;
        else if (sh->row == 1 && sh->ssel < sh->n_seasons - 1) { show_pick_season(sh, sh->ssel + 1); sh->esel = 0; sh->sx_e = 0; }
        else if (sh->row == 2 && sh->esel < sh->n_sidx - 1) sh->esel++;
        return true;
    case A_OK: case A_PLAYPAUSE: case A_PLAY:
        if (ev->repeat) return true;
        if (sh->row == 0 && nb && k[sh->bsel] == B_SEEN) {
            if (ev->a != A_OK) return true;
            watch_do(sh->srv, sh->key, show_seen(sh, -1, NULL, NULL) ? WA_UNSEEN : WA_SEEN);
        } else if (sh->row == 0 && nb && sh->next >= 0) {
            PItem *ne = &sh->eps[sh->next];
            plex_play_chain(ne, k[sh->bsel] == B_RESUME ? ne->offset : 0, NULL);
        } else if (sh->row == 1 || (sh->row == 2 && sh->esel < sh->n_sidx)) {
            /* a season or an episode: its sheet (Episodes / Play first); Play keys play at once */
            if (ev->a == A_OK) show_sheet(sh, true);
            else if (sh->row == 2) play_with_resume(&sh->eps[sh->sidx[sh->esel]], NULL);
        }
        return true;
    case A_INFO: if (!ev->repeat) show_watch_menu(sh); return true;
    default: return false;
    }
}

static Screen *show_screen(int srv, const char *show_key, const char *focus_rk, int season) {
    Screen *s = calloc(1, sizeof *s);
    Show *sh = calloc(1, sizeof *sh);
    sh->srv = srv;
    snprintf(sh->key, sizeof sh->key, "%s", show_key ? show_key : "");
    snprintf(sh->focus_rk, sizeof sh->focus_rk, "%s", focus_rk ? focus_rk : "");
    sh->want_season = season;
    sh->next = -1;
    s->name = "plex-show";
    s->st = sh;
    s->enter = show_enter;
    s->leave = show_leave;
    s->update = show_update;
    s->draw = show_draw;
    s->input = show_input;
    s->destroy = show_destroy;
    return s;
}

Screen *plex_screen(void) {
    plex_client_id();
    srv_load();
    if (!*cfg_get("plex_token", "") || n_enabled() == 0) return link_screen();
    return browse_screen("Plex", NULL, false, -1);
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
/* the server conversion only matters when the TV gets no HDR signal (no HDR, or HDR turned off) */
static const char *plex_hdr_locked(void) { return cfg_int("hdr", 1) && display_hdr_ok() ? T("Not needed - the TV shows HDR") : NULL; }
static const char *const plexst_o[] = {"hero", "focus", NULL}, *const plexst_l[] = {"Hero", N_("Focus"), NULL};
static const char *const plexsub_o[] = {"plex", "player", "off", NULL}, *const plexsub_l[] = {N_("Follow Plex"), N_("Language setting"), N_("Off"), NULL};
static void act_plex_logout(void) { app_ask_signout("Plex", plex_forget); }
static const char *plex_signed_out(void) { return *cfg_get("plex_token", "") ? NULL : ITEM_HIDDEN; }
static const Item plex_items[] = {
    {N_("Plex subtitles"), IT_CYCLE, "plex_subs", "plex", plexsub_o, plexsub_l,
     .help = N_("Follow Plex: the subtitle track chosen in Plex, otherwise none. Language setting: the player picks by Subtitle language under Picture and sound. Off: never")},
    {N_("Home screen (Plex and Jellyfin)"), IT_CYCLE, "plex_home_style", "hero", plexst_o, plexst_l, .help = N_("Hero: a featured title at the top. Focus: background and info follow the selected title")},
    {N_("Server converts HDR movies"), IT_TOGGLE, "plex_hdr_server", "0", set_onoff,
     .help = N_("Only when the TV shows HDR movies without HDR: the Plex server converts them instead of the box (1080p, stereo sound). Needs a powerful server"),
     .locked = plex_hdr_locked},
    {N_("Plex server"), IT_INFO, .info_cmd = "plex-name", .locked = plex_signed_out},
    {N_("Sign out of Plex"), IT_ACTION, .action = act_plex_logout, .locked = plex_signed_out},
};

static void plex_row_open(const HomeItem *it, int idx) { (void)idx; if (it->key) plex_play_key(it->key); }
static void plex_app_apply(void) {
    plex_client_id(); /* made here on the main thread, never first from a worker */
}
const NotaApp nota_app = {
    NOTA_API, "plex", "Plex", N_("Movies and shows from your Plex server"), APP_VERSION, 20, 0,
    ICON_PLEX, {40, 40, 44, 255}, {22, 22, 26, 255}, {229, 160, 13, 255},
    .open = plex_screen, .init = plex_app_apply, .apply = plex_app_apply, .forget = plex_forget,
    .settings = plex_items, .n_settings = sizeof plex_items / sizeof plex_items[0],
    .row_title = N_("Continue watching"), .shelf_label = N_("CONTINUE IN PLEX"),
    .row = plex_ondeck, .row_refresh = plex_refresh_ondeck, .row_open = plex_row_open,
};
