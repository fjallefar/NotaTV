/* NotaTV app package "tesla" - a small MQTT 3.1.1 client (QoS 0, read only) for the topics TeslaMate publishes:
 *   teslamate/[<namespace>/]cars/<car id>/<key>   e.g. teslamate/cars/1/battery_level = "78"
 * TeslaMate keeps them retained on the server, so everything is known right after subscribing, and changes arrive
 * live. The box only reads: it never publishes anything to the server.
 *
 * Also finds servers on our own subnet (port scan of 1883, then a short subscribe to see if TeslaMate topics are
 * there; port 4000 = TeslaMate's web page, to explain when only that answers). Everything stays on the local network. */
#define _GNU_SOURCE
#include "tesla.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---------- socket + MQTT packets ---------- */

static int tcp_connect(const char *host, int port, int timeout_ms) {
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0};
            int err = -1;
            socklen_t el = sizeof err;
            if (poll(&p, 1, timeout_ms) == 1) getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            r = err == 0 ? 0 : -1;
        }
        if (r == 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static bool send_all(int fd, const uint8_t *b, size_t n) {
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return false; }
        b += w;
        n -= w;
    }
    return true;
}

/* read exactly n bytes before the deadline */
static bool read_full(int fd, uint8_t *b, size_t n, double deadline) {
    while (n) {
        int left = (int)((deadline - now_s()) * 1000);
        if (left <= 0) return false;
        struct pollfd p = {fd, POLLIN, 0};
        int pr = poll(&p, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return false;
        ssize_t r = recv(fd, b, n, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
        b += r;
        n -= r;
    }
    return true;
}

typedef struct { uint8_t b[600]; size_t n; } Pkt;

static void put_u16(Pkt *p, unsigned v) { p->b[p->n++] = v >> 8; p->b[p->n++] = v & 0xff; }
static void put_str(Pkt *p, const char *s) {
    size_t l = strlen(s);
    if (l > 250) l = 250;
    put_u16(p, l);
    memcpy(p->b + p->n, s, l);
    p->n += l;
}

/* fixed header (type byte + remaining length) in front of the body */
static bool send_pkt(int fd, uint8_t type, const Pkt *body) {
    uint8_t h[5];
    size_t hn = 0, len = body ? body->n : 0;
    h[hn++] = type;
    do { uint8_t d = len % 128; len /= 128; if (len) d |= 0x80; h[hn++] = d; } while (len);
    return send_all(fd, h, hn) && (!body || !body->n || send_all(fd, body->b, body->n));
}

static bool mqtt_connect(int fd, const char *user, const char *pass) {
    Pkt p = {.n = 0};
    put_str(&p, "MQTT");
    p.b[p.n++] = 4; /* 3.1.1 */
    uint8_t flags = 0x02; /* clean session */
    if (user && *user) { flags |= 0x80; if (pass && *pass) flags |= 0x40; }
    p.b[p.n++] = flags;
    put_u16(&p, 60); /* keep alive */
    char cid[32];
    snprintf(cid, sizeof cid, "notatv-%08x", (unsigned)(rand() ^ (unsigned)time(NULL) ^ (unsigned)getpid()));
    put_str(&p, cid);
    if (flags & 0x80) put_str(&p, user);
    if (flags & 0x40) put_str(&p, pass);
    return send_pkt(fd, 0x10, &p);
}

static bool mqtt_subscribe(int fd) {
    Pkt p = {.n = 0};
    put_u16(&p, 1);
    put_str(&p, "teslamate/#");
    p.b[p.n++] = 0; /* QoS 0 */
    return send_pkt(fd, 0x82, &p);
}

/* one packet: its type byte, body in buf (cut at cap, the rest is skipped). -1 = error/timeout */
static long read_pkt(int fd, uint8_t *type, uint8_t *buf, size_t cap, double first_deadline) {
    if (!read_full(fd, type, 1, first_deadline)) return -1;
    double dl = now_s() + 10;
    size_t len = 0, mul = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t d;
        if (!read_full(fd, &d, 1, dl)) return -1;
        len += (d & 0x7f) * mul;
        mul *= 128;
        if (!(d & 0x80)) break;
        if (i == 3) return -1;
    }
    size_t take = len < cap ? len : cap;
    if (take && !read_full(fd, buf, take, dl)) return -1;
    for (size_t left = len - take; left;) {
        uint8_t junk[512];
        size_t n = left < sizeof junk ? left : sizeof junk;
        if (!read_full(fd, junk, n, dl)) return -1;
        left -= n;
    }
    return (long)take;
}

/* teslamate/[ns/]cars/<id>/<key>: car id + key; false for other topics */
static bool parse_topic(const char *t, int *car, char *key, size_t kn) {
    if (strncmp(t, "teslamate/", 10)) return false;
    const char *c = strstr(t, "/cars/");
    if (!c) return false;
    c += 6;
    char *end;
    long id = strtol(c, &end, 10);
    if (end == c || *end != '/' || id <= 0) return false;
    const char *k = end + 1;
    if (!*k || strchr(k, '/')) return false;
    *car = (int)id;
    snprintf(key, kn, "%s", k);
    return true;
}

/* a PUBLISH body -> topic + payload (both 0-terminated, cut to their sizes) */
static bool parse_publish(uint8_t type, const uint8_t *b, long n, char *topic, size_t tn, char *val, size_t vn) {
    if (n < 2) return false;
    size_t tl = (b[0] << 8) | b[1];
    if ((long)(2 + tl) > n) return false;
    size_t o = 2 + tl;
    if ((type >> 1) & 3) o += 2; /* QoS > 0: packet id (we ask for QoS 0, but be safe) */
    if ((long)o > n) return false;
    size_t c = tl < tn - 1 ? tl : tn - 1;
    memcpy(topic, b + 2, c);
    topic[c] = 0;
    size_t vl = n - o;
    if (vl > vn - 1) vl = vn - 1;
    memcpy(val, b + o, vl);
    val[vl] = 0;
    return true;
}

/* ---------- the live connection ---------- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static TmCar cars[TM_MAX_CARS];
static int ncars;
static TmState state = TM_OFF;
static unsigned gen;     /* bumped whenever the connection must stop or restart */
static int cur_fd = -1;  /* the running thread's socket (shut down to wake it) */
static char cur_sig[400]; /* host|port|user of the values in memory */

typedef struct { unsigned gen; char host[128], user[128], pass[128]; int port; } ConnCfg;

static void store(int car, const char *key, const char *val) {
    TmCar *c = NULL;
    for (int i = 0; i < ncars; i++) if (cars[i].id == car) c = &cars[i];
    if (!c) {
        if (ncars >= TM_MAX_CARS) return;
        int at = ncars;
        while (at > 0 && cars[at - 1].id > car) at--;
        memmove(&cars[at + 1], &cars[at], (ncars - at) * sizeof cars[0]);
        c = &cars[at];
        memset(c, 0, sizeof *c);
        c->id = car;
        ncars++;
    }
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->kv[i].k, key)) { snprintf(c->kv[i].v, sizeof c->kv[i].v, "%s", val); return; }
    if (c->n >= TM_MAX_KV) return;
    snprintf(c->kv[c->n].k, sizeof c->kv[c->n].k, "%s", key);
    snprintf(c->kv[c->n].v, sizeof c->kv[c->n].v, "%s", val);
    c->n++;
}

