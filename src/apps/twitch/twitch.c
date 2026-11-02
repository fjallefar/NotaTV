/* NotaTV app package "twitch" - Twitch: followed channels, popular streams, categories and search via the Helix API.
 * Login with the Device Code Flow (twitch.tv/activate) as a "Public" client: no secret key on the box.
 * Playback via mpv/yt-dlp. Built as app.so (see src/apps/README.md); the descriptor nota_app is at the bottom. */
#include "tv.h"
#include "vendor/cJSON.h"
#include <curl/curl.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define TW_ACCENT RGB(145, 70, 255)
#define TW_SCOPES "user:read:follows"
#define GCOLS 4
#define TILE_W 384
#define TILE_H 216
#define CAT_W 200
#define CAT_H 266

static const char *tw_client(void) { return cfg_get("tw_client", TW_CLIENT_ID); } /* TW_CLIENT_ID: see app.mk */
static const char *tw_token(void) { return cfg_get("tw_token", ""); }

/* ---------- HTTP ---------- */
typedef struct TReq TReq;
struct TReq {
    char *url, *body;
    const char *method;
    bool form, retried;
    cJSON *json;
    long status;
    int token;
    void (*done)(void *ud, cJSON *json, long status);
    void *ud;
};
static int tw_gen; /* increased when the view changes: replies for old views are ignored */

static void treq_work(void *arg) {
    TReq *r = arg;
    char auth[300], cid[120];
    snprintf(cid, sizeof cid, "Client-Id: %s", tw_client());
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", tw_token());
    const char *hdr_api[] = {"Accept: application/json", cid, auth, NULL};
    const char *hdr_form[] = {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded", NULL};
    HttpBuf b;
    if (http_request(r->method, r->url, r->form ? hdr_form : hdr_api, r->body, &b, 20)) {
        r->status = b.status;
        if (b.data) r->json = cJSON_Parse(b.data);
    }
    http_free(&b);
}

static void tw_submit(TReq *r);
static void refresh_then(TReq *r);

static void treq_free(TReq *r) {
    cJSON_Delete(r->json);
    free(r->url);
    free(r->body);
    free(r);
}

static void treq_done(void *arg) {
    TReq *r = arg;
    /* the access token lasts about 4 hours: renew it once and try again */
    if (r->status == 401 && !r->form && !r->retried && *cfg_get("tw_refresh", "")) { refresh_then(r); return; }
    if (r->done && (r->token == tw_gen || r->token < 0)) r->done(r->ud, r->json, r->status);
    treq_free(r);
}

static void tw_submit(TReq *r) { job_submit(treq_work, treq_done, r); }

static void tw_api(const char *path, void (*done)(void *, cJSON *, long), void *ud) {
    TReq *r = calloc(1, sizeof *r);
    r->method = "GET";
    r->url = str_printf("https://api.twitch.tv/helix%s", path);
    r->done = done;
    r->ud = ud;
    r->token = tw_gen;
    tw_submit(r);
}

static void tw_form(const char *url, const char *body, void (*done)(void *, cJSON *, long), void *ud) {
    TReq *r = calloc(1, sizeof *r);
    r->method = "POST";
    r->url = str_dup(url);
    r->body = str_dup(body);
    r->form = true;
    r->done = done;
    r->ud = ud;
    r->token = tw_gen;
    tw_submit(r);
}

static char *js(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? str_dup(v->valuestring) : NULL;
}

/* renewal: the refresh token can only be used once, so both are saved again */
static void refreshed(void *ud, cJSON *json, long status) {
    TReq *orig = ud;
    char *at = status == 200 ? js(json, "access_token") : NULL, *rt = status == 200 ? js(json, "refresh_token") : NULL;
    if (at) {
        cfg_set("tw_token", at);
        if (rt) cfg_set("tw_refresh", rt);
        orig->retried = true;
        orig->status = 0;
        cJSON_Delete(orig->json);
        orig->json = NULL;
        tw_submit(orig);
    } else {
        cfg_set("tw_token", ""); /* expired: sign in again */
        cfg_set("tw_refresh", "");
        if (orig->done && orig->token == tw_gen) orig->done(orig->ud, NULL, 401);
        treq_free(orig);
    }
    free(at);
    free(rt);
}
static void refresh_then(TReq *r) {
    char *rt = url_encode(cfg_get("tw_refresh", ""));
    char *body = str_printf("grant_type=refresh_token&refresh_token=%s&client_id=%s", rt, tw_client());
    TReq *f = calloc(1, sizeof *f);
    f->method = "POST";
    f->url = str_dup("https://id.twitch.tv/oauth2/token");
    f->body = body;
    f->form = true;
    f->done = refreshed;
    f->ud = r;
    f->token = -1; /* always delivered: otherwise the original request is lost */
    tw_submit(f);
    free(rt);
}

/* ---------- elementer ---------- */
enum { K_STREAM, K_GAME, K_CHANNEL };
typedef struct {
    int kind;
    char *id, *login, *name, *title, *game, *thumb;
    int viewers;
    bool live;
} TItem;

static void titem_free(TItem *t) {
    free(t->id); free(t->login); free(t->name); free(t->title); free(t->game); free(t->thumb);
}

/* Twitch images have {width}x{height} in the address */
static char *sized(const char *u, int w, int h) {
    if (!u) return NULL;
    char *o = str_dup(u), *p;
    char wh[32];
    snprintf(wh, sizeof wh, "%dx%d", w, h);
    if ((p = strstr(o, "{width}x{height}"))) {
        char *n = str_printf("%.*s%s%s", (int)(p - o), o, wh, p + 16);
        free(o);
        o = n;
    } else if ((p = strstr(o, "%{width}x%{height}"))) {
        char *n = str_printf("%.*s%s%s", (int)(p - o), o, wh, p + 18);
        free(o);
        o = n;
    }
    return o;
}

static void fmt_viewers(int n, char *buf, size_t sz) {
    if (n >= 1000000) snprintf(buf, sz, T("%d.%dM viewers"), n / 1000000, (n / 100000) % 10);
    else if (n >= 1000) snprintf(buf, sz, T("%d,%03d viewers"), n / 1000, n % 1000);
    else snprintf(buf, sz, T("%d viewers"), n);
}

/* ---------- screen ---------- */
enum { V_FOLLOW, V_TOP, V_CATS, V_GAME, V_SEARCH };
enum { S_LOGIN, S_CODE, S_LOADING, S_READY, S_ERROR };
typedef struct {
    int state, view;
    char msg[200];
    TItem *it;
    int n, cap;
    bool loading;
    int row, col;
    float sy;
    char heading[200];
    SideMenu side;
    int side_map[8]; /* menu item -> M_* (the menu depends on whether you are signed in) */
    /* login */
    char device_code[128], user_code[32], verify_uri[256];
    double poll_next, code_expires, interval;
    bool polling;
} Tw;

static Tw *live;
static void build_side(Tw *v);

static void clear_items(Tw *v) {
    for (int i = 0; i < v->n; i++) titem_free(&v->it[i]);
    free(v->it);
    v->it = NULL;
    v->n = v->cap = 0;
    v->row = v->col = 0;
    v->sy = 0;
}
static TItem *push_item(Tw *v) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 32; v->it = realloc(v->it, sizeof(TItem) * v->cap); }
    TItem *t = &v->it[v->n++];
    memset(t, 0, sizeof *t);
    return t;
}

