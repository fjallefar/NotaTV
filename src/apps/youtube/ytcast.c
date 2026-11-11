/* NotaTV app package "youtube" - YouTube cast: the box as a YouTube screen ("Cast" in the YouTube app and "Link with TV code").
 * DIAL (SSDP 1900 + HTTP 8008) makes the box visible on the local network; the control itself goes over YouTube's
 * "lounge" protocol (www.youtube.com/api/lounge): a long-polling connection with commands from the phone
 * (setPlaylist, play, pause, seekTo …) and status back (nowPlaying, onStateChange). The video is played with
 * our own YouTube player (yt-dlp + mpv). Written from scratch; no third-party code. */
#define _GNU_SOURCE /* accept4, strcasestr */
#include "youtube.h"
#include "vendor/cJSON.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DIAL_PORT 8008
#define LOUNGE "https://www.youtube.com/api/lounge"
#define APP_NAME "NotaTV"

/* ---------- delt tilstand ---------- */
#define CAST_APPROVED_MAX 16
enum { Y_NONE, Y_PLAY, Y_PAUSE, Y_RESUME, Y_STOP, Y_SEEK, Y_NEXT, Y_PREV, Y_VOLUME };
typedef struct { int cmd; double arg; } YCmd;
typedef struct { char sc[32]; char k[10][24]; char v[10][256]; int n; } YMsg;

static SDL_Mutex *mu;
static SDL_Condition *out_cv;
static SDL_AtomicInt enabled;
static bool started;
static char name[128], screen_id[64], device_id[64], udn[64];
static struct {
    char token[512];          /* lounge-token */
    double token_exp;         /* epoch seconds */
    char sid[64], gsid[128];
    long aid, rid, ofs;
    YCmd q[32];
    int qn;
    /* playlist from the phone */
    char ids[200][16];
    int nids, cur;
    char list_id[64], ctt[256];
    double start;
    bool want_play;           /* the main thread must start ids[cur] from the beginning */
    /* playback */
    bool playing_now, paused;
    double pos, dur;
    int volume, state;        /* state: 1 playing, 2 paused, 3 buffering, 0 ended, -1 stopped */
    char remote[128];         /* connected phone */
    bool connected;
    YMsg out[32];
    int nout;
    bool rebind;
    /* cast devices approved on the TV (by IP) */
    char approved[CAST_APPROVED_MAX][64];
    int napproved;
    char ask_ip[64];          /* a new device waits for Allow/Deny on the TV */
    bool ask;
    double asked_t;           /* when the question was shown; 0 = none, expires after 60 s */
    /* devices denied on the TV (by IP), remembered for 10 minutes */
    struct { char ip[64]; double t; } denied[CAST_APPROVED_MAX];
    int ndenied;
} Y = {.volume = 100, .state = -1, .cur = -1};

static void push(int cmd, double arg) {
    SDL_LockMutex(mu);
    if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){cmd, arg};
    SDL_UnlockMutex(mu);
}

/* message to the phone (queued; sent by out_thread) - called with mu locked */
static YMsg *msg_new(const char *sc) {
    if (Y.nout >= 32) return NULL;
    YMsg *m = &Y.out[Y.nout++];
    memset(m, 0, sizeof *m);
    snprintf(m->sc, sizeof m->sc, "%s", sc);
    SDL_SignalCondition(out_cv);
    return m;
}
static void kv(YMsg *m, const char *k, const char *fmt, ...) {
    if (!m || m->n >= 10) return;
    snprintf(m->k[m->n], sizeof m->k[0], "%s", k);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->v[m->n], sizeof m->v[0], fmt, ap);
    va_end(ap);
    m->n++;
}
static void send_now_playing(void) { /* mu locked */
    YMsg *m = msg_new("nowPlaying");
    if (Y.cur < 0 || Y.cur >= Y.nids) return; /* empty = nothing is playing */
    kv(m, "videoId", "%s", Y.ids[Y.cur]);
    kv(m, "currentTime", "%.3f", Y.pos);
    kv(m, "duration", "%.3f", Y.dur);
    kv(m, "state", "%d", Y.state);
    kv(m, "currentIndex", "%d", Y.cur);
    kv(m, "listId", "%s", Y.list_id);
    kv(m, "ctt", "%s", Y.ctt);
    kv(m, "loadedTime", "%.3f", Y.pos);
}
static void send_state(void) { /* mu locked */
    YMsg *m = msg_new("onStateChange");
    kv(m, "currentTime", "%.3f", Y.pos);
    kv(m, "duration", "%.3f", Y.dur);
    kv(m, "state", "%d", Y.state);
    kv(m, "cpn", "notatv");
    kv(m, "loadedTime", "%.3f", Y.pos);
    kv(m, "seekableStartTime", "0");
    kv(m, "seekableEndTime", "%.3f", Y.dur);
}
static void send_volume(void) { /* mu locked */
    YMsg *m = msg_new("onVolumeChanged");
    kv(m, "volume", "%d", Y.volume);
    kv(m, "muted", "false");
}
static void send_prevnext(void) { /* mu locked */
    YMsg *m = msg_new("onHasPreviousNextChanged");
    kv(m, "hasPrevious", Y.cur > 0 ? "true" : "false");
    kv(m, "hasNext", Y.cur >= 0 && Y.cur + 1 < Y.nids ? "true" : "false");
}