static void set_state(unsigned g, TmState s) {
    pthread_mutex_lock(&mu);
    if (g == gen) state = s;
    pthread_mutex_unlock(&mu);
    app_request_redraw();
}

static bool stale(unsigned g) {
    pthread_mutex_lock(&mu);
    bool s = g != gen;
    pthread_mutex_unlock(&mu);
    return s;
}

/* wait secs, but stop early when the connection is no longer wanted */
static bool nap(unsigned g, double secs) {
    for (double end = now_s() + secs; now_s() < end;) {
        if (stale(g)) return false;
        usleep(200000);
    }
    return !stale(g);
}

static void *conn_thread(void *arg) {
    ConnCfg *cfg = arg;
    unsigned g = cfg->gen;
    double backoff = 2;
    while (!stale(g)) {
        set_state(g, TM_CONNECTING);
        int fd = tcp_connect(cfg->host, cfg->port, 4000);
        if (fd < 0) {
            set_state(g, TM_UNREACHABLE);
            if (!nap(g, backoff)) break;
            backoff = backoff < 30 ? backoff * 2 : 60;
            continue;
        }
        pthread_mutex_lock(&mu);
        bool ok = g == gen;
        if (ok) cur_fd = fd;
        pthread_mutex_unlock(&mu);
        if (!ok) { close(fd); break; }
        uint8_t type, buf[1024];
        long n;
        TmState fail = TM_UNREACHABLE;
        if (mqtt_connect(fd, cfg->user, cfg->pass) && (n = read_pkt(fd, &type, buf, sizeof buf, now_s() + 5)) >= 2 &&
            (type & 0xf0) == 0x20) {
            if (buf[1] == 4 || buf[1] == 5) fail = TM_AUTH;
            else if (buf[1] == 0 && mqtt_subscribe(fd)) {
                set_state(g, TM_LIVE);
                backoff = 2;
                double last_tx = now_s(), last_rx = now_s();
                bool pinged = false;
                while (!stale(g)) {
                    double t = now_s();
                    if (t - last_tx > 30) {
                        if (!send_pkt(fd, 0xC0, NULL)) break;
                        last_tx = t;
                        pinged = true;
                    }
                    if (pinged && t - last_rx > 25 && t - last_tx > 20) break; /* no answer to the ping */
                    struct pollfd p = {fd, POLLIN, 0};
                    int pr = poll(&p, 1, 1000);
                    if (pr < 0 && errno == EINTR) continue;
                    if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL) && !(p.revents & POLLIN))) break;
                    if (pr == 0) continue;
                    if ((n = read_pkt(fd, &type, buf, sizeof buf, now_s() + 5)) < 0) break;
                    last_rx = now_s();
                    pinged = false;
                    if ((type & 0xf0) != 0x30) continue; /* SUBACK, PINGRESP */
                    char topic[300], val[400], key[40];
                    int car;
                    if (!parse_publish(type, buf, n, topic, sizeof topic, val, sizeof val)) continue;
                    if (!parse_topic(topic, &car, key, sizeof key)) continue;
                    pthread_mutex_lock(&mu);
                    if (g == gen) store(car, key, val);
                    pthread_mutex_unlock(&mu);
                    app_request_redraw();
                }
                fail = TM_CONNECTING;
            }
        }
        pthread_mutex_lock(&mu);
        if (cur_fd == fd) cur_fd = -1;
        pthread_mutex_unlock(&mu);
        close(fd);
        if (stale(g)) break;
        set_state(g, fail);
        if (!nap(g, fail == TM_AUTH ? 60 : fail == TM_CONNECTING ? 2 : backoff)) break;
        if (fail != TM_CONNECTING) backoff = backoff < 30 ? backoff * 2 : 60;
    }
    memset(cfg->pass, 0, sizeof cfg->pass);
    free(cfg);
    return NULL;
}

