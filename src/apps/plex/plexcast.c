/* NotaTV app package "plex" - Plex cast: the box as a Plex player, so "Cast"/"Play on" in Plex apps can start and control
 * playback. GDM discovery (UDP 32412, multicast 239.0.0.250) + Plex Companion over HTTP (port 3005):
 * playMedia/pause/seek/stop, timeline for the app (subscription and long poll) and navigation (arrow keys).
 * Local network only; nothing is sent to plex.tv. The playback itself happens in ui_plex.c (plex_cast_play). */
#define _GNU_SOURCE /* accept4 */
#include "plex.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define HTTP_PORT 3005
#define GDM_PORT 32412
#define GDM_GROUP "239.0.0.250"
#define CAPS "timeline,playback,navigation,playqueues"
#define CONTROLLABLE "playPause,stop,stepBack,stepForward,seekTo,skipNext,skipPrevious,volume"

enum { P_NONE, P_PLAY, P_PAUSE, P_RESUME, P_STOP, P_SEEK, P_SEEKREL, P_NEXT, P_PREV, P_VOLUME, P_NAV };
typedef struct { int cmd; double arg; } PCmd;
typedef struct { char ip[64], id[128]; int port, cmd_id; double seen; } Sub;

static SDL_Mutex *mu;
static SDL_Condition *changed;
static SDL_AtomicInt enabled;
static bool started;
static char my_id[96], my_name[128];
static struct {
    PCmd q[32];
    int qn;
    PlexCastMedia play; /* pending playMedia */
    bool have_play;
    int skip;           /* +1/-1: next/previous in the playQueue (ui_plex.c fetches it with plexcast_take_skip) */
    /* timeline */
    bool active, paused;
    double pos, dur, base; /* seconds; base = the transcode's start point */
    PlexCastNow now;
    int volume;
    unsigned seq;          /* counted up on every change (long poll) */
    Sub subs[8];
    int nsubs;
} PC = {.volume = 100};

static void push(int cmd, double arg) {
    SDL_LockMutex(mu);
    if (PC.qn < 32) PC.q[PC.qn++] = (PCmd){cmd, arg};
    SDL_UnlockMutex(mu);
}
static void bump(void) { PC.seq++; SDL_BroadcastCondition(changed); } /* called with mu locked */

/* ---------- timeline-XML ---------- */
static void xml_attr(char *out, size_t n, const char *s) {
    size_t o = 0;
    for (; s && *s && o + 7 < n; s++) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        if (e) { memcpy(out + o, e, strlen(e)); o += strlen(e); }
        else out[o++] = *s;
    }
    out[o] = 0;
}

static char *timeline_xml(int command_id) { /* called with mu locked */
    char video[2048];
    if (PC.active) {
        PlexCastNow *n = &PC.now;
        char key[300], ck[600];
        xml_attr(key, sizeof key, n->key);
        xml_attr(ck, sizeof ck, n->container);
        snprintf(video, sizeof video,
                 "<Timeline type=\"video\" itemType=\"video\" state=\"%s\" time=\"%d\" duration=\"%d\" seekRange=\"0-%d\" "
                 "key=\"%s\" ratingKey=\"%s\" containerKey=\"%s\" playQueueID=\"%s\" playQueueItemID=\"%s\" playQueueVersion=\"1\" "
                 "machineIdentifier=\"%s\" address=\"%s\" port=\"%d\" protocol=\"%s\" providerIdentifier=\"com.plexapp.plugins.library\" "
                 "volume=\"%d\" mediaIndex=\"0\" partIndex=\"0\" partCount=\"1\" controllable=\"" CONTROLLABLE "\"/>",
                 PC.paused ? "paused" : "playing", (int)((PC.pos + PC.base) * 1000), (int)(PC.dur * 1000), (int)(PC.dur * 1000),
                 key, n->rating_key, ck, n->pq_id, n->pq_item, n->server_id, n->address, n->port, n->protocol, PC.volume);
    } else snprintf(video, sizeof video, "<Timeline type=\"video\" state=\"stopped\"/>");
    return str_printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?><MediaContainer commandID=\"%d\" location=\"%s\">"
                      "<Timeline type=\"music\" state=\"stopped\"/><Timeline type=\"photo\" state=\"stopped\"/>%s</MediaContainer>",
                      command_id, PC.active ? "fullScreenVideo" : "navigation", video);
}

