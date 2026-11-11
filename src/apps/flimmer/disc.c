/* NotaTV app "flimmer" - find Flimmertv servers on the local network
 *
 * Port scan of our own subnets (at most /22; bigger: only our own /24) for port 7676 (Flimmertv's usual port),
 * then each open port is confirmed over HTTP: GET /api/v1/app/server answers {"service": "flimmertv"} (no sign-in
 * needed). Everything happens locally; nothing is sent out to the internet.
 * Same method as the Seerr app's search (its own copy, so it needs no new tvshell). */
#include "flimmer.h"
#include "vendor/cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FL_PORT 7676
#define MAX_HOSTS 1024

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

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

/* the addresses on our subnets where the TCP port is open (our own address too: Flimmertv may run on the box's LAN
   neighbor or, in tests, on this machine) */
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
        for (uint32_t h = 1; h + 1 < size && nh < MAX_HOSTS; h++) hosts[nh++] = htonl(net + h);
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

static cJSON *get_json(const char *url) {
    static const char *const hdr[] = {"Accept: application/json", NULL};
    HttpBuf b;
    cJSON *j = NULL;
    if (http_request("GET", url, hdr, NULL, &b, 3) && b.status == 200 && b.data) j = cJSON_Parse(b.data);
    http_free(&b);
    return j;
}

typedef struct {
    FlFound f[FL_MAX_FOUND];
    int n;
    FlDiscDone cb;
    void *ud;
} Disc;

static void disc_work(void *arg) {
    Disc *d = arg;
    if (getenv("TVSHELL_FLIMMER_NOSCAN")) return; /* headless tests on a dev PC: never touch the real network */
    Net nets[8];
    int nn = local_nets(nets, 8);
    if (!nn) return;
    uint32_t open[64];
    int no = port_scan(nets, nn, FL_PORT, open, 64);
    for (int i = 0; i < no && d->n < FL_MAX_FOUND; i++) {
        char ip[INET_ADDRSTRLEN], base[64];
        inet_ntop(AF_INET, &open[i], ip, sizeof ip);
        snprintf(base, sizeof base, "http://%s:%d", ip, FL_PORT);
        char *u = str_printf("%s/api/v1/app/server", base);
        cJSON *st = get_json(u);
        free(u);
        cJSON *sv = cJSON_GetObjectItem(st, "service");
        bool ok = cJSON_IsString(sv) && !strcmp(sv->valuestring, "flimmertv");
        cJSON_Delete(st);
        if (!ok) continue; /* something else on port 7676 */
        FlFound *f = &d->f[d->n++];
        snprintf(f->url, sizeof f->url, "%s", base);
        snprintf(f->name, sizeof f->name, "Flimmertv");
    }
    SDL_Log("flimmer: discovery - %d open, %d server(s)", no, d->n);
}

static void disc_done(void *arg) {
    Disc *d = arg;
    d->cb(d->ud, d->f, d->n);
    free(d);
}

void flimmer_discover(FlDiscDone cb, void *ud) {
    Disc *d = xcalloc(1, sizeof *d);
    d->cb = cb;
    d->ud = ud;
    job_submit_q(JQ_SLOW, disc_work, disc_done, d);
}