/* stop the running thread (it notices the new gen); call with mu held */
static void stop_locked(void) {
    gen++;
    if (cur_fd >= 0) shutdown(cur_fd, SHUT_RDWR);
    cur_fd = -1;
    state = TM_OFF;
}

void tm_conn_apply(void) {
    const char *host = cfg_get("tesla_host", "");
    bool want = app_on("tesla") && *host;
    char sig[400];
    snprintf(sig, sizeof sig, "%s|%d|%s|%s", host, cfg_int("tesla_port", 1883), cfg_get("tesla_user", ""), cfg_get("tesla_pass", ""));
    pthread_mutex_lock(&mu);
    bool running = state != TM_OFF;
    bool same = !strcmp(sig, cur_sig);
    if (running && want && same) { pthread_mutex_unlock(&mu); return; }
    if (running) stop_locked();
    if (!same || !want) {
        memset(cars, 0, sizeof cars);
        ncars = 0;
    }
    memset(cur_sig, 0, sizeof cur_sig);
    if (!want) { pthread_mutex_unlock(&mu); return; }
    snprintf(cur_sig, sizeof cur_sig, "%s", sig);
    memset(sig, 0, sizeof sig);
    ConnCfg *c = calloc(1, sizeof *c);
    c->gen = gen;
    snprintf(c->host, sizeof c->host, "%s", host);
    c->port = cfg_int("tesla_port", 1883);
    snprintf(c->user, sizeof c->user, "%s", cfg_get("tesla_user", ""));
    snprintf(c->pass, sizeof c->pass, "%s", cfg_get("tesla_pass", ""));
    state = TM_CONNECTING;
    pthread_t t;
    if (pthread_create(&t, NULL, conn_thread, c) == 0) pthread_detach(t);
    else { state = TM_OFF; free(c); }
    pthread_mutex_unlock(&mu);
}

