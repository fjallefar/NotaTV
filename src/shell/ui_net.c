/* NotaTV shell - Network monitor: which programs on the box are talking to whom right now.
 * Reads /proc/net/{tcp,tcp6,udp,udp6} and links sockets to processes via /proc/<pid>/fd.
 * Host names are looked up in the background (reverse DNS) and remembered. Everything stays on the box. */
#include "tv.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CONN 256
#define MAX_SEEN 256

typedef struct {
    char proc[32], ip[64], host[160];
    int lport, rport;
    bool tcp, listen, incoming, lan;
} Conn;

typedef struct { char ip[64], host[160]; bool done; } DnsEntry;
static DnsEntry dns[256];
static int n_dns;

typedef struct {
    Conn now[MAX_CONN];
    int n_now;
    Conn seen[MAX_SEEN]; /* everything seen outward since the screen was opened */
    double seen_t[MAX_SEEN];
    int n_seen;
    bool busy;
    double next;
    int scroll, sel;
    bool alive;
} Net;

/* ---------- collection (background thread) ---------- */
typedef struct { unsigned long inode; int pid; } InoPid;

static int map_inodes(InoPid *out, int max) {
    int n = 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/fd", pid);
        DIR *fd = opendir(path);
        if (!fd) continue;
        struct dirent *f;
        while ((f = readdir(fd)) && n < max) {
            char lp[96], tgt[64];
            snprintf(lp, sizeof lp, "%s/%s", path, f->d_name);
            ssize_t l = readlink(lp, tgt, sizeof tgt - 1);
            if (l <= 0) continue;
            tgt[l] = 0;
            unsigned long ino;
            if (sscanf(tgt, "socket:[%lu]", &ino) == 1) out[n++] = (InoPid){ino, pid};
        }
        closedir(fd);
    }
    closedir(d);
    return n;
}

static void proc_name(int pid, char *out, size_t n) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) { if (fgets(out, (int)n, f)) str_trim(out); fclose(f); }
    if (!out[0]) snprintf(out, n, "pid %d", pid);
}

/* /proc/net addresses: IPv4 "0100007F:0050", IPv6 32 hex characters (4 words in host order) */
static bool parse_addr(const char *s, bool v6, char *ip, size_t n, int *port) {
    const char *colon = strchr(s, ':');
    if (!colon) return false;
    *port = (int)strtol(colon + 1, NULL, 16);
    unsigned char b[16];
    int words = v6 ? 4 : 1;
    for (int w = 0; w < words; w++) {
        char h[9];
        memcpy(h, s + w * 8, 8);
        h[8] = 0;
        unsigned v = (unsigned)strtoul(h, NULL, 16);
        memcpy(b + w * 4, &v, 4);
    }
    if (v6 && !memcmp(b, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12)) { inet_ntop(AF_INET, b + 12, ip, (socklen_t)n); return true; } /* IPv4-mappet */
    inet_ntop(v6 ? AF_INET6 : AF_INET, b, ip, (socklen_t)n);
    return true;
}

static bool is_any(const char *ip) { return !strcmp(ip, "0.0.0.0") || !strcmp(ip, "::"); }
static bool is_loop(const char *ip) { return !strncmp(ip, "127.", 4) || !strcmp(ip, "::1"); }
static bool is_lan(const char *ip) {
    if (!strncmp(ip, "10.", 3) || !strncmp(ip, "192.168.", 8) || !strncmp(ip, "169.254.", 8)) return true;
    if (!strncmp(ip, "172.", 4)) { int b = atoi(ip + 4); return b >= 16 && b <= 31; }
    if (!strncmp(ip, "fe80", 4) || !strncmp(ip, "fd", 2) || !strncmp(ip, "fc", 2)) return true;
    if (!strncmp(ip, "224.", 4) || !strncmp(ip, "239.", 4) || !strncmp(ip, "ff", 2)) return true; /* multicast */
    return false;
}

typedef struct { Conn c[MAX_CONN]; int n; } Scan;