/* ---------- HTTP helpers (lounge) ---------- */
typedef struct { char *d; size_t n; } Buf;
static size_t buf_write(char *p, size_t sz, size_t nm, void *ud) {
    Buf *b = ud;
    size_t n = sz * nm;
    b->d = xrealloc(b->d, b->n + n + 1);
    memcpy(b->d + b->n, p, n);
    b->n += n;
    b->d[b->n] = 0;
    return n;
}
static long post(const char *url, const char *body, Buf *out) {
    CURL *c = curl_easy_init();
    if (!c) return 0;
    out->d = NULL;
    out->n = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux) NotaTV/" TV_VERSION);
    long st = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
    curl_easy_cleanup(c);
    return st;
}
static char *enc(const char *s) { return url_encode(s ? s : ""); }

static bool ensure_screen(void) {
    if (screen_id[0]) return true;
    Buf b;
    long st = post(LOUNGE "/pairing/generate_screen_id", NULL, &b);
    bool ok = st == 200 && b.d && strlen(b.d) > 10 && strlen(b.d) < sizeof screen_id;
    if (ok) {
        SDL_LockMutex(mu);
        snprintf(screen_id, sizeof screen_id, "%s", b.d);
        str_trim(screen_id);
        SDL_UnlockMutex(mu);
    }
    free(b.d);
    return ok;
}

static bool refresh_token(void) {
    char *body = str_printf("screen_ids=%s", screen_id);
    Buf b;
    long st = post(LOUNGE "/pairing/get_lounge_token_batch", body, &b);
    free(body);
    bool ok = false;
    cJSON *j = st == 200 && b.d ? cJSON_Parse(b.d) : NULL;
    cJSON *s = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "screens"), 0);
    cJSON *t = cJSON_GetObjectItem(s, "loungeToken"), *e = cJSON_GetObjectItem(s, "expiration");
    if (cJSON_IsString(t)) {
        SDL_LockMutex(mu);
        snprintf(Y.token, sizeof Y.token, "%s", t->valuestring);
        Y.token_exp = cJSON_IsNumber(e) ? e->valuedouble / 1000 : (double)time(NULL) + 86400;
        SDL_UnlockMutex(mu);
        ok = true;
    }
    cJSON_Delete(j);
    free(b.d);
    return ok;
}

/* shared query for bind calls (mu locked) */
static char *bind_query(const char *extra) {
    char *n = enc(name), *tok = enc(Y.token);
    char *dev = enc("{\"brand\":\"NotaTV\",\"model\":\"NotaTV\",\"year\":0,\"os\":\"Linux\",\"osVersion\":\"\",\"chipset\":\"\","
                    "\"clientName\":\"TVHTML5\",\"dialAdditionalDataSupportLevel\":\"full\",\"mdxDialServerType\":\"MDX_DIAL_SERVER_TYPE_IN_APP\"}");
    char *q = str_printf("%s/bc/bind?device=LOUNGE_SCREEN&theme=cl&capabilities=dsp%%2Cmic%%2Cdpa&mdxVersion=2&loungeIdToken=%s"
                         "&name=%s&app=" APP_NAME "&id=%s&deviceInfo=%s&VER=8&v=2&zx=notatv%ld&t=1%s",
                         LOUNGE, tok, n, device_id, dev, (long)SDL_GetTicks(), extra);
    free(n); free(tok); free(dev);
    return q;
}

/* ---------- indkommende kommandoer ---------- */
static const char *jget(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void set_playlist(cJSON *a, bool replace_current) { /* mu locked */
    const char *ids = jget(a, "videoIds"), *vid = jget(a, "videoId");
    if (*ids) {
        Y.nids = 0;
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s", ids);
        char *sv_t;
        for (char *t = strtok_r(tmp, ",", &sv_t); t && Y.nids < 200; t = strtok_r(NULL, ",", &sv_t))
            snprintf(Y.ids[Y.nids++], sizeof Y.ids[0], "%s", t);
    }
    if (*jget(a, "listId")) snprintf(Y.list_id, sizeof Y.list_id, "%s", jget(a, "listId"));
    if (*jget(a, "ctt")) snprintf(Y.ctt, sizeof Y.ctt, "%s", jget(a, "ctt"));
    int idx = *jget(a, "currentIndex") ? atoi(jget(a, "currentIndex")) : -1;
    if (*vid && (idx < 0 || idx >= Y.nids || strcmp(Y.ids[idx], vid))) { /* find the video in the list, otherwise only that one */
        idx = -1;
        for (int i = 0; i < Y.nids; i++) if (!strcmp(Y.ids[i], vid)) { idx = i; break; }
        if (idx < 0) { Y.nids = 1; snprintf(Y.ids[0], sizeof Y.ids[0], "%s", vid); idx = 0; }
    }
    if (!replace_current) return;
    if (idx >= 0) {
        Y.cur = idx;
        Y.start = atof(jget(a, "currentTime"));
        Y.want_play = true;
        Y.state = 3;
        Y.pos = Y.start;
        Y.dur = 0;
        send_now_playing();
        send_state();
        send_prevnext();
        if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){Y_PLAY, 0};
    }
}