void tm_conn_forget(void) {
    pthread_mutex_lock(&mu);
    stop_locked();
    memset(cars, 0, sizeof cars);
    ncars = 0;
    memset(cur_sig, 0, sizeof cur_sig);
    pthread_mutex_unlock(&mu);
}

TmState tm_state(void) {
    pthread_mutex_lock(&mu);
    TmState s = state;
    pthread_mutex_unlock(&mu);
    return s;
}

int tm_cars(TmCar *out, int max) {
    pthread_mutex_lock(&mu);
    int n = ncars < max ? ncars : max;
    memcpy(out, cars, n * sizeof *out);
    pthread_mutex_unlock(&mu);
    return n;
}

const char *tm_get(const TmCar *c, const char *key) {
    if (!c) return "";
    for (int i = 0; i < c->n; i++) if (!strcmp(c->kv[i].k, key)) return c->kv[i].v;
    return "";
}

/* ---------- one short look at a server ---------- */

static void add_name(char *cars_out, size_t cn, const char *name) {
    if (!cars_out || !*name || strstr(cars_out, name)) return;
    size_t l = strlen(cars_out);
    snprintf(cars_out + l, cn - l, "%s%s", l ? ", " : "", name);
}

/* 0 = TeslaMate topics, 1 = login needed/refused, 2 = MQTT without TeslaMate, 3 = unreachable */
static int probe(const char *host, int port, const char *user, const char *pass, char *cars_out, size_t cn) {
    if (cars_out) cars_out[0] = 0;
    int fd = tcp_connect(host, port, 2000);
    if (fd < 0) return 3;
    uint8_t type, buf[1024];
    long n;
    int r = 3;
    if (mqtt_connect(fd, user, pass) && (n = read_pkt(fd, &type, buf, sizeof buf, now_s() + 3)) >= 2 && (type & 0xf0) == 0x20) {
        if (buf[1] == 4 || buf[1] == 5) r = 1;
        else if (buf[1] == 0 && mqtt_subscribe(fd)) {
            r = 2;
            /* retained topics come in one burst: stop 0.4 s after the last one, at most 2.5 s */
            double end = now_s() + 2.5, quiet = 0;
            while (now_s() < end && (!quiet || now_s() < quiet)) {
                double dl = quiet && quiet < end ? quiet : end;
                if ((n = read_pkt(fd, &type, buf, sizeof buf, dl)) < 0) break;
                if ((type & 0xf0) != 0x30) continue;
                char topic[300], val[400], key[40];
                int car;
                if (!parse_publish(type, buf, n, topic, sizeof topic, val, sizeof val)) continue;
                if (!strncmp(topic, "teslamate/", 10)) { r = 0; quiet = now_s() + 0.4; }
                if (parse_topic(topic, &car, key, sizeof key) && !strcmp(key, "display_name")) add_name(cars_out, cn, val);
            }
            send_pkt(fd, 0xE0, NULL); /* DISCONNECT */
        }
    }
    close(fd);
    return r;
}

typedef struct {
    char host[128], user[128], pass[128], cars[96];
    int port, result;
    TmProbeDone cb;
    void *ud;
} ProbeJob;

static void probe_work(void *arg) {
    ProbeJob *j = arg;
    j->result = probe(j->host, j->port, j->user, j->pass, j->cars, sizeof j->cars);
}
static void probe_done(void *arg) {
    ProbeJob *j = arg;
    j->cb(j->ud, j->result, j->cars);
    memset(j->pass, 0, sizeof j->pass);
    free(j);
}

void tm_probe(const char *host, int port, const char *user, const char *pass, TmProbeDone cb, void *ud) {
    ProbeJob *j = calloc(1, sizeof *j);
    snprintf(j->host, sizeof j->host, "%s", host);
    snprintf(j->user, sizeof j->user, "%s", user ? user : "");
    snprintf(j->pass, sizeof j->pass, "%s", pass ? pass : "");
    j->port = port;
    j->cb = cb;
    j->ud = ud;
    job_submit_q(JQ_SLOW, probe_work, probe_done, j);
}

/* ---------- discovery ---------- */

#define MAX_HOSTS 1024
#define MAX_FOUND 16

typedef struct { uint32_t addr, mask; } Net;

static int local_nets(Net *out, int max) {
    struct ifaddrs *ifa, *i;
    int n = 0;
    if (getifaddrs(&ifa) < 0) return 0;
    for (i = ifa; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        out[n].addr = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        out[n].mask = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr;
        n++;
    }
    freeifaddrs(ifa);
    return n;
}