static void list_done(void *ud, cJSON *json, long status) {
    Tw *v = ud;
    if (v != live) return;
    v->loading = false;
    if (status == 401) { v->state = S_LOGIN; build_side(v); snprintf(v->msg, sizeof v->msg, T("Login expired - sign in again")); return; }
    if (status != 200 || !json) { snprintf(v->msg, sizeof v->msg, status ? T("Twitch responded with error %ld") : T("Cannot reach Twitch"), status); return; }
    v->msg[0] = 0;
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(json, "data")) {
        TItem *t = push_item(v);
        if (v->view == V_CATS) {
            t->kind = K_GAME;
            t->id = js(o, "id");
            t->name = js(o, "name");
            t->thumb = sized(cJSON_GetStringValue(cJSON_GetObjectItem(o, "box_art_url")), 285, 380);
        } else if (v->view == V_SEARCH) {
            t->kind = K_CHANNEL;
            t->login = js(o, "broadcaster_login");
            t->name = js(o, "display_name");
            t->title = js(o, "title");
            t->game = js(o, "game_name");
            t->thumb = js(o, "thumbnail_url"); /* profilbillede */
            t->live = cJSON_IsTrue(cJSON_GetObjectItem(o, "is_live"));
        } else {
            t->kind = K_STREAM;
            t->login = js(o, "user_login");
            t->name = js(o, "user_name");
            t->title = js(o, "title");
            t->game = js(o, "game_name");
            t->thumb = sized(cJSON_GetStringValue(cJSON_GetObjectItem(o, "thumbnail_url")), 640, 360);
            cJSON *vc = cJSON_GetObjectItem(o, "viewer_count");
            t->viewers = cJSON_IsNumber(vc) ? vc->valueint : 0;
            t->live = true;
        }
        if (!t->id && !t->login) { titem_free(t); v->n--; }
    }
    if (v->view == V_SEARCH) { /* live channels first */
        int w = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = w; i < v->n; i++)
                if (v->it[i].live == (pass == 0)) { TItem t = v->it[i]; memmove(&v->it[w + 1], &v->it[w], sizeof(TItem) * (i - w)); v->it[w++] = t; }
    }
}

static void show(Tw *v, int view, const char *heading, const char *path) {
    tw_gen++;
    clear_items(v);
    v->view = view;
    v->loading = true;
    v->msg[0] = 0;
    snprintf(v->heading, sizeof v->heading, "%s", heading);
    tw_api(path, list_done, v);
}