static void handle(const char *cmd, cJSON *a) {
    SDL_LockMutex(mu);
    if (!strcmp(cmd, "c")) {
    } else if (!strcmp(cmd, "remoteConnected")) {
        snprintf(Y.remote, sizeof Y.remote, "%s", *jget(a, "name") ? jget(a, "name") : T("A phone"));
        Y.connected = true;
        send_volume();
    } else if (!strcmp(cmd, "remoteDisconnected")) {
        Y.connected = false;
    } else if (!strcmp(cmd, "getNowPlaying")) {
        send_now_playing();
    } else if (!strcmp(cmd, "setPlaylist")) {
        set_playlist(a, true);
    } else if (!strcmp(cmd, "updatePlaylist")) {
        char cur[16] = "";
        if (Y.cur >= 0 && Y.cur < Y.nids) snprintf(cur, sizeof cur, "%s", Y.ids[Y.cur]);
        set_playlist(a, false);
        Y.cur = -1;
        for (int i = 0; i < Y.nids; i++) if (!strcmp(Y.ids[i], cur)) Y.cur = i;
        if (!*jget(a, "videoIds")) { Y.nids = 0; Y.cur = -1; }
        send_prevnext();
    } else if (!strcmp(cmd, "setVideo")) {
        set_playlist(a, true);
    } else if (!strcmp(cmd, "play")) {
        if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){Y_RESUME, 0};
    } else if (!strcmp(cmd, "pause")) {
        if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){Y_PAUSE, 0};
    } else if (!strcmp(cmd, "seekTo")) {
        if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){Y_SEEK, atof(jget(a, "newTime"))};
    } else if (!strcmp(cmd, "stopVideo")) {
        if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){Y_STOP, 0};
    } else if (!strcmp(cmd, "next")) {
        if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){Y_NEXT, 0};
    } else if (!strcmp(cmd, "previous")) {
        if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){Y_PREV, 0};
    } else if (!strcmp(cmd, "setVolume")) {
        if (Y.qn < 32) Y.q[Y.qn++] = (YCmd){Y_VOLUME, atof(jget(a, "volume"))};
    } else if (!strcmp(cmd, "getVolume")) {
        send_volume();
    }
    SDL_UnlockMutex(mu);
}

/* long polling: the reply streams as "<length>\n[[aid,[cmd,{...}]],...]" in chunks */
typedef struct { Buf b; size_t used; } Stream;

static void parse_chunks(Stream *s) {
    for (;;) {
        char *p = s->b.d + s->used, *end = s->b.d + s->b.n;
        while (p < end && (*p == '\n' || *p == '\r' || (*p >= '0' && *p <= '9'))) p++;
        if (p >= end || *p != '[') return;
        /* find the closing ']' (the length is in UTF-16 characters, so count brackets instead) */
        int depth = 0;
        bool str = false, esc = false;
        char *q = p;
        for (; q < end; q++) {
            if (str) { if (esc) esc = false; else if (*q == '\\') esc = true; else if (*q == '"') str = false; continue; }
            if (*q == '"') str = true;
            else if (*q == '[') depth++;
            else if (*q == ']' && --depth == 0) break;
        }
        if (q >= end) return; /* incomplete: wait for more */
        char save = q[1];
        q[1] = 0;
        cJSON *arr = cJSON_Parse(p);
        q[1] = save;
        s->used = (size_t)(q + 1 - s->b.d);
        cJSON *m;
        cJSON_ArrayForEach(m, arr) {
            cJSON *idx = cJSON_GetArrayItem(m, 0), *body = cJSON_GetArrayItem(m, 1);
            cJSON *cmd = cJSON_GetArrayItem(body, 0), *arg = cJSON_GetArrayItem(body, 1);
            if (cJSON_IsNumber(idx)) { SDL_LockMutex(mu); Y.aid = (long)idx->valuedouble; SDL_UnlockMutex(mu); }
            if (!cJSON_IsString(cmd)) continue;
            if (!strcmp(cmd->valuestring, "c") && cJSON_IsString(arg)) {
                SDL_LockMutex(mu); snprintf(Y.sid, sizeof Y.sid, "%s", arg->valuestring); SDL_UnlockMutex(mu);
            } else if (!strcmp(cmd->valuestring, "S") && cJSON_IsString(arg)) {
                SDL_LockMutex(mu); snprintf(Y.gsid, sizeof Y.gsid, "%s", arg->valuestring); SDL_UnlockMutex(mu);
            } else if (!strcmp(cmd->valuestring, "noop")) {
            } else {
                SDL_Log("ytcast: %s", cmd->valuestring);
                handle(cmd->valuestring, arg);
            }
        }
        cJSON_Delete(arr);
    }
}
static size_t stream_write(char *p, size_t sz, size_t nm, void *ud) {
    Stream *s = ud;
    size_t n = buf_write(p, sz, nm, &s->b);
    parse_chunks(s);
    return SDL_GetAtomicInt(&enabled) ? n : 0; /* turned off: abort */
}