/* ---------- HTTP (Companion) ---------- */
static void qparam(const char *q, const char *name, char *out, size_t n) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = q; p && *p;) {
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            p += nl + 1;
            size_t o = 0;
            for (; *p && *p != '&' && o + 1 < n; p++) {
                if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    char h[3] = {p[1], p[2], 0};
                    out[o++] = (char)strtol(h, NULL, 16);
                    p += 2;
                } else out[o++] = *p == '+' ? ' ' : *p;
            }
            out[o] = 0;
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}
static int qint(const char *q, const char *name, int def) {
    char v[32];
    qparam(q, name, v, sizeof v);
    return v[0] ? atoi(v) : def;
}
static void header(const char *hdrs, const char *name, char *out, size_t n) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = hdrs; p && *p; p = strstr(p, "\r\n") ? strstr(p, "\r\n") + 2 : NULL) {
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ') p++;
            size_t o = 0;
            while (*p && *p != '\r' && o + 1 < n) out[o++] = *p++;
            out[o] = 0;
            return;
        }
    }
}

static void send_all(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}
/* only Plex's own web apps (*.plex.tv over https) may control the box from a browser; other web sites
 * are rejected (CSRF). Apps without an Origin header (the phone's Plex app) are unaffected. */
static __thread char cur_origin[160];
static bool origin_ok(const char *o) {
    if (!o[0]) return true;
    if (strncmp(o, "https://", 8)) return false;
    const char *h = o + 8;
    size_t n = strlen(h);
    return !strcmp(h, "plex.tv") || (n > 8 && !strcmp(h + n - 8, ".plex.tv"));
}

static void reply(int fd, int code, const char *body, const char *extra_hdrs) {
    char h[1280], cors[256] = "";
    if (cur_origin[0] && origin_ok(cur_origin))
        snprintf(cors, sizeof cors, "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n"
                                    "Access-Control-Expose-Headers: X-Plex-Client-Identifier\r\n", cur_origin);
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\nContent-Type: text/xml; charset=utf-8\r\nContent-Length: %zu\r\n"
                     "%sX-Plex-Client-Identifier: %s\r\nConnection: close\r\n%s\r\n",
                     code, code == 200 ? "OK" : code == 403 ? "Forbidden" : "Not Found", body ? strlen(body) : 0, cors, my_id,
                     extra_hdrs ? extra_hdrs : "");
    send_all(fd, h, (size_t)n);
    if (body) send_all(fd, body, strlen(body));
}
#define OK_BODY "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Response code=\"200\" status=\"OK\"/>"

static void subscribe(const char *ip, const char *q, const char *hdrs, bool on) {
    char id[128];
    header(hdrs, "X-Plex-Client-Identifier", id, sizeof id);
    if (!id[0]) snprintf(id, sizeof id, "%s", ip);
    SDL_LockMutex(mu);
    int k;
    for (k = 0; k < PC.nsubs; k++) if (!strcmp(PC.subs[k].id, id)) break;
    if (!on) {
        if (k < PC.nsubs) PC.subs[k] = PC.subs[--PC.nsubs];
    } else {
        if (k == PC.nsubs) k = PC.nsubs < 8 ? PC.nsubs++ : 7;
        Sub *s = &PC.subs[k];
        snprintf(s->id, sizeof s->id, "%s", id);
        snprintf(s->ip, sizeof s->ip, "%s", ip);
        s->port = qint(q, "port", 32500);
        s->cmd_id = qint(q, "commandID", 0);
        s->seen = app_time();
        bump(); /* a new subscriber gets the state right away */
    }
    SDL_UnlockMutex(mu);
}

static const struct { const char *path; Action a; } NAV[] = {
    {"moveUp", A_UP}, {"moveDown", A_DOWN}, {"moveLeft", A_LEFT}, {"moveRight", A_RIGHT}, {"select", A_OK},
    {"back", A_BACK}, {"home", A_HOME}, {"contextMenu", A_MENU}, {"toggleOSD", A_INFO}, {"pageUp", A_CHUP}, {"pageDown", A_CHDOWN},
};