static void show_follow(Tw *v) {
    char p[160];
    snprintf(p, sizeof p, "/streams/followed?user_id=%s&first=100", cfg_get("tw_uid", ""));
    show(v, V_FOLLOW, T("Followed channels - live now"), p);
}
static void show_top(Tw *v) { show(v, V_TOP, T("Trending now"), "/streams?first=60"); }
static void show_cats(Tw *v) { show(v, V_CATS, T("Categories"), "/games/top?first=60"); }
static void show_game(Tw *v, const char *id, const char *name) {
    char p[160];
    snprintf(p, sizeof p, "/streams?game_id=%s&first=60", id);
    show(v, V_GAME, name ? name : T("Category"), p);
}

static void search_done(void *ud, const char *q) {
    Tw *v = ud;
    if (!q || v != live) return;
    char *t = str_dup(q);
    str_trim(t);
    if (*t) {
        char *e = url_encode(t), *p = str_printf("/search/channels?query=%s&first=40", e), *h = str_printf(T("Search: %s"), t);
        show(v, V_SEARCH, h, p);
        free(e); free(p); free(h);
    }
    free(t);
}

/* ---------- login (Device Code Flow) ---------- */
static void me_done(void *ud, cJSON *json, long status) {
    Tw *v = ud;
    if (v != live) return;
    cJSON *u = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "data"), 0);
    char *id = status == 200 ? js(u, "id") : NULL;
    if (!id) { v->state = S_ERROR; snprintf(v->msg, sizeof v->msg, T("Could not load your Twitch profile (%ld)"), status); return; }
    char *login = js(u, "login"), *name = js(u, "display_name");
    cfg_set("tw_uid", id);
    cfg_set("tw_login", login ? login : "");
    cfg_set("tw_name", name ? name : (login ? login : ""));
    free(id); free(login); free(name);
    v->state = S_READY;
    build_side(v);
    show_follow(v);
}

static void tw_connect(Tw *v) {
    if (!*tw_token()) { v->state = S_LOGIN; return; }
    v->state = S_LOADING;
    snprintf(v->msg, sizeof v->msg, T("Connecting to Twitch…"));
    tw_gen++;
    tw_api("/users", me_done, v);
}

static void token_done(void *ud, cJSON *json, long status) {
    Tw *v = ud;
    v->polling = false;
    if (v != live || v->state != S_CODE) return;
    char *at = status == 200 ? js(json, "access_token") : NULL;
    if (at) {
        char *rt = js(json, "refresh_token");
        cfg_set("tw_token", at);
        cfg_set("tw_refresh", rt ? rt : "");
        free(rt);
        free(at);
        app_toast(T("Signed in to Twitch"));
        tw_connect(v);
        return;
    }
    char *m = js(json, "message");
    if (m && !strcmp(m, "slow_down")) v->interval += 2;
    else if (!m || strcmp(m, "authorization_pending")) {
        v->state = S_ERROR;
        snprintf(v->msg, sizeof v->msg, T("Login failed: %s"), m ? m : T("no response"));
    }
    free(m);
}

static void device_done(void *ud, cJSON *json, long status) {
    Tw *v = ud;
    if (v != live) return;
    char *dc = status == 200 ? js(json, "device_code") : NULL, *uc = js(json, "user_code"), *uri = js(json, "verification_uri");
    if (dc && uc) {
        snprintf(v->device_code, sizeof v->device_code, "%s", dc);
        snprintf(v->user_code, sizeof v->user_code, "%s", uc);
        snprintf(v->verify_uri, sizeof v->verify_uri, "%s", uri ? uri : "https://www.twitch.tv/activate");
        cJSON *iv = cJSON_GetObjectItem(json, "interval"), *ex = cJSON_GetObjectItem(json, "expires_in");
        v->interval = cJSON_IsNumber(iv) ? iv->valuedouble : 5;
        v->code_expires = app_time() + (cJSON_IsNumber(ex) ? ex->valuedouble : 1800);
        v->poll_next = app_time() + v->interval;
        v->state = S_CODE;
    } else {
        char *m = js(json, "message");
        v->state = S_ERROR;
        snprintf(v->msg, sizeof v->msg, T("Could not start login: %s"), m ? m : (status ? T("error from Twitch") : T("no connection")));
        free(m);
    }
    free(dc); free(uc); free(uri);
}

static void start_login(Tw *v) {
    v->state = S_LOADING;
    snprintf(v->msg, sizeof v->msg, T("Getting login code…"));
    tw_gen++;
    char *b = str_printf("client_id=%s&scopes=%s", tw_client(), "user%3Aread%3Afollows");
    tw_form("https://id.twitch.tv/oauth2/device", b, device_done, v);
    free(b);
}

