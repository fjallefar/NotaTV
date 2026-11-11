/* NotaTV shell - find Plex and Jellyfin servers on the local network
 *
 * Two methods at the same time, so it works both when the server answers "who is there?" and when it does not
 * (e.g. in Docker without host networking):
 *   1. UDP query: Plex GDM (port 32414, "M-SEARCH * HTTP/1.1") / Jellyfin (port 7359, "who is JellyfinServer?")
 *   2. Port scan of our own subnet (at most /22): Plex 32400, Jellyfin 8096 + 30013 (the TrueNAS app's default)
 * Each candidate is confirmed over HTTP (Plex /identity, Jellyfin /System/Info/Public), so only real servers are shown.
 * Everything happens locally; nothing is sent out to the internet. */
#include "tv.h"
#include "vendor/cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_FOUND 16
#define MAX_HOSTS 1024

typedef struct {
    DiscKind kind;
    DiscDone cb;
    void *ud;
    DiscServer s[MAX_FOUND];
    int n;
    /* candidates: IPv4 (network order) + port, name/id from the UDP reply if there was one */
    struct { uint32_t ip; int port; char name[128], id[96]; } c[64];
    int nc;
} Disc;

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

static void add_cand(Disc *d, uint32_t ip, int port, const char *name, const char *id) {
    for (int i = 0; i < d->nc; i++)
        if (d->c[i].ip == ip && d->c[i].port == port) {
            if (name && *name && !d->c[i].name[0]) snprintf(d->c[i].name, sizeof d->c[i].name, "%s", name);
            if (id && *id && !d->c[i].id[0]) snprintf(d->c[i].id, sizeof d->c[i].id, "%s", id);
            return;
        }
    if (d->nc >= (int)(sizeof d->c / sizeof d->c[0])) return;
    d->c[d->nc].ip = ip;
    d->c[d->nc].port = port;
    snprintf(d->c[d->nc].name, sizeof d->c[d->nc].name, "%s", name ? name : "");
    snprintf(d->c[d->nc].id, sizeof d->c[d->nc].id, "%s", id ? id : "");
    d->nc++;
}

/* "Name: value" from a GDM reply */
static void hdr_val(const char *msg, const char *key, char *out, size_t n) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = msg; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncasecmp(p, key, kl) && p[kl] == ':') {
            p += kl + 1;
            while (*p == ' ') p++;
            size_t l = strcspn(p, "\r\n");
            if (l >= n) l = n - 1;
            memcpy(out, p, l);
            out[l] = 0;
            return;
        }
    }
}

/* 1. UDP query to broadcast (+ GDM multicast), replies are collected for ~1.5 s */
static void udp_probe(Disc *d, const Net *nets, int nn) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), one = 1;
    if (s < 0) return;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    bool plex = d->kind == DISC_PLEX;
    int port = plex ? 32414 : 7359;
    const char *msg = plex ? "M-SEARCH * HTTP/1.1\r\n\r\n" : "who is JellyfinServer?";
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    a.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a);
    for (int i = 0; i < nn; i++) {
        a.sin_addr.s_addr = nets[i].addr | ~nets[i].mask;
        sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a);
    }
    if (plex) {
        inet_pton(AF_INET, "239.0.0.250", &a.sin_addr);
        sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a);
    }
    double end = app_time() + 1.5;
    for (;;) {
        int left = (int)((end - app_time()) * 1000);
        if (left <= 0) break;
        struct pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, left) <= 0) break;
        char buf[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        buf[r] = 0;
        if (plex) {
            char name[128], pt[16], id[96];
            hdr_val(buf, "Name", name, sizeof name);
            hdr_val(buf, "Port", pt, sizeof pt);
            hdr_val(buf, "Resource-Identifier", id, sizeof id);
            add_cand(d, from.sin_addr.s_addr, pt[0] ? atoi(pt) : 32400, name, id);
        } else {
            cJSON *j = cJSON_Parse(buf);
            cJSON *ad = cJSON_GetObjectItem(j, "Address"), *nm = cJSON_GetObjectItem(j, "Name"), *id = cJSON_GetObjectItem(j, "Id");
            int jp = 8096;
            if (cJSON_IsString(ad)) { /* "http://192.168.1.10:8096" - use the port, but the sender's IP */
                const char *c = strrchr(ad->valuestring, ':');
                if (c && atoi(c + 1) > 0) jp = atoi(c + 1);
            }
            add_cand(d, from.sin_addr.s_addr, jp, cJSON_IsString(nm) ? nm->valuestring : NULL, cJSON_IsString(id) ? id->valuestring : NULL);
            cJSON_Delete(j);
        }
    }
    close(s);
}