typedef struct { int fd; char ip[64]; } Conn;

static int conn_thread(void *arg) {
    Conn *c = arg;
    char buf[8192];
    size_t n = 0;
    struct pollfd p = {c->fd, POLLIN, 0};
    while (n < sizeof buf - 1 && poll(&p, 1, 5000) > 0) {
        ssize_t r = recv(c->fd, buf + n, sizeof buf - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[n] = 0;
    char method[16] = "", target[4096] = "";
    if (!SDL_GetAtomicInt(&enabled) || sscanf(buf, "%15s %4095s", method, target) != 2) goto done;
    const char *hdrs = strstr(buf, "\r\n");
    hdrs = hdrs ? hdrs + 2 : "";
    char *q = strchr(target, '?');
    if (q) *q++ = 0;
    else q = "";
    const char *path = target;
    header(hdrs, "Origin", cur_origin, sizeof cur_origin);
    if (!origin_ok(cur_origin)) { reply(c->fd, 403, "", NULL); goto done; }
    if (!strcmp(method, "OPTIONS")) { /* CORS from Plex Web */
        char want[512], ex[800];
        header(hdrs, "Access-Control-Request-Headers", want, sizeof want);
        snprintf(ex, sizeof ex, "Access-Control-Allow-Methods: GET, POST, OPTIONS, DELETE, PUT, HEAD\r\n"
                                "Access-Control-Allow-Headers: %s\r\nAccess-Control-Max-Age: 1209600\r\n",
                 want[0] ? want : "x-plex-client-identifier, x-plex-token, x-plex-device-name, x-plex-target-client-identifier");
        reply(c->fd, 200, "", ex);
        goto done;
    }
    if (!strcmp(path, "/resources")) {
        char name[256];
        xml_attr(name, sizeof name, my_name);
        char *b = str_printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?><MediaContainer><Player title=\"%s\" protocol=\"plex\" protocolVersion=\"1\" "
                             "protocolCapabilities=\"" CAPS "\" machineIdentifier=\"%s\" product=\"NotaTV\" platform=\"Linux\" "
                             "platformVersion=\"" TV_VERSION "\" deviceClass=\"stb\"/></MediaContainer>", name, my_id);
        reply(c->fd, 200, b, NULL);
        free(b);
        goto done;
    }
    if (!strcmp(path, "/player/timeline/subscribe") || !strcmp(path, "/player/timeline/unsubscribe")) {
        subscribe(c->ip, q, hdrs, !strcmp(path, "/player/timeline/subscribe"));
        reply(c->fd, 200, OK_BODY, NULL);
        goto done;
    }
    if (!strcmp(path, "/player/timeline/poll")) { /* long poll: reply when something changes (max 20 s) */
        int cmd_id = qint(q, "commandID", 0);
        SDL_LockMutex(mu);
        if (qint(q, "wait", 0)) {
            unsigned seq = PC.seq;
            Uint64 until = SDL_GetTicks() + 20000;
            while (PC.seq == seq && SDL_GetTicks() < until) SDL_WaitConditionTimeout(changed, mu, 1000);
        }
        char *b = timeline_xml(cmd_id);
        SDL_UnlockMutex(mu);
        reply(c->fd, 200, b, NULL);
        free(b);
        goto done;
    }
    if (!strncmp(path, "/player/playback/", 17)) {
        const char *a = path + 17;
        if (!strcmp(a, "playMedia")) {
            PlexCastMedia m = {0};
            qparam(q, "key", m.key, sizeof m.key);
            qparam(q, "machineIdentifier", m.server_id, sizeof m.server_id);
            qparam(q, "address", m.address, sizeof m.address);
            qparam(q, "protocol", m.protocol, sizeof m.protocol);
            qparam(q, "containerKey", m.container, sizeof m.container);
            qparam(q, "token", m.token, sizeof m.token);
            if (!m.token[0]) header(hdrs, "X-Plex-Token", m.token, sizeof m.token);
            m.port = qint(q, "port", 32400);
            m.offset = qint(q, "offset", 0) / 1000.0;
            if (!m.protocol[0]) snprintf(m.protocol, sizeof m.protocol, "http");
            SDL_Log("plexcast: playMedia %s fra %s (server %s)", m.key, c->ip, m.server_id);
            if (m.key[0]) {
                SDL_LockMutex(mu);
                PC.play = m;
                PC.have_play = true;
                if (PC.qn < 32) PC.q[PC.qn++] = (PCmd){P_PLAY, 0};
                SDL_UnlockMutex(mu);
            }
        } else if (!strcmp(a, "pause")) push(P_PAUSE, 0);
        else if (!strcmp(a, "play")) push(P_RESUME, 0);
        else if (!strcmp(a, "stop")) push(P_STOP, 0);
        else if (!strcmp(a, "seekTo")) push(P_SEEK, qint(q, "offset", 0) / 1000.0);
        else if (!strcmp(a, "stepForward")) push(P_SEEKREL, 30);
        else if (!strcmp(a, "stepBack")) push(P_SEEKREL, -15);
        else if (!strcmp(a, "skipNext")) push(P_NEXT, 0);
        else if (!strcmp(a, "skipPrevious")) push(P_PREV, 0);
        else if (!strcmp(a, "setParameters") && qint(q, "volume", -1) >= 0) push(P_VOLUME, qint(q, "volume", 100));
        /* setStreams, refreshPlayQueue etc.: accepted without action */
        reply(c->fd, 200, OK_BODY, NULL);
        goto done;
    }
    if (!strncmp(path, "/player/navigation/", 19)) {
        for (size_t i = 0; i < sizeof NAV / sizeof NAV[0]; i++)
            if (!strcmp(path + 19, NAV[i].path)) push(P_NAV, NAV[i].a);
        reply(c->fd, 200, OK_BODY, NULL);
        goto done;
    }
    if (!strncmp(path, "/player/", 8)) { reply(c->fd, 200, OK_BODY, NULL); goto done; } /* mirror/application: ignored */
    reply(c->fd, 404, "", NULL);
done:
    close(c->fd);
    free(c);
    return 0;
}

static int http_thread(void *arg) {
    (void)arg;
    int s = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0), off = 0, one = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(HTTP_PORT), .sin6_addr = in6addr_any};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 16) < 0) {
        SDL_Log("plexcast: kan ikke lytte på port %d", HTTP_PORT);
        if (s >= 0) close(s);
        return 0;
    }
    for (;;) {
        struct sockaddr_in6 from;
        socklen_t fl = sizeof from;
        int fd = accept4(s, (struct sockaddr *)&from, &fl, SOCK_CLOEXEC);
        if (fd < 0) { SDL_Delay(100); continue; }
        Conn *c = calloc(1, sizeof *c);
        c->fd = fd;
        inet_ntop(AF_INET6, &from.sin6_addr, c->ip, sizeof c->ip);
        if (!strncmp(c->ip, "::ffff:", 7)) memmove(c->ip, c->ip + 7, strlen(c->ip + 7) + 1);
        SDL_Thread *t = SDL_CreateThread(conn_thread, "plexcast-conn", c);
        if (t) SDL_DetachThread(t);
        else { close(fd); free(c); }
    }
    return 0;
}