static void scan_file(Scan *sc, const char *file, bool tcp, bool v6, const InoPid *ip, int nip, int *lports, int *nl) {
    FILE *f = fopen(file, "r");
    if (!f) return;
    char line[512];
    if (!fgets(line, sizeof line, f)) { fclose(f); return; } /* heading */
    while (fgets(line, sizeof line, f) && sc->n < MAX_CONN) {
        char la[64], ra[64];
        unsigned st;
        unsigned long ino = 0;
        if (sscanf(line, "%*d: %63s %63s %x %*s %*s %*s %*s %*s %lu", la, ra, &st, &ino) < 4) continue;
        char lip[64], rip[64];
        int lp, rp;
        if (!parse_addr(la, v6, lip, sizeof lip, &lp) || !parse_addr(ra, v6, rip, sizeof rip, &rp)) continue;
        bool listen = tcp ? st == 0x0A : is_any(rip);
        if (!listen && (is_loop(rip) || (tcp && st != 0x01 && st != 0x02 && st != 0x03))) continue; /* only active ones, not internal */
        if (listen && is_loop(lip)) continue;
        Conn *c = &sc->c[sc->n++];
        memset(c, 0, sizeof *c);
        c->tcp = tcp;
        c->listen = listen;
        c->lport = lp;
        c->rport = rp;
        snprintf(c->ip, sizeof c->ip, "%s", listen ? "" : rip);
        c->lan = listen || is_lan(rip);
        snprintf(c->proc, sizeof c->proc, "%s", "?");
        for (int i = 0; i < nip; i++) if (ip[i].inode == ino && ino) { proc_name(ip[i].pid, c->proc, sizeof c->proc); break; }
        if (listen && *nl < 64) lports[(*nl)++] = lp;
    }
    fclose(f);
}

static void dns_lookup(const char *ip, char *out, size_t n) {
    struct sockaddr_storage ss = {0};
    socklen_t sl;
    if (strchr(ip, ':')) { struct sockaddr_in6 *a = (void *)&ss; a->sin6_family = AF_INET6; inet_pton(AF_INET6, ip, &a->sin6_addr); sl = sizeof *a; }
    else { struct sockaddr_in *a = (void *)&ss; a->sin_family = AF_INET; inet_pton(AF_INET, ip, &a->sin_addr); sl = sizeof *a; }
    if (getnameinfo((struct sockaddr *)&ss, sl, out, (socklen_t)n, NULL, 0, NI_NAMEREQD) != 0) out[0] = 0;
}

typedef struct { Net *v; Scan sc; char lookups[16][64]; char results[16][160]; int n_look; } NetJob;

static void net_work(void *arg) {
    NetJob *j = arg;
    InoPid *ip = malloc(sizeof(InoPid) * 4096);
    int nip = ip ? map_inodes(ip, 4096) : 0;
    int lports[64], nl = 0;
    scan_file(&j->sc, "/proc/net/tcp", true, false, ip, nip, lports, &nl);
    scan_file(&j->sc, "/proc/net/tcp6", true, true, ip, nip, lports, &nl);
    scan_file(&j->sc, "/proc/net/udp", false, false, ip, nip, lports, &nl);
    scan_file(&j->sc, "/proc/net/udp6", false, true, ip, nip, lports, &nl);
    free(ip);
    for (int i = 0; i < j->sc.n; i++) { /* incoming = our local port is a listening port */
        Conn *c = &j->sc.c[i];
        if (c->listen) continue;
        for (int k = 0; k < nl; k++) if (lports[k] == c->lport) { c->incoming = true; break; }
    }
    for (int i = 0; i < j->n_look; i++) dns_lookup(j->lookups[i], j->results[i], sizeof j->results[i]);
}

static Net *live_net;

static DnsEntry *dns_find(const char *ip) {
    for (int i = 0; i < n_dns; i++) if (!strcmp(dns[i].ip, ip)) return &dns[i];
    return NULL;
}

static int conn_rank(const Conn *c) { return c->listen ? 3 : c->lan ? (c->incoming ? 2 : 1) : 0; } /* internet first */