/* the addresses on our subnets (at most /22; bigger: only our own /24) where the TCP port is open */
static int port_scan(const Net *nets, int nn, int port, uint32_t *open, int max_open) {
    static uint32_t hosts[MAX_HOSTS];
    int nh = 0, no = 0;
    for (int i = 0; i < nn; i++) {
        uint32_t mask = ntohl(nets[i].mask), net = ntohl(nets[i].addr) & mask, size = ~mask + 1;
        if (size > 1024) {
            mask = 0xffffff00;
            net = ntohl(nets[i].addr) & mask;
            size = 256;
        }
        for (uint32_t h = 1; h + 1 < size && nh < MAX_HOSTS; h++) {
            uint32_t ip = htonl(net + h);
            if (ip != nets[i].addr) hosts[nh++] = ip;
        }
    }
    for (int base = 0; base < nh; base += 256) {
        int cnt = nh - base < 256 ? nh - base : 256, np = 0;
        struct pollfd pf[256];
        uint32_t ipof[256];
        for (int k = 0; k < cnt; k++) {
            int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (s < 0) break;
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = hosts[base + k]};
            int r = connect(s, (struct sockaddr *)&a, sizeof a);
            if (r < 0 && errno != EINPROGRESS) { close(s); continue; }
            pf[np] = (struct pollfd){s, POLLOUT, 0};
            ipof[np++] = hosts[base + k];
        }
        double end = now_s() + 0.8;
        int pending = np;
        while (pending > 0) {
            int left = (int)((end - now_s()) * 1000);
            if (left <= 0 || poll(pf, np, left) <= 0) break;
            for (int k = 0; k < np; k++) {
                if (pf[k].fd < 0 || !pf[k].revents) continue;
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(pf[k].fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (!err && (pf[k].revents & POLLOUT) && no < max_open) open[no++] = ipof[k];
                close(pf[k].fd);
                pf[k].fd = -1;
                pending--;
            }
        }
        for (int k = 0; k < np; k++) if (pf[k].fd >= 0) close(pf[k].fd);
    }
    return no;
}

typedef struct {
    TmFound f[MAX_FOUND];
    int n;
    TmDiscDone cb;
    void *ud;
} Disc;

static void disc_work(void *arg) {
    Disc *d = arg;
    if (getenv("TVSHELL_TESLA_NOSCAN")) return; /* headless tests on a dev PC: never touch the real network */
    Net nets[8];
    int nn = local_nets(nets, 8);
    if (!nn) return;
    uint32_t mq[64], web[64];
    int nmq = port_scan(nets, nn, 1883, mq, 64), nweb = port_scan(nets, nn, 4000, web, 64);
    for (int i = 0; i < nmq && d->n < MAX_FOUND; i++) {
        TmFound *f = &d->f[d->n];
        inet_ntop(AF_INET, &mq[i], f->addr, sizeof f->addr);
        f->port = 1883;
        int r = probe(f->addr, 1883, "", "", f->cars, sizeof f->cars);
        if (r == 3) continue;
        f->kind = r == 0 ? TD_TESLAMATE : r == 1 ? TD_LOGIN : TD_NOTM;
        d->n++;
    }
    for (int i = 0; i < nweb && d->n < MAX_FOUND; i++) {
        bool has = false;
        for (int k = 0; k < nmq; k++) has |= mq[k] == web[i];
        if (has) continue;
        TmFound *f = &d->f[d->n++];
        inet_ntop(AF_INET, &web[i], f->addr, sizeof f->addr);
        f->port = 4000;
        f->kind = TD_WEBONLY;
    }
    /* TeslaMate first, then servers that want a login, then the rest */
    for (int i = 1; i < d->n; i++)
        for (int k = i; k > 0 && d->f[k].kind < d->f[k - 1].kind; k--) {
            TmFound t = d->f[k];
            d->f[k] = d->f[k - 1];
            d->f[k - 1] = t;
        }
    SDL_Log("tesla: discovery - %d MQTT, %d web, %d listed", nmq, nweb, d->n);
}

static void disc_done(void *arg) {
    Disc *d = arg;
    d->cb(d->ud, d->f, d->n);
    free(d);
}

void tm_discover(TmDiscDone cb, void *ud) {
    Disc *d = calloc(1, sizeof *d);
    d->cb = cb;
    d->ud = ud;
    job_submit_q(JQ_SLOW, disc_work, disc_done, d);
}