/* ---------- GDM-opdagelse ---------- */
static int gdm_data(char *out, size_t n) {
    SDL_LockMutex(mu);
    int r = snprintf(out, n,
                     "Content-Type: plex/media-player\r\nResource-Identifier: %s\r\nName: %s\r\nPort: %d\r\nProduct: NotaTV\r\n"
                     "Version: " TV_VERSION "\r\nProtocol: plex\r\nProtocol-Version: 1\r\nProtocol-Capabilities: " CAPS "\r\n"
                     "Device-Class: stb\r\n",
                     my_id, my_name, HTTP_PORT);
    SDL_UnlockMutex(mu);
    return r;
}

static int gdm_thread(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), one = 1, ttl = 255;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(GDM_PORT), .sin_addr.s_addr = INADDR_ANY};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        SDL_Log("plexcast: kan ikke binde GDM-port %d", GDM_PORT);
        if (s >= 0) close(s);
        return 0;
    }
    struct sockaddr_in grp = {.sin_family = AF_INET, .sin_port = htons(32413)};
    inet_pton(AF_INET, GDM_GROUP, &grp.sin_addr);
    double last_hello = -1000, last_join = -1000;
    char data[1024], msg[1200];
    for (;;) {
        double now = app_time();
        if (now - last_join > 30) { /* the network may come later: announce again regularly */
            struct ip_mreq mr = {.imr_multiaddr = grp.sin_addr, .imr_interface.s_addr = INADDR_ANY};
            setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
            last_join = now;
        }
        if (SDL_GetAtomicInt(&enabled) && now - last_hello > 60) {
            gdm_data(data, sizeof data);
            int n = snprintf(msg, sizeof msg, "HELLO * HTTP/1.0\r\n%s", data);
            sendto(s, msg, (size_t)n, 0, (struct sockaddr *)&grp, sizeof grp);
            last_hello = now;
        }
        struct pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, 2000) <= 0) continue;
        char in[1024];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(s, in, sizeof in - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        in[r] = 0;
        if (!SDL_GetAtomicInt(&enabled) || !strstr(in, "M-SEARCH * HTTP/1.")) continue;
        gdm_data(data, sizeof data);
        int n = snprintf(msg, sizeof msg, "HTTP/1.0 200 OK\r\n%s", data);
        sendto(s, msg, (size_t)n, 0, (struct sockaddr *)&from, fl);
    }
    return 0;
}