/* 2. TCP port scan of our own subnet: non-blocking connect in batches of 256 */
static void port_scan(Disc *d, const Net *nets, int nn, int port) {
    uint32_t hosts[MAX_HOSTS];
    int nh = 0;
    for (int i = 0; i < nn; i++) {
        uint32_t mask = ntohl(nets[i].mask), net = ntohl(nets[i].addr) & mask, size = ~mask + 1;
        if (size > 1024) { /* network bigger than /22: only scan our own /24 */
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
        int cnt = nh - base < 256 ? nh - base : 256;
        struct pollfd pf[256];
        int np = 0;
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
        double end = app_time() + 0.8;
        int pending = np;
        while (pending > 0) {
            int left = (int)((end - app_time()) * 1000);
            if (left <= 0 || poll(pf, np, left) <= 0) break;
            for (int k = 0; k < np; k++) {
                if (pf[k].fd < 0 || !pf[k].revents) continue;
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(pf[k].fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (!err && (pf[k].revents & POLLOUT)) add_cand(d, ipof[k], port, NULL, NULL);
                close(pf[k].fd);
                pf[k].fd = -1;
                pending--;
            }
        }
        for (int k = 0; k < np; k++) if (pf[k].fd >= 0) close(pf[k].fd);
    }
}

/* 3. confirm over HTTP and get name/id */
static void verify(Disc *d) {
    for (int i = 0; i < d->nc && d->n < MAX_FOUND; i++) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &d->c[i].ip, ip, sizeof ip);
        char base[160];
        snprintf(base, sizeof base, "http://%s:%d", ip, d->c[i].port);
        char *url = str_printf("%s%s", base, d->kind == DISC_PLEX ? "/identity" : "/System/Info/Public");
        static const char *const hdr[] = {"Accept: application/json", NULL};
        HttpBuf b;
        bool ok = http_request("GET", url, hdr, NULL, &b, 3) && b.status == 200 && b.data;
        free(url);
        char name[128], id[96];
        snprintf(name, sizeof name, "%s", d->c[i].name);
        snprintf(id, sizeof id, "%s", d->c[i].id);
        if (ok) {
            cJSON *j = cJSON_Parse(b.data);
            if (d->kind == DISC_PLEX) {
                /* JSON: {"MediaContainer":{"machineIdentifier":...}}; older servers reply with XML */
                cJSON *mc = cJSON_GetObjectItem(j, "MediaContainer"), *mi = cJSON_GetObjectItem(mc, "machineIdentifier");
                if (cJSON_IsString(mi)) snprintf(id, sizeof id, "%s", mi->valuestring);
                else if (!j) {
                    const char *m = strstr(b.data, "machineIdentifier=\"");
                    if (m) { m += 19; size_t l = strcspn(m, "\""); if (l >= sizeof id) l = sizeof id - 1; memcpy(id, m, l); id[l] = 0; }
                    else ok = false;
                } else ok = false;
            } else {
                cJSON *nm = cJSON_GetObjectItem(j, "ServerName"), *jid = cJSON_GetObjectItem(j, "Id");
                if (cJSON_IsString(nm)) snprintf(name, sizeof name, "%s", nm->valuestring);
                if (cJSON_IsString(jid)) snprintf(id, sizeof id, "%s", jid->valuestring);
                if (!cJSON_IsString(jid) && !cJSON_IsString(cJSON_GetObjectItem(j, "Version"))) ok = false;
            }
            cJSON_Delete(j);
        }
        http_free(&b);
        if (!ok) continue;
        bool dup = false;
        for (int k = 0; k < d->n; k++) dup |= id[0] && !strcmp(d->s[k].id, id);
        if (dup) continue;
        DiscServer *s = &d->s[d->n++];
        snprintf(s->name, sizeof s->name, "%s", name[0] ? name : d->kind == DISC_PLEX ? "Plex Media Server" : "Jellyfin");
        snprintf(s->url, sizeof s->url, "%s", base);
        snprintf(s->id, sizeof s->id, "%s", id);
    }
}

static void disc_work(void *arg) {
    Disc *d = arg;
    Net nets[8];
    int nn = local_nets(nets, 8);
    if (!nn) return;
    udp_probe(d, nets, nn);
    if (d->kind == DISC_PLEX) port_scan(d, nets, nn, 32400);
    else { port_scan(d, nets, nn, 8096); port_scan(d, nets, nn, 30013); }
    verify(d);
    SDL_Log("netdisc: %s - %d kandidat(er), %d server(e)", d->kind == DISC_PLEX ? "Plex" : "Jellyfin", d->nc, d->n);
}

static void disc_done(void *arg) {
    Disc *d = arg;
    d->cb(d->ud, d->s, d->n);
    free(d);
}

void netdisc_start(DiscKind kind, DiscDone cb, void *ud) {
    Disc *d = xcalloc(1, sizeof *d);
    d->kind = kind;
    d->cb = cb;
    d->ud = ud;
    job_submit_q(JQ_SLOW, disc_work, disc_done, d);
}