static void net_done(void *arg) {
    NetJob *j = arg;
    Net *v = j->v;
    for (int i = 0; i < j->n_look; i++) { /* store DNS replies (also empty ones: so we do not ask again) */
        DnsEntry *d = dns_find(j->lookups[i]);
        if (!d && n_dns < 256) d = &dns[n_dns++];
        if (!d) continue;
        snprintf(d->ip, sizeof d->ip, "%s", j->lookups[i]);
        snprintf(d->host, sizeof d->host, "%s", j->results[i]);
        d->done = true;
    }
    if (v != live_net) { /* the screen was closed while we were scanning: clean up here */
        if (!v->alive) free(v);
        free(j);
        return;
    }
    {
        v->busy = false;
        /* sort: internet, local out, local in, listening */
        for (int a = 1; a < j->sc.n; a++)
            for (int b = a; b > 0 && conn_rank(&j->sc.c[b - 1]) > conn_rank(&j->sc.c[b]); b--) { Conn t = j->sc.c[b]; j->sc.c[b] = j->sc.c[b - 1]; j->sc.c[b - 1] = t; }
        memcpy(v->now, j->sc.c, sizeof(Conn) * j->sc.n);
        v->n_now = j->sc.n;
        for (int i = 0; i < v->n_now; i++) {
            Conn *c = &v->now[i];
            DnsEntry *d = c->ip[0] ? dns_find(c->ip) : NULL;
            if (d) snprintf(c->host, sizeof c->host, "%s", d->host);
            if (c->listen || c->incoming || c->lan) continue;
            int k = 0; /* history: outgoing to the internet */
            while (k < v->n_seen && !(strcmp(v->seen[k].proc, c->proc) == 0 && strcmp(v->seen[k].ip, c->ip) == 0 && v->seen[k].rport == c->rport)) k++;
            if (k == v->n_seen && v->n_seen < MAX_SEEN) v->seen[v->n_seen++] = *c;
            if (k < MAX_SEEN) { v->seen[k] = *c; v->seen_t[k] = app_time(); }
        }
    }
    free(j);
}

static void net_refresh(Net *v) {
    NetJob *j = calloc(1, sizeof *j);
    j->v = v;
    for (int i = 0; i < v->n_now && j->n_look < 16; i++) { /* new IPs: look up the name in the same round */
        const char *ip = v->now[i].ip;
        if (!ip[0] || dns_find(ip)) continue;
        bool dup = false;
        for (int k = 0; k < j->n_look && !dup; k++) dup = !strcmp(j->lookups[k], ip);
        if (!dup) snprintf(j->lookups[j->n_look++], 64, "%s", ip);
    }
    v->busy = true;
    job_submit_q(JQ_SLOW, net_work, net_done, j);
}

/* ---------- screen ---------- */
#define ROW_H 54
#define LIST_TOP 250

static int n_rows(Net *v) { return v->n_now + (v->n_seen ? v->n_seen + 1 : 0); }

static void net_update(Screen *s, float dt) {
    (void)dt;
    Net *v = s->st;
    if (!v->busy && app_time() >= v->next) { v->next = app_time() + 2; net_refresh(v); }
}

static void draw_conn(const Conn *c, float y, bool sel, bool old) {
    float x = 120;
    if (sel) fill_rrect(x - 20, y - 8, UI_W - 200, ROW_H - 6, 12, RGBA(255, 255, 255, 22));
    Color tag = c->listen ? C_TEXT3 : c->lan ? RGB(90, 170, 255) : RGB(255, 170, 60);
    const char *kind = c->listen ? T("LISTENING") : c->lan ? (c->incoming ? T("LOCAL IN") : T("LOCAL OUT")) : "INTERNET";
    float tw = text_width(F_BOLD, 18, kind) + 20;
    fill_rrect(x, y + 2, tw, 30, 8, color_alpha(tag, old ? 0.12f : 0.22f));
    text_draw(F_BOLD, 18, x + tw / 2, y + 7, tag, AL_CENTER, kind);
    Color tc = old ? C_TEXT3 : C_TEXT;
    text_draw_clip(F_SEMI, 26, x + 190, y, 300, tc, AL_LEFT, c->proc);
    char where[300];
    if (c->listen) snprintf(where, sizeof where, T("port %d (%s) - reachable from the network"), c->lport, c->tcp ? "TCP" : "UDP");
    else {
        const char *h = c->host[0] ? c->host : c->ip;
        if (strchr(h, ':') && !c->host[0]) snprintf(where, sizeof where, "[%s]:%d", h, c->rport);
        else snprintf(where, sizeof where, "%s:%d", h, c->rport);
    }
    text_draw_clip(F_REG, 26, x + 510, y, 900, old ? C_TEXT3 : C_TEXT2, AL_LEFT, where);
    if (!c->listen && c->host[0]) text_draw_clip(F_REG, 20, x + 1430, y + 4, 300, C_TEXT3, AL_LEFT, c->ip);
}