/* ---------- timeline for subscribers (POST http://app:port/:/timeline) ---------- */
static int notify_thread(void *arg) {
    (void)arg;
    unsigned sent_seq = 0;
    double last_full = 0;
    for (;;) {
        SDL_Delay(1000);
        SDL_LockMutex(mu);
        double now = app_time();
        for (int i = 0; i < PC.nsubs;) /* apps renew the subscription; forgotten ones disappear after 90 s */
            if (now - PC.subs[i].seen > 90) PC.subs[i] = PC.subs[--PC.nsubs];
            else i++;
        bool due = PC.nsubs > 0 && (PC.seq != sent_seq || PC.active || now - last_full > 10);
        Sub subs[8];
        int ns = PC.nsubs;
        memcpy(subs, PC.subs, sizeof subs);
        char *xml[8] = {0};
        if (due)
            for (int i = 0; i < ns; i++) xml[i] = timeline_xml(subs[i].cmd_id);
        sent_seq = PC.seq;
        SDL_UnlockMutex(mu);
        if (!due) continue;
        last_full = now;
        for (int i = 0; i < ns; i++) {
            char url[160], h1[160], h2[200];
            snprintf(url, sizeof url, "http://%s:%d/:/timeline", subs[i].ip, subs[i].port);
            snprintf(h1, sizeof h1, "X-Plex-Client-Identifier: %s", my_id);
            snprintf(h2, sizeof h2, "X-Plex-Device-Name: %s", my_name);
            const char *hd[] = {"Content-Type: text/xml", h1, h2, "X-Plex-Product: NotaTV", "X-Plex-Platform: Linux", NULL};
            HttpBuf b;
            http_request("POST", url, hd, xml[i], &b, 2);
            http_free(&b);
            free(xml[i]);
        }
    }
    return 0;
}

/* ---------- main thread ---------- */
void plexcast_apply(void) { SDL_SetAtomicInt(&enabled, cfg_int("plexcast_enabled", 1) && app_on("plex")); }

void plexcast_tick(void) {
    if (!started) {
        started = true;
        mu = SDL_CreateMutex();
        changed = SDL_CreateCondition();
        snprintf(my_id, sizeof my_id, "%s", plex_client_id());
        snprintf(my_name, sizeof my_name, "%s", cfg_get("device_name", "NotaTV"));
        plexcast_apply();
        SDL_Thread *t;
        if ((t = SDL_CreateThread(http_thread, "plexcast-http", NULL))) SDL_DetachThread(t);
        if ((t = SDL_CreateThread(gdm_thread, "plexcast-gdm", NULL))) SDL_DetachThread(t);
        if ((t = SDL_CreateThread(notify_thread, "plexcast-notify", NULL))) SDL_DetachThread(t);
        SDL_Log("plexcast: klar (GDM %d, HTTP %d)", GDM_PORT, HTTP_PORT);
        return;
    }
    const char *name = cfg_get("device_name", "NotaTV");
    if (strcmp(name, my_name)) { SDL_LockMutex(mu); snprintf(my_name, sizeof my_name, "%s", name); SDL_UnlockMutex(mu); }
    /* outside playback: playMedia starts, navigation becomes keys */
    PCmd q[32];
    PlexCastMedia m;
    bool play = false;
    SDL_LockMutex(mu);
    int n = PC.qn;
    memcpy(q, PC.q, sizeof(PCmd) * n);
    PC.qn = 0;
    PC.skip = 0;
    if (PC.have_play) { m = PC.play; PC.have_play = false; play = true; }
    SDL_UnlockMutex(mu);
    for (int i = 0; i < n; i++)
        if (q[i].cmd == P_NAV) input_push((Action)q[i].arg, 0, false);
    if (play) plex_cast_play(&m);
}