/* sign out: the token is revoked at Twitch, and everything about the login is forgotten here */
static void twitch_forget(void) {
    if (*tw_token()) {
        char *b = str_printf("client_id=%s&token=%s", tw_client(), tw_token());
        static const char *const hdr[] = {"Content-Type: application/x-www-form-urlencoded", NULL};
        HttpBuf r;
        http_request("POST", "https://id.twitch.tv/oauth2/revoke", hdr, b, &r, 4);
        http_free(&r);
        free(b);
    }
    cfg_set("tw_token", "");
    cfg_set("tw_refresh", "");
    cfg_set("tw_uid", "");
    cfg_set("tw_name", "");
    cfg_set("tw_login", "");
    wipe_session_traces();
}

static void logout_cb(void *ud, bool yes) {
    Tw *v = ud;
    if (!yes || v != live) return;
    twitch_forget();
    clear_items(v);
    v->state = S_LOGIN;
    build_side(v);
    app_toast(T("Signed out of Twitch"));
}

/* ---------- chat: anonymous reading via Twitch IRC (justinfan) in a background thread ---------- */
#define CHAT_Q 64
typedef struct {
    char chan[64];
    SDL_Mutex *mu;
    char *q[CHAT_Q]; /* "BBGGRR\tname\ttext" (ASS color) */
    int qn;
    SDL_AtomicInt stop;
} Chat; /* owned by the thread: freed when it stops */

typedef struct {        /* per playback (main thread) */
    char chan[64];
    Chat *conn;         /* NULL = chat turned off: no connection */
    double show_t;
    int shown;
} ChatSession;

static int chat_thread(void *arg);
static Chat *chat_start(const char *chan) {
    Chat *c = calloc(1, sizeof *c);
    snprintf(c->chan, sizeof c->chan, "%s", chan);
    c->mu = SDL_CreateMutex();
    SDL_Thread *th = SDL_CreateThread(chat_thread, "twitch-chat", c);
    if (!th) { SDL_DestroyMutex(c->mu); free(c); return NULL; }
    SDL_DetachThread(th); /* the thread cleans up itself */
    return c;
}
static void chat_stop(Chat *c) {
    if (c) SDL_SetAtomicInt(&c->stop, 1); /* do not touch it anymore: the thread frees it within ~1 s */
}

static void tag_value(const char *tags, const char *key, char *out, size_t n) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = tags; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            size_t l = strcspn(p + kl + 1, "; ");
            snprintf(out, n, "%.*s", (int)(l < n ? l : n - 1), p + kl + 1);
            return;
        }
    }
}

static void chat_line(Chat *c, char *line) {
    char *tags = NULL;
    if (line[0] == '@') { tags = line + 1; char *sp = strchr(line, ' '); if (!sp) return; *sp = 0; line = sp + 1; }
    char *pm = strstr(line, " PRIVMSG #");
    if (!pm) return;
    char *txt = strstr(pm, " :");
    if (!txt) return;
    txt += 2;
    if (!strncmp(txt, "\001ACTION ", 8)) { txt += 8; char *e = strchr(txt, '\001'); if (e) *e = 0; } /* /me */
    char name[64] = "", col[16] = "";
    if (tags) { tag_value(tags, "display-name", name, sizeof name); tag_value(tags, "color", col, sizeof col); }
    if (!name[0]) { /* :login!login@... */
        size_t l = strcspn(line + 1, "!");
        snprintf(name, sizeof name, "%.*s", (int)(l < sizeof name ? l : sizeof name - 1), line + 1);
    }
    char ass[8];
    if (strlen(col) == 7 && col[0] == '#') snprintf(ass, sizeof ass, "%.2s%.2s%.2s", col + 5, col + 3, col + 1);
    else { /* no chosen color: a fixed color from the name */
        static const char *pal[] = {"5A5AFF", "FF8C3C", "50C878", "FFB43C", "B464FF", "3CC8FF", "8CDCFF", "FF64C8"};
        unsigned h = 0;
        for (const char *p = name; *p; p++) h = h * 31 + (unsigned char)*p;
        snprintf(ass, sizeof ass, "%s", pal[h % 8]);
    }
    char *msg = str_printf("%s\t%s\t%s", ass, name, txt);
    SDL_LockMutex(c->mu);
    if (c->qn == CHAT_Q) { free(c->q[0]); memmove(c->q, c->q + 1, sizeof(char *) * (CHAT_Q - 1)); c->qn--; } /* busy chat: drop the oldest */
    c->q[c->qn++] = msg;
    SDL_UnlockMutex(c->mu);
}

/* the chat over TLS (port 6697; anonymous reading, but nothing goes over the network in clear text): libcurl
   makes the connection (CONNECT_ONLY) and then just moves bytes - no own TLS code */