static int lounge_thread(void *arg) {
    (void)arg;
    double backoff = 2;
    for (;;) {
        if (!SDL_GetAtomicInt(&enabled)) { SDL_Delay(2000); continue; }
        if (!ensure_screen() || (Y.token_exp - time(NULL) < 3600 && !refresh_token())) {
            SDL_Delay((Uint32)(backoff * 1000));
            backoff = backoff < 60 ? backoff * 2 : 60;
            continue;
        }
        /* new session */
        SDL_LockMutex(mu);
        Y.sid[0] = Y.gsid[0] = 0;
        Y.aid = 0;
        Y.ofs = 0;
        Y.rid = 1000 + (long)(SDL_rand_bits() % 8000);
        char first[64];
        snprintf(first, sizeof first, "&RID=%ld&AID=0&CVER=1", Y.rid); /* later sends must continue the numbering */
        char *url = bind_query(first);
        Y.rebind = false;
        SDL_UnlockMutex(mu);
        Stream s = {0};
        CURL *c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, "count=0");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        long st = 0;
        if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
        curl_easy_cleanup(c);
        free(url);
        free(s.b.d);
        if (st == 401 || st == 403) { SDL_LockMutex(mu); Y.token_exp = 0; SDL_UnlockMutex(mu); }
        if (st != 200 || !Y.sid[0]) {
            SDL_Log("ytcast: bind fejlede (%ld)", st);
            SDL_Delay((Uint32)(backoff * 1000));
            backoff = backoff < 60 ? backoff * 2 : 60;
            continue;
        }
        backoff = 2;
        SDL_Log("ytcast: forbundet til YouTube som \"%s\"", name);
        /* long polling until the session dies */
        for (;;) {
            SDL_LockMutex(mu);
            char *gs = enc(Y.gsid), *sid = enc(Y.sid);
            char extra[512];
            snprintf(extra, sizeof extra, "&RID=rpc&SID=%s&gsessionid=%s&CI=0&TYPE=xmlhttp&AID=%ld", sid, gs, Y.aid);
            free(gs); free(sid);
            url = bind_query(extra);
            bool stop = Y.rebind || !SDL_GetAtomicInt(&enabled);
            SDL_UnlockMutex(mu);
            if (stop) { free(url); break; }
            memset(&s, 0, sizeof s);
            c = curl_easy_init();
            curl_easy_setopt(c, CURLOPT_URL, url);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
            curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L); /* YouTube sends "noop" about every 30 s */
            curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 90L);
            curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
            st = 0;
            CURLcode rc = curl_easy_perform(c);
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
            curl_easy_cleanup(c);
            free(url);
            free(s.b.d);
            if (st == 400 || st == 404 || st == 410 || st == 401) { SDL_Log("ytcast: session udløbet (%ld)", st); break; }
            if (rc != CURLE_OK && rc != CURLE_OPERATION_TIMEDOUT && rc != CURLE_PARTIAL_FILE) SDL_Delay(2000);
            if (Y.token_exp - time(NULL) < 3600) break; /* new token -> new session */
        }
    }
    return 0;
}

/* sends messages to the phone: POST bind?RID=n with count/ofs/reqN_… */
static int out_thread(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(mu);
        while (Y.nout == 0 || !Y.sid[0]) SDL_WaitConditionTimeout(out_cv, mu, 1000);
        YMsg msgs[32];
        int n = Y.nout;
        memcpy(msgs, Y.out, sizeof(YMsg) * n);
        Y.nout = 0;
        char *gs = enc(Y.gsid), *sid = enc(Y.sid);
        char extra[512];
        snprintf(extra, sizeof extra, "&RID=%ld&SID=%s&gsessionid=%s&AID=%ld", ++Y.rid, sid, gs, Y.aid);
        free(gs); free(sid);
        char *url = bind_query(extra);
        long ofs = Y.ofs;
        Y.ofs += n;
        SDL_UnlockMutex(mu);
        /* exact size: a message can take ~8 KB (10 values of 255 characters, URL-encoded up to x3);
           too small a guess made the snprintf lengths run past the buffer */
        size_t cap = 64, len = 0;
        for (int i = 0; i < n; i++) {
            cap += 32 + strlen(msgs[i].sc);
            for (int k = 0; k < msgs[i].n; k++) cap += 32 + strlen(msgs[i].k[k]) + 3 * strlen(msgs[i].v[k]);
        }
        char *body = xmalloc(cap);
        len += (size_t)snprintf(body + len, cap - len, "count=%d&ofs=%ld", n, ofs);
        if (len > cap - 1) len = cap - 1;
        for (int i = 0; i < n; i++) {
            len += (size_t)snprintf(body + len, cap - len, "&req%d__sc=%s", i, msgs[i].sc);
            if (len > cap - 1) len = cap - 1;
            for (int k = 0; k < msgs[i].n; k++) {
                char *v = enc(msgs[i].v[k]);
                len += (size_t)snprintf(body + len, cap - len, "&req%d_%s=%s", i, msgs[i].k[k], v);
                if (len > cap - 1) len = cap - 1;
                free(v);
            }
        }
        Buf b;
        long st = post(url, body, &b);
        if (st != 200) {
            SDL_Log("ytcast: afsendelse fejlede (%ld): %.300s | %.200s", st, b.d ? b.d : "", body);
            if (st == 400 || st == 404 || st == 410) { SDL_LockMutex(mu); Y.rebind = true; Y.sid[0] = 0; SDL_UnlockMutex(mu); }
        }
        free(b.d);
        free(body);
        free(url);
    }
    return 0;
}

/* ---------- DIAL: SSDP + HTTP (port 8008) ---------- */
static void local_ip_for(const struct sockaddr *peer, socklen_t pl, char *out, size_t n) {
    snprintf(out, n, "0.0.0.0");
    int s = socket(peer->sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct sockaddr_storage me;
    socklen_t ml = sizeof me;
    if (connect(s, peer, pl) == 0 && getsockname(s, (struct sockaddr *)&me, &ml) == 0)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&me)->sin_addr, out, (socklen_t)n);
    close(s);
}