static void net_draw(Screen *s) {
    Net *v = s->st;
    draw_background();
    draw_topbar(T("Network monitor"));
    text_draw(F_BOLD, 44, 120, 130, C_TEXT, AL_LEFT, T("Who is the box talking to?"));
    int inet = 0, lan = 0, lis = 0;
    for (int i = 0; i < v->n_now; i++) { if (v->now[i].listen) lis++; else if (v->now[i].lan) lan++; else inet++; }
    char sum[200];
    snprintf(sum, sizeof sum, T("%d to the internet  ·  %d on the local network  ·  %d listening  ·  updated every 2 seconds"), inet, lan, lis);
    text_draw(F_REG, 24, 120, 192, C_TEXT2, AL_LEFT, sum);
    clip_push(0, LIST_TOP - 10, UI_W, UI_H - LIST_TOP - 80);
    int total = n_rows(v);
    for (int r = v->scroll; r < total; r++) {
        float y = LIST_TOP + (r - v->scroll) * ROW_H;
        if (y > UI_H - 90) break;
        if (r < v->n_now) draw_conn(&v->now[r], y, r == v->sel, false);
        else if (r == v->n_now) text_draw(F_SEMI, 24, 120, y + 10, C_TEXT2, AL_LEFT, T("Seen on the internet since you opened this page (including closed connections):"));
        else {
            int k = r - v->n_now - 1;
            Conn c = v->seen[k];
            c.listen = false;
            DnsEntry *d = dns_find(c.ip);
            if (d) snprintf(c.host, sizeof c.host, "%s", d->host);
            draw_conn(&c, y, r == v->sel, app_time() - v->seen_t[k] > 3);
        }
    }
    if (!total) { draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT); }
    clip_pop();
    text_draw(F_REG, 22, 120, UI_H - 62, C_TEXT3, AL_LEFT,
              T("Orange = the internet  ·  Blue = your local network  ·  Gray = ports the box listens on. Very short connections may end between two updates."));
}

static bool net_input(Screen *s, const InputEvent *ev) {
    Net *v = s->st;
    int total = n_rows(v), vis = (UI_H - LIST_TOP - 90) / ROW_H;
    switch (ev->a) {
    case A_UP: if (v->sel > 0) v->sel--; break;
    case A_DOWN: if (v->sel < total - 1) v->sel++; break;
    case A_CHUP: v->sel = v->sel > vis ? v->sel - vis : 0; break;
    case A_CHDOWN: v->sel = v->sel + vis < total ? v->sel + vis : (total ? total - 1 : 0); break;
    default: return false;
    }
    if (v->sel < v->scroll) v->scroll = v->sel;
    if (v->sel >= v->scroll + vis) v->scroll = v->sel - vis + 1;
    return true;
}

static void net_enter(Screen *s) { live_net = s->st; }
static void net_destroy(Screen *s) {
    Net *v = s->st;
    v->alive = false;
    if (live_net == v) live_net = NULL;
    /* a job may still be running: then net_done frees v when it is done */
    if (!v->busy) free(v);
    free(s);
}

Screen *netwatch_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    Net *v = calloc(1, sizeof *v);
    v->alive = true;
    s->name = "netwatch";
    s->st = v;
    s->enter = net_enter;
    s->update = net_update;
    s->draw = net_draw;
    s->input = net_input;
    s->destroy = net_destroy;
    return s;
}