static CURL *chat_connect(void) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, "https://irc.chat.twitch.tv:6697");
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (curl_easy_perform(c) != CURLE_OK) { curl_easy_cleanup(c); return NULL; }
    return c;
}
static bool chat_send(CURL *c, const char *s, size_t n) {
    size_t off = 0;
    for (int tries = 0; off < n && tries < 100; tries++) {
        size_t sent = 0;
        CURLcode r = curl_easy_send(c, s + off, n - off, &sent);
        if (r == CURLE_OK) { off += sent; continue; }
        if (r != CURLE_AGAIN) return false;
        SDL_Delay(10);
    }
    return off == n;
}

static int chat_thread(void *arg) {
    Chat *c = arg;
    while (!SDL_GetAtomicInt(&c->stop)) {
        CURL *h = chat_connect();
        if (!h) { for (int i = 0; i < 5 && !SDL_GetAtomicInt(&c->stop); i++) SDL_Delay(1000); continue; }
        curl_socket_t fd = CURL_SOCKET_BAD;
        curl_easy_getinfo(h, CURLINFO_ACTIVESOCKET, &fd);
        char hello[200];
        int hl = snprintf(hello, sizeof hello, "CAP REQ :twitch.tv/tags\r\nNICK justinfan%d\r\nJOIN #%s\r\n", 10000 + rand() % 80000, c->chan);
        if (fd == CURL_SOCKET_BAD || !chat_send(h, hello, (size_t)hl)) { curl_easy_cleanup(h); continue; }
        char buf[16384];
        size_t len = 0;
        while (!SDL_GetAtomicInt(&c->stop)) {
            size_t n = 0;
            CURLcode r = curl_easy_recv(h, buf + len, sizeof buf - 1 - len, &n);
            if (r == CURLE_AGAIN) { /* wait for data, but check the stop flag every second */
                struct pollfd p = {fd, POLLIN, 0};
                poll(&p, 1, 1000);
                continue;
            }
            if (r != CURLE_OK || n == 0) break; /* closed or an error: connect again */
            len += n;
            buf[len] = 0;
            char *p = buf, *e;
            while ((e = strstr(p, "\r\n"))) {
                *e = 0;
                if (!strncmp(p, "PING", 4)) { char pong[300]; int pl = snprintf(pong, sizeof pong, "PONG%s\r\n", p + 4); chat_send(h, pong, (size_t)pl); }
                else chat_line(c, p);
                p = e + 2;
            }
            len = strlen(p);
            memmove(buf, p, len + 1);
            if (len >= sizeof buf - 1) len = 0; /* unreasonably long line */
        }
        curl_easy_cleanup(h);
    }
    /* the thread owns the chat: clean up here, so the main thread never waits on a hanging connect() */
    for (int i = 0; i < c->qn; i++) free(c->q[i]);
    SDL_DestroyMutex(c->mu);
    free(c);
    return 0;
}

static void chat_tick(void *ud) {
    ChatSession *cs = ud;
    int want = cfg_int("tw_chat", 1) != 0; /* yellow in the player toggles tw_chat */
    if (want && !cs->conn) cs->conn = chat_start(cs->chan);
    else if (!want && cs->conn) { chat_stop(cs->conn); cs->conn = NULL; }
    if (want != cs->shown || app_time() - cs->show_t > 2) { /* the OSD script may have started after us: repeat */
        cs->shown = want;
        cs->show_t = app_time();
        player_message("notatv-chat-show", want ? "1" : "0");
    }
    Chat *c = cs->conn;
    if (!c) return;
    char *batch[8];
    int nb = 0;
    SDL_LockMutex(c->mu);
    while (c->qn && nb < 8) { batch[nb++] = c->q[0]; memmove(c->q, c->q + 1, sizeof(char *) * (c->qn - 1)); c->qn--; }
    SDL_UnlockMutex(c->mu);
    for (int i = 0; i < nb; i++) { player_message("notatv-chat", batch[i]); free(batch[i]); }
}

/* ---------- playback ---------- */
/* quality in the player menu, remembered in cfg tw_quality. Twitch sends H.264: the Pi decodes it in
   hardware up to 1080p, and Twitch rarely sends more */
static const char *const tw_qualities[] = {"1080p", "720p", "480p", "360p", NULL};