static int ssdp_thread(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(1900), .sin_addr.s_addr = INADDR_ANY};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        SDL_Log("ytcast: kan ikke binde SSDP-port 1900");
        if (s >= 0) close(s);
        return 0;
    }
    double last_join = -1000;
    for (;;) {
        if (app_time() - last_join > 30) {
            struct ip_mreq mr = {.imr_interface.s_addr = INADDR_ANY};
            inet_pton(AF_INET, "239.255.255.250", &mr.imr_multiaddr);
            setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
            last_join = app_time();
        }
        struct pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, 2000) <= 0) continue;
        char in[1500];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(s, in, sizeof in - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        in[r] = 0;
        if (!SDL_GetAtomicInt(&enabled) || strncmp(in, "M-SEARCH", 8) || (!strstr(in, "dial-multiscreen") && !strstr(in, "ssdp:all"))) continue;
        char ip[64];
        local_ip_for((struct sockaddr *)&from, fl, ip, sizeof ip);
        char msg[800];
        int n = snprintf(msg, sizeof msg,
                         "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: http://%s:%d/ssdp/device-desc.xml\r\n"
                         "SERVER: Linux/1.0 UPnP/1.1 NotaTV/" TV_VERSION "\r\nST: urn:dial-multiscreen-org:service:dial:1\r\n"
                         "USN: %s::urn:dial-multiscreen-org:service:dial:1\r\nBOOTID.UPNP.ORG: 1\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n",
                         ip, DIAL_PORT, udn);
        sendto(s, msg, (size_t)n, 0, (struct sockaddr *)&from, fl);
    }
    return 0;
}

static void send_all(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}
static void reply(int fd, const char *status, const char *ctype, const char *extra, const char *body) {
    char h[1024];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",
                     status, ctype, body ? strlen(body) : 0, extra ? extra : "");
    send_all(fd, h, (size_t)n);
    if (body) send_all(fd, body, strlen(body));
}

static void register_pairing(const char *code) {
    SDL_LockMutex(mu);
    char *n = enc(name), *sc = enc(screen_id), *c = enc(code);
    char *body = str_printf("access_type=permanent&app=" APP_NAME "&pairing_code=%s&screen_id=%s&screen_name=%s&device_id=%s", c, sc, n, device_id);
    SDL_UnlockMutex(mu);
    free(n); free(sc); free(c);
    Buf b;
    long st = post(LOUNGE "/pairing/register_pairing_code", body, &b);
    SDL_Log("ytcast: DIAL-parring %s (%ld)", st == 200 ? "ok" : "fejlede", st);
    free(b.d);
    free(body);
}

typedef struct { int fd; } DConn;
#define DIAL_MAX_CONN 8 /* connections served at a time (one thread each) */
static SDL_AtomicInt dial_n;

/* a device is allowed if it has been approved on the TV (localhost always is) */
static bool cast_approved(const char *ip) {
    if (!strcmp(ip, "127.0.0.1") || !strcmp(ip, "::1")) return true;
    bool ok = false;
    SDL_LockMutex(mu);
    for (int i = 0; i < Y.napproved; i++)
        if (!strcmp(Y.approved[i], ip)) { ok = true; break; }
    SDL_UnlockMutex(mu);
    return ok;
}
/* mu locked: drop denials older than 10 minutes and say whether ip was denied recently */
static bool cast_denied(const char *ip) {
    const double forget_after = 600; /* 10 minutes */
    double now = app_time();
    bool no = false;
    for (int i = 0; i < Y.ndenied; ) {
        if (now - Y.denied[i].t >= forget_after) { Y.denied[i] = Y.denied[--Y.ndenied]; continue; }
        if (!strcmp(Y.denied[i].ip, ip)) no = true;
        i++;
    }
    return no;
}
/* mu locked: remember a denial for 10 minutes */
static void cast_deny(const char *ip) {
    if (!ip[0]) return;
    for (int i = 0; i < Y.ndenied; i++)
        if (!strcmp(Y.denied[i].ip, ip)) { Y.denied[i].t = app_time(); return; }
    if (Y.ndenied < CAST_APPROVED_MAX) {
        int i = Y.ndenied++;
        snprintf(Y.denied[i].ip, sizeof Y.denied[i].ip, "%s", ip);
        Y.denied[i].t = app_time();
    } else { /* full: forget the oldest */
        int old = 0;
        for (int i = 1; i < Y.ndenied; i++)
            if (Y.denied[i].t < Y.denied[old].t) old = i;
        snprintf(Y.denied[old].ip, sizeof Y.denied[old].ip, "%s", ip);
        Y.denied[old].t = app_time();
    }
}
/* a new device: let the main thread ask on the TV (once at a time) */
static void cast_ask(const char *ip) {
    if (!ip[0]) return;
    SDL_LockMutex(mu);
    bool pending = Y.asked_t > 0 && app_time() - Y.asked_t < 60; /* Home can close the question without an answer */
    if (!Y.ask && !pending && !cast_denied(ip)) {
        snprintf(Y.ask_ip, sizeof Y.ask_ip, "%s", ip);
        Y.ask = true;
    }
    SDL_UnlockMutex(mu);
}