/* from the Plex playback's rq.tick: control from the app + state for the timeline */
void plexcast_play_tick(void) {
    if (!started) return;
    PCmd q[32];
    SDL_LockMutex(mu);
    int n = PC.qn;
    memcpy(q, PC.q, sizeof(PCmd) * n);
    PC.qn = 0;
    double base = PC.base;
    SDL_UnlockMutex(mu);
    for (int i = 0; i < n; i++) {
        char a[32];
        switch (q[i].cmd) {
        case P_PLAY: player_cmd((const char *[]){"quit", NULL}); break; /* new content: plexcast_tick starts it */
        case P_PAUSE: player_cmd((const char *[]){"set", "pause", "yes", NULL}); break;
        case P_RESUME: player_cmd((const char *[]){"set", "pause", "no", NULL}); break;
        case P_STOP: player_cmd((const char *[]){"quit", NULL}); break;
        case P_SEEK:
            snprintf(a, sizeof a, "%.1f", q[i].arg - base > 0 ? q[i].arg - base : 0);
            player_cmd((const char *[]){"seek", a, "absolute", NULL});
            break;
        case P_SEEKREL:
            snprintf(a, sizeof a, "%.0f", q[i].arg);
            player_cmd((const char *[]){"seek", a, "relative", NULL});
            break;
        case P_NEXT: case P_PREV:
            SDL_LockMutex(mu);
            PC.skip = q[i].cmd == P_NEXT ? 1 : -1;
            SDL_UnlockMutex(mu);
            player_cmd((const char *[]){"quit", NULL});
            break;
        case P_VOLUME:
            snprintf(a, sizeof a, "%d", (int)q[i].arg);
            player_cmd((const char *[]){"set", "volume", a, NULL});
            SDL_LockMutex(mu);
            PC.volume = (int)q[i].arg;
            bump();
            SDL_UnlockMutex(mu);
            break;
        case P_NAV: input_push((Action)q[i].arg, 0, false); break;
        }
    }
    double pos, dur;
    bool paused;
    if (player_state(&pos, &dur, &paused)) {
        SDL_LockMutex(mu);
        bool ch = paused != PC.paused || (int)pos != (int)PC.pos;
        PC.pos = pos;
        if (PC.now.duration <= 0) PC.dur = dur + PC.base;
        PC.paused = paused;
        if (ch) bump();
        SDL_UnlockMutex(mu);
    }
}

void plexcast_playing(const PlexCastNow *now, double base) {
    if (!started) return;
    SDL_LockMutex(mu);
    PC.now = *now;
    PC.active = true;
    PC.paused = false;
    PC.pos = 0;
    PC.base = base;
    PC.dur = now->duration;
    bump();
    SDL_UnlockMutex(mu);
}

void plexcast_stopped(void) {
    if (!started) return;
    SDL_LockMutex(mu);
    PC.active = false;
    bump();
    SDL_UnlockMutex(mu);
}

int plexcast_take_skip(void) {
    if (!started) return 0;
    SDL_LockMutex(mu);
    int s = PC.skip;
    PC.skip = 0;
    SDL_UnlockMutex(mu);
    return s;
}

bool plexcast_play_pending(void) {
    if (!started) return false;
    SDL_LockMutex(mu);
    bool p = PC.have_play;
    SDL_UnlockMutex(mu);
    return p;
}