static void tw_play(const TItem *t) {
    if (!t->login) return;
    if (t->kind == K_CHANNEL && !t->live) { app_toast(T("%s is not live right now"), t->name ? t->name : t->login); return; }
    char *url = str_printf("https://www.twitch.tv/%s", t->login);
    char sub[300];
    snprintf(sub, sizeof sub, "%s%s%s", t->name ? t->name : t->login, t->game ? " · " : "", t->game ? t->game : "");
    char *poster = t->kind == K_STREAM ? sized(t->thumb, 640, 360) : NULL;
    ChatSession cs = {.shown = -1};
    snprintf(cs.chan, sizeof cs.chan, "%s", t->login);
    for (;;) {
        int h = atoi(cfg_get("tw_quality", "1080p"));
        if (h <= 0 || h > 1080) h = 1080;
        char quality[16], fmt[96];
        snprintf(quality, sizeof quality, "%dp", h);
        snprintf(fmt, sizeof fmt, "--ytdl-format=best[height<=%d]/best", h);
        const char *extra[] = {fmt, NULL};
        PlayRequest rq = {0};
        rq.url = url;
        rq.title = t->title && *t->title ? t->title : (t->name ? t->name : t->login);
        rq.subtitle = sub;
        rq.poster = poster;
        rq.extra = extra;
        rq.qualities = tw_qualities;
        rq.quality = quality;
        rq.chat = true;
        rq.tick = chat_tick;
        rq.ud = &cs;
        PlayResult res = player_run(&rq);
        if (res.quality[0] && strcmp(res.quality, quality)) { /* new quality: start the stream again */
            cfg_set("tw_quality", res.quality);
            app_toast(T("Quality: %s"), res.quality);
            continue;
        }
        break;
    }
    chat_stop(cs.conn);
    free(url);
    free(poster);
}

/* ---------- main screen ---------- */
enum { M_FOLLOW, M_TOP, M_CATS, M_SEARCH, M_LOGOUT, M_LOGIN };
static void build_side(Tw *v) {
    side_clear(&v->side);
    snprintf(v->side.subtitle, sizeof v->side.subtitle, "%s", *tw_token() ? cfg_get("tw_name", "") : "");
#define ADD(label, icon, gap, id) (v->side_map[side_add(&v->side, label, icon, gap)] = (id))
    if (!*tw_token()) {
        ADD(T("Sign in"), ICON_LOCK, false, M_LOGIN);
    } else {
        ADD(T("Following"), ICON_STAR, false, M_FOLLOW);
        ADD(T("Popular"), ICON_PLAY, false, M_TOP);
        ADD(T("Categories"), ICON_FOLDER, false, M_CATS);
        ADD(T("Search"), ICON_SEARCH, false, M_SEARCH);
        ADD(T("Sign out"), ICON_LOCK, true, M_LOGOUT);
    }
#undef ADD
    v->side.active = 0;
}

static void side_activate(Tw *v, int item) {
    int idx = item >= 0 && item < 8 ? v->side_map[item] : M_LOGIN;
    if (idx == M_LOGIN || v->state != S_READY) { if (v->state != S_LOADING && v->state != S_CODE) start_login(v); return; }
    if (idx != M_SEARCH && idx != M_LOGOUT) v->side.active = item;
    switch (idx) {
    case M_FOLLOW: show_follow(v); break;
    case M_TOP: show_top(v); break;
    case M_CATS: show_cats(v); break;
    case M_SEARCH: keyboard_open(T("Search Twitch"), "", false, search_done, v); break;
    case M_LOGOUT: confirm_open(T("Sign out of Twitch?"), T("You will need to sign in again via twitch.tv/activate."), T("Sign out"), T("Cancel"), logout_cb, v); break;
    default: break;
    }
}

static void tw_enter(Screen *s) {
    Tw *v = s->st;
    live = v;
    side_init(&v->side, "Twitch", ICON_PLAY, TW_ACCENT);
    build_side(v);
    tw_connect(v);
}

static void tw_resume(Screen *s) {
    Tw *v = s->st;
    live = v;
}

static void tw_destroy(Screen *s) {
    Tw *v = s->st;
    clear_items(v);
    if (live == v) live = NULL;
    tw_gen++;
    free(v);
    free(s);
}

static bool is_cats(const Tw *v) { return v->view == V_CATS; }
static int cols(const Tw *v) { return is_cats(v) ? 7 : GCOLS; }
static float cell_h(const Tw *v) { return is_cats(v) ? CAT_H + 90 : TILE_H + 120; }

static void tw_update(Screen *s, float dt) {
    Tw *v = s->st;
    side_update(&v->side, dt);
    if (v->state == S_CODE && !v->polling && app_time() >= v->poll_next) {
        if (app_time() > v->code_expires) { v->state = S_ERROR; snprintf(v->msg, sizeof v->msg, T("The code has expired - press OK for a new one")); return; }
        v->polling = true;
        v->poll_next = app_time() + v->interval;
        char *b = str_printf("client_id=%s&scopes=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
                             tw_client(), "user%3Aread%3Afollows", v->device_code);
        tw_form("https://id.twitch.tv/oauth2/token", b, token_done, v);
        free(b);
    }
    float t = (v->row - 1) * cell_h(v);
    v->sy = anim_to(v->sy, t > 0 ? t : 0, dt, 12);
}