static int dial_conn(void *arg) {
    DConn *dc = arg;
    int fd = dc->fd;
    free(dc);
    char buf[8192];
    size_t n = 0;
    struct pollfd p = {fd, POLLIN, 0};
    Uint64 deadline = SDL_GetTicks() + 6000; /* the whole request within 6 s */
    while (n < sizeof buf - 1 && SDL_GetTicks() < deadline && poll(&p, 1, 3000) > 0) {
        ssize_t r = recv(fd, buf + n, sizeof buf - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
        char *he = strstr(buf, "\r\n\r\n");
        if (he) { /* wait for the whole body */
            char *cl = strcasestr(buf, "Content-Length:");
            size_t want = cl && cl < he ? (size_t)atol(cl + 15) : 0;
            if (n >= (size_t)(he + 4 - buf) + want) break;
        }
    }
    buf[n] = 0;
    char method[16] = "", path[512] = "";
    sscanf(buf, "%15s %511s", method, path);
    struct sockaddr_storage me;
    socklen_t ml = sizeof me;
    char ip[64] = "0.0.0.0";
    if (getsockname(fd, (struct sockaddr *)&me, &ml) == 0) {
        if (me.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&me)->sin6_addr, ip, sizeof ip);
            if (!strncmp(ip, "::ffff:", 7)) memmove(ip, ip + 7, strlen(ip + 7) + 1);
        } else inet_ntop(AF_INET, &((struct sockaddr_in *)&me)->sin_addr, ip, sizeof ip);
    }
    struct sockaddr_storage peer;
    socklen_t pl = sizeof peer;
    char cip[64] = "";
    if (getpeername(fd, (struct sockaddr *)&peer, &pl) == 0) {
        if (peer.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, cip, sizeof cip);
            if (!strncmp(cip, "::ffff:", 7)) memmove(cip, cip + 7, strlen(cip + 7) + 1);
        } else inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, cip, sizeof cip);
    }
    if (!SDL_GetAtomicInt(&enabled)) { reply(fd, "404 Not Found", "text/plain", NULL, ""); goto done; }
    /* CSRF: a random web site must not start/control. The DIAL spec: apps send "Origin: package:<app>"
     * (YouTube on Android) - those and YouTube's own pages are allowed, all other web origins are rejected */
    char *org = strcasestr(buf, "\r\nOrigin:");
    if (org && (!strstr(buf, "\r\n\r\n") || org < strstr(buf, "\r\n\r\n"))) {
        char o[200] = "";
        sscanf(org + 9, " %199[^\r\n]", o);
        size_t ol = strlen(o);
        bool ok = !strncmp(o, "package:", 8) || !strcmp(o, "https://www.youtube.com") || !strcmp(o, "https://m.youtube.com") ||
                  (ol > 12 && !strcmp(o + ol - 12, ".youtube.com") && !strncmp(o, "https://", 8));
        if (!ok) {
            SDL_Log("ytcast: DIAL afviste Origin '%s'", o);
            reply(fd, "403 Forbidden", "text/plain", NULL, "");
            goto done;
        }
    }
    if (!strcmp(path, "/ssdp/device-desc.xml")) {
        char ex[200], nm[256];
        snprintf(ex, sizeof ex, "Application-URL: http://%s:%d/apps/\r\nAccess-Control-Expose-Headers: Application-URL\r\n", ip, DIAL_PORT);
        SDL_LockMutex(mu);
        snprintf(nm, sizeof nm, "%s", name);
        SDL_UnlockMutex(mu);
        for (char *q = nm; *q; q++) if (*q == '<' || *q == '>' || *q == '&') *q = ' ';
        char *b = str_printf("<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
                             "<URLBase>http://%s:%d</URLBase><device><deviceType>urn:dial-multiscreen-org:device:dial:1</deviceType>"
                             "<friendlyName>%s</friendlyName><manufacturer>NotaTV</manufacturer><modelName>NotaTV</modelName><UDN>%s</UDN>"
                             "<serviceList><service><serviceType>urn:dial-multiscreen-org:service:dial:1</serviceType>"
                             "<serviceId>urn:dial-multiscreen-org:serviceId:dial</serviceId><controlURL>/ssdp/notfound</controlURL>"
                             "<eventSubURL>/ssdp/notfound</eventSubURL><SCPDURL>/ssdp/notfound</SCPDURL></service></serviceList></device></root>",
                             ip, DIAL_PORT, nm, udn);
        reply(fd, "200 OK", "application/xml", ex, b);
        free(b);
    } else if (!strncmp(path, "/apps/YouTube", 13)) {
        bool ok = cast_approved(cip);
        if (!strcmp(method, "GET")) {
            SDL_LockMutex(mu);
            bool running = ok && (Y.connected || Y.playing_now);
            char add[1024] = "";
            if (ok) snprintf(add, sizeof add, "<additionalData><screenId>%s</screenId><loungeToken>%s</loungeToken>"
                                              "<loungeTokenRefreshInterval>1500000</loungeTokenRefreshInterval></additionalData>", screen_id, Y.token);
            char *b = str_printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?><service xmlns=\"urn:dial-multiscreen-org:schemas:dial\" dialVer=\"2.1\">"
                                 "<name>YouTube</name><options allowStop=\"true\"/><state>%s</state>%s%s</service>",
                                 running ? "running" : "stopped", running ? "<link rel=\"run\" href=\"run\"/>" : "", add);
            SDL_UnlockMutex(mu);
            reply(fd, "200 OK", "application/xml", NULL, b);
            free(b);
        } else if (!strcmp(method, "POST")) {
            if (!ok) { cast_ask(cip); reply(fd, "403 Forbidden", "text/plain", NULL, ""); goto done; }
            char *body = strstr(buf, "\r\n\r\n"), code[64] = "";
            if (body) {
                char *pc = strstr(body + 4, "pairingCode=");
                if (pc) sscanf(pc + 12, "%63[^&\r\n]", code);
            }
            if (code[0]) register_pairing(code);
            char ex[160];
            snprintf(ex, sizeof ex, "Location: http://%s:%d/apps/YouTube/run\r\n", ip, DIAL_PORT);
            reply(fd, "201 Created", "text/plain", ex, "");
        } else if (!strcmp(method, "DELETE")) {
            if (!ok) { reply(fd, "403 Forbidden", "text/plain", NULL, ""); goto done; }
            push(Y_STOP, 0);
            reply(fd, "200 OK", "text/plain", NULL, "");
        } else reply(fd, "405 Method Not Allowed", "text/plain", NULL, "");
    } else reply(fd, "404 Not Found", "text/plain", NULL, "");
done:
    close(fd);
    SDL_AddAtomicInt(&dial_n, -1);
    return 0;
}

static int dial_thread(void *arg) {
    (void)arg;
    int s = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0), off = 0, one = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(DIAL_PORT), .sin6_addr = in6addr_any};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) {
        SDL_Log("ytcast: kan ikke lytte på port %d", DIAL_PORT);
        if (s >= 0) close(s);
        return 0;
    }
    for (;;) {
        int fd = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) { SDL_Delay(100); continue; }
        DConn *dc = SDL_GetAtomicInt(&dial_n) < DIAL_MAX_CONN ? xmalloc(sizeof *dc) : NULL;
        if (!dc) { close(fd); continue; } /* busy: the phone tries again */
        dc->fd = fd;
        SDL_AddAtomicInt(&dial_n, 1);
        SDL_Thread *t = SDL_CreateThread(dial_conn, "ytcast-dial", dc);
        if (t) SDL_DetachThread(t);
        else { close(fd); free(dc); SDL_AddAtomicInt(&dial_n, -1); }
    }
    return 0;
}

/* ---------- "Link with TV code" ---------- */
char *ytcast_pairing_code(void) { /* blocking (background job); malloc'ed "123 456 789 012" or NULL */
    if (!started || !ensure_screen() || (!Y.token[0] && !refresh_token())) return NULL;
    SDL_LockMutex(mu);
    char *n = enc(name), *sc = enc(screen_id), *tok = enc(Y.token);
    char *body = str_printf("access_type=permanent&app=" APP_NAME "&lounge_token=%s&screen_id=%s&screen_name=%s&device_id=%s", tok, sc, n, device_id);
    SDL_UnlockMutex(mu);
    free(n); free(sc); free(tok);
    Buf b;
    long st = post(LOUNGE "/pairing/get_pairing_code?ctx=pair", body, &b);
    free(body);
    char *r = NULL;
    if (st == 200 && b.d) {
        char d[16] = "";
        int k = 0;
        for (char *p = b.d; *p && k < 12; p++) if (*p >= '0' && *p <= '9') d[k++] = *p;
        if (k == 12) r = str_printf("%.3s %.3s %.3s %.3s", d, d + 3, d + 6, d + 9);
    }
    free(b.d);
    return r;
}

/* ---------- main thread ---------- */
void ytcast_apply(void) { SDL_SetAtomicInt(&enabled, cfg_int("ytcast_enabled", 1) && app_on("youtube")); }

/* sign out: no cast device is approved any more */
void ytcast_forget(void) {
    if (!mu) return;
    SDL_LockMutex(mu);
    Y.napproved = 0;
    Y.ndenied = 0;
    Y.ask = false;
    Y.asked_t = 0;
    SDL_UnlockMutex(mu);
}

static void new_id(char *out, size_t n) {
    snprintf(out, n, "%08x-%04x-4%03x-a%03x-%08x%04x", (unsigned)SDL_rand_bits(), (unsigned)SDL_rand_bits() & 0xffff,
             (unsigned)SDL_rand_bits() & 0xfff, (unsigned)SDL_rand_bits() & 0xfff, (unsigned)SDL_rand_bits(), (unsigned)SDL_rand_bits() & 0xffff);
}

static void play_current(void);

/* the viewer allowed or denied a new cast device */
static void cast_allow_cb(void *ud, bool yes) {
    (void)ud;
    SDL_LockMutex(mu);
    Y.asked_t = 0; /* answered: a new POST may ask again */
    if (Y.ask_ip[0]) {
        if (yes) {
            if (Y.napproved < CAST_APPROVED_MAX)
                snprintf(Y.approved[Y.napproved++], sizeof Y.approved[0], "%s", Y.ask_ip);
        } else cast_deny(Y.ask_ip);
    }
    SDL_UnlockMutex(mu);
}