static void draw_tile(const TItem *t, float x, float y, bool f) {
    bool cat = t->kind == K_GAME, chan = t->kind == K_CHANNEL;
    float w = cat ? CAT_W : TILE_W, h = cat ? CAT_H : TILE_H;
    float fa = ui_focus(t, f), sc = 1 + 0.06f * fa, ww = w * sc, hh = h * sc, xx = x - (ww - w) / 2, yy = y - (hh - h) / 2;
    ui_focus_under(xx, yy, ww, hh, 12, fa);
    fill_rrect(xx, yy, ww, hh, 12, C_SURFACE);
    if (chan) { /* search result: profile picture in the middle */
        fill_rrect(xx, yy, ww, hh, 12, color_mix(C_SURFACE, TW_ACCENT, 0.25f));
        float r = hh * 0.32f;
        if (!img_draw_cover(img_get(t->thumb), xx + ww / 2 - r, yy + hh / 2 - r, r * 2, r * 2, r, 1))
            fill_circle(xx + ww / 2, yy + hh / 2, r, C_SURFACE2);
    } else if (!img_draw_cover(img_get(t->thumb), xx, yy, ww, hh, 12, 1)) {
        text_draw_wrap(F_SEMI, 22, xx + 14, yy + 18, ww - 28, 3, 28, C_TEXT2, t->name ? t->name : "");
    }
    if (t->live && !cat) {
        float bw = text_width(F_BOLD, 18, "LIVE") + 18;
        fill_rrect(xx + 12, yy + 12, bw, 28, 6, RGB(235, 4, 0));
        text_draw(F_BOLD, 18, xx + 12 + bw / 2, yy + 15, C_TEXT, AL_CENTER, "LIVE");
        if (t->viewers > 0) {
            char vw[48];
            fmt_viewers(t->viewers, vw, sizeof vw);
            float vwid = text_width(F_SEMI, 18, vw) + 18;
            fill_rrect(xx + 12, yy + hh - 40, vwid, 28, 6, RGBA(0, 0, 0, 170));
            text_draw(F_SEMI, 18, xx + 12 + vwid / 2, yy + hh - 37, C_TEXT, AL_CENTER, vw);
        }
    }
    ui_focus_over(xx, yy, ww, hh, 12, fa);
    if (cat) {
        text_draw_clip(F_SEMI, 20, x, y + h + 14, w, f ? C_TEXT : C_TEXT2, AL_LEFT, t->name ? t->name : "");
        return;
    }
    text_draw_clip(F_SEMI, 22, x, y + h + 14, w, f ? C_TEXT : C_TEXT2, AL_LEFT, chan ? (t->name ? t->name : "") : (t->title ? t->title : ""));
    char sub[300];
    if (chan) snprintf(sub, sizeof sub, "%s", t->live ? (t->game ? t->game : "Live") : "Offline");
    else snprintf(sub, sizeof sub, "%s%s%s", t->name ? t->name : "", t->game ? " · " : "", t->game ? t->game : "");
    text_draw_clip(F_REG, 20, x, y + h + 44, w, C_TEXT3, AL_LEFT, sub);
}

static void tw_draw(Screen *s) {
    Tw *v = s->st;
    draw_background();
    draw_topbar(NULL);
    float x0 = SIDE_W + 60, cx = x0 + (UI_W - x0) / 2;
    if (v->state != S_READY) {
        draw_icon(ICON_PLAY, cx, 230, 110, TW_ACCENT);
        if (v->state == S_CODE) {
            text_draw(F_BOLD, 42, cx, 330, C_TEXT, AL_CENTER, T("Sign in with your phone"));
            text_draw(F_REG, 28, cx - 300, 420, C_TEXT2, AL_LEFT, T("1. Go to twitch.tv/activate"));
            text_draw(F_REG, 28, cx - 300, 470, C_TEXT2, AL_LEFT, T("   - or scan the QR code"));
            text_draw(F_REG, 28, cx - 300, 540, C_TEXT2, AL_LEFT, T("2. Enter the code:"));
            text_draw(F_BOLD, 64, cx - 300, 590, C_TEXT, AL_LEFT, v->user_code);
            draw_qr(v->verify_uri, cx + 200, 420, 260);
            draw_spinner(cx - 280, 760, 16, C_TEXT3);
            text_draw(F_REG, 22, cx - 250, 746, C_TEXT3, AL_LEFT, T("Waiting for you to approve…"));
        } else if (v->state == S_LOADING) {
            draw_spinner(cx, 480, 30, C_TEXT);
            text_draw(F_REG, 26, cx, 540, C_TEXT2, AL_CENTER, v->msg);
        } else {
            text_draw(F_BOLD, 42, cx, 330, C_TEXT, AL_CENTER, v->state == S_ERROR ? T("Something went wrong") : T("Sign in to Twitch"));
            text_draw_wrap(F_REG, 28, cx - 520, 400, 1040, 6, 40, C_TEXT2,
                           v->msg[0] ? v->msg : T("Watch the channels you follow, popular streams and categories.\n\nChoose Sign in - the box then shows a code that you enter at twitch.tv/activate on your phone."));
            const char *b[1] = {v->state == S_ERROR ? T("Try again") : T("Sign in")};
            draw_buttons(cx, 780, b, 1, v->side.open ? -1 : 0);
            text_draw(F_REG, 22, cx, UI_H - 70, C_TEXT3, AL_CENTER, T("Left: menu"));
        }
        side_draw(&v->side);
        return;
    }
    bool cf = !v->side.open;
    clip_push(SIDE_W, 120, UI_W, UI_H - 120);
    float top = 140 - v->sy;
    text_draw(F_BOLD, 44, x0, top - 70 + 20, C_TEXT, AL_LEFT, v->heading);
    if (v->loading && !v->n) draw_spinner(cx, UI_H / 2, 30, C_TEXT);
    else if (!v->n)
        text_draw(F_REG, 26, x0, top + 60, C_TEXT3, AL_LEFT,
                  v->msg[0] ? v->msg : v->view == V_FOLLOW ? T("None of the channels you follow are live right now") : T("Nothing found"));
    int nc = cols(v);
    float cw = is_cats(v) ? CAT_W + 28 : TILE_W + 30;
    for (int i = 0; i < v->n; i++) {
        float x = x0 + (i % nc) * cw, y = top + 40 + (i / nc) * cell_h(v);
        if (y > UI_H || y + cell_h(v) < 0) continue;
        draw_tile(&v->it[i], x, y, cf && v->row == i / nc && v->col == i % nc);
    }
    clip_pop();
    draw_keybar(x0, UI_H - 62, v->view == V_CATS ? T("Open") : T("Watch"), NULL, NULL, T("Search"), "Menu");
    side_draw(&v->side);
}

static bool tw_input(Screen *s, const InputEvent *ev) {
    Tw *v = s->st;
    int act;
    if (side_input(&v->side, ev, &act)) {
        if (act >= 0) side_activate(v, act);
        return true;
    }
    if (v->state != S_READY) {
        if (ev->a == A_OK && !ev->repeat && (v->state == S_LOGIN || v->state == S_ERROR)) { start_login(v); return true; }
        if (ev->a == A_LEFT || ev->a == A_MENU) { side_open(&v->side); return true; }
        return false;
    }
    int nc = cols(v), i = v->row * nc + v->col;
    switch (ev->a) {
    case A_UP: if (v->row > 0) v->row--; return true;
    case A_DOWN:
        if ((v->row + 1) * nc >= v->n) return false; /* bottom: the key bar */
        v->row++;
        if (v->row * nc + v->col >= v->n && v->n) v->col = (v->n - 1) % nc;
        return true;
    case A_LEFT:
        if (v->col > 0) v->col--;
        else side_open(&v->side);
        return true;
    case A_RIGHT: if (v->col < nc - 1 && i + 1 < v->n) v->col++; return true;
    case A_MENU: side_open(&v->side); return true;
    case A_SEARCH: keyboard_open(T("Search Twitch"), "", false, search_done, v); return true;
    case A_CHAR: {
        char init[8] = {(char)ev->ch, 0};
        keyboard_open(T("Search Twitch"), init, false, search_done, v);
        return true;
    }
    case A_OK: case A_PLAYPAUSE: case A_PLAY:
        if (ev->repeat || i >= v->n) return true;
        if (v->it[i].kind == K_GAME) {
            char id[32], name[200];
            snprintf(id, sizeof id, "%s", v->it[i].id);
            snprintf(name, sizeof name, "%s", v->it[i].name ? v->it[i].name : "");
            show_game(v, id, name);
        } else tw_play(&v->it[i]);
        return true;
    case A_BACK:
        if (v->view == V_GAME) { v->side.active = M_CATS; show_cats(v); return true; }
        if (v->view != V_FOLLOW) { v->side.active = M_FOLLOW; show_follow(v); return true; }
        return false;
    default: return false;
    }
}

static Screen *twitch_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    Tw *v = calloc(1, sizeof *v);
    s->name = "twitch";
    s->st = v;
    s->enter = tw_enter;
    s->resume = tw_resume;
    s->update = tw_update;
    s->draw = tw_draw;
    s->input = tw_input;
    s->destroy = tw_destroy;
    return s;
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
static void act_tw_logout(void) { app_ask_signout("Twitch", twitch_forget); }
static const char *tw_signed_out(void) { return *tw_token() ? NULL : ITEM_HIDDEN; }
static const Item tw_items[] = {
    {N_("Sign out of Twitch"), IT_ACTION, .action = act_tw_logout, .help = N_("Revokes the login at Twitch and deletes it from this box"),
     .locked = tw_signed_out},
};
const NotaApp nota_app = {
    NOTA_API, "twitch", "Twitch", N_("Live streams from the channels you follow and all of Twitch"), APP_VERSION, 50, 0,
    ICON_PLAY, {145, 70, 255, 255}, {88, 38, 170, 255}, {255, 255, 255, 255},
    .open = twitch_screen, .forget = twitch_forget,
    .settings = tw_items, .n_settings = sizeof tw_items / sizeof tw_items[0],
};