void ytcast_tick(void) {
    if (!started) {
        started = true;
        mu = SDL_CreateMutex();
        out_cv = SDL_CreateCondition();
        if (!*cfg_get("yt_device_id", "")) { char id[48]; new_id(id, sizeof id); cfg_set("yt_device_id", id); }
        snprintf(device_id, sizeof device_id, "%s", cfg_get("yt_device_id", ""));
        snprintf(udn, sizeof udn, "uuid:%s", device_id);
        snprintf(screen_id, sizeof screen_id, "%s", cfg_get("yt_screen_id", ""));
        snprintf(name, sizeof name, "%s", cfg_get("device_name", "NotaTV"));
        ytcast_apply();
        const char *nm[] = {"ytcast-lounge", "ytcast-out", "ytcast-ssdp", "ytcast-dial"};
        SDL_ThreadFunction fn[] = {lounge_thread, out_thread, ssdp_thread, dial_thread};
        for (int i = 0; i < 4; i++) {
            SDL_Thread *t = SDL_CreateThread(fn[i], nm[i], NULL);
            if (t) SDL_DetachThread(t);
        }
        return;
    }
    SDL_LockMutex(mu);
    bool ask = Y.ask;
    char ask_ip[64];
    if (ask) {
        snprintf(ask_ip, sizeof ask_ip, "%s", Y.ask_ip);
        Y.ask = false;
        Y.asked_t = app_time(); /* pending until answered or 60 s pass */
    }
    if (screen_id[0] && strcmp(screen_id, cfg_get("yt_screen_id", ""))) cfg_set("yt_screen_id", screen_id); /* fixed screen id: phones remember the box */
    const char *nm = cfg_get("device_name", "NotaTV");
    if (strcmp(nm, name)) { snprintf(name, sizeof name, "%s", nm); Y.rebind = true; }
    static bool was_connected;
    char remote[128];
    bool conn = Y.connected, announce = conn && !was_connected;
    was_connected = conn;
    snprintf(remote, sizeof remote, "%s", Y.remote);
    bool start = Y.want_play;
    Y.want_play = false;
    int n = Y.qn;
    YCmd q[32];
    memcpy(q, Y.q, sizeof(YCmd) * n);
    Y.qn = 0;
    SDL_UnlockMutex(mu);
    if (ask) {
        char msg[160];
        snprintf(msg, sizeof msg, T("Allow %s to cast YouTube?"), ask_ip);
        confirm_open(T("YouTube cast"), msg, T("Allow"), T("Deny"), cast_allow_cb, NULL);
    }
    if (announce) app_toast(T("YouTube: %s is connected"), remote);
    (void)q;
    (void)n; /* outside playback: only new content does anything */
    if (start) play_current();
}

/* from the YouTube playback's rq.tick */
void ytcast_play_tick(void) {
    if (!started) return;
    SDL_LockMutex(mu);
    int n = Y.qn;
    YCmd q[32];
    memcpy(q, Y.q, sizeof(YCmd) * n);
    Y.qn = 0;
    SDL_UnlockMutex(mu);
    for (int i = 0; i < n; i++) {
        char a[32];
        switch (q[i].cmd) {
        case Y_PLAY: player_cmd((const char *[]){"quit", NULL}); break; /* new content: ytcast_tick starts it */
        case Y_PAUSE: player_cmd((const char *[]){"set", "pause", "yes", NULL}); break;
        case Y_RESUME: player_cmd((const char *[]){"set", "pause", "no", NULL}); break;
        case Y_STOP:
            SDL_LockMutex(mu);
            Y.cur = -1;
            SDL_UnlockMutex(mu);
            player_cmd((const char *[]){"quit", NULL});
            break;
        case Y_SEEK:
            snprintf(a, sizeof a, "%.1f", q[i].arg);
            player_cmd((const char *[]){"seek", a, "absolute", NULL});
            break;
        case Y_NEXT: case Y_PREV:
            SDL_LockMutex(mu);
            if (Y.cur >= 0) {
                int nc = Y.cur + (q[i].cmd == Y_NEXT ? 1 : -1);
                if (nc >= 0 && nc < Y.nids) { Y.cur = nc; Y.start = 0; Y.want_play = true; }
            }
            SDL_UnlockMutex(mu);
            player_cmd((const char *[]){"quit", NULL});
            break;
        case Y_VOLUME:
            snprintf(a, sizeof a, "%d", (int)q[i].arg);
            player_cmd((const char *[]){"set", "volume", a, NULL});
            SDL_LockMutex(mu);
            Y.volume = (int)q[i].arg;
            send_volume();
            SDL_UnlockMutex(mu);
            break;
        }
    }
    double pos, dur;
    bool paused;
    if (player_state(&pos, &dur, &paused)) {
        SDL_LockMutex(mu);
        int st = paused ? 2 : (pos > 0 ? 1 : 3);
        static double last_sent;
        bool ch = st != Y.state || fabs(pos - Y.pos) > 2.5 || (st == 1 && app_time() - last_sent > 5);
        Y.pos = pos;
        Y.dur = dur;
        Y.state = st;
        if (ch) { send_state(); last_sent = app_time(); }
        SDL_UnlockMutex(mu);
    }
}

static void play_current(void) {
    SDL_LockMutex(mu);
    if (Y.cur < 0 || Y.cur >= Y.nids) { SDL_UnlockMutex(mu); return; }
    char id[16];
    snprintf(id, sizeof id, "%s", Y.ids[Y.cur]);
    double start = Y.start;
    Y.playing_now = true;
    Y.state = 3;
    send_now_playing();
    send_prevnext();
    SDL_UnlockMutex(mu);
    bool ended = youtube_cast_play(id, start);
    SDL_LockMutex(mu);
    Y.playing_now = false;
    bool next = false;
    if (Y.want_play) { /* the phone chose something new / next / previous: ytcast_tick starts it */
    } else if (ended && Y.cur >= 0 && Y.cur + 1 < Y.nids) {
        Y.cur++;
        Y.start = 0;
        next = true;
    } else {
        Y.state = ended ? 0 : -1;
        send_state();
        if (!ended) Y.cur = -1;
        send_now_playing();
    }
    SDL_UnlockMutex(mu);
    if (next) play_current();
}
