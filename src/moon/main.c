/* tvmoon - command line: discover PCs, pairing, app list, start/quit and the stream itself.
 *
 * For tvshell, one line per result is written to stdout (fields separated by tabs); everything else is logged
 * to stderr. Certificate, key and client id live in /data/notatv/moonlight (root only).
 */
#define _GNU_SOURCE
#include "tvmoon.h"
#include <gamestream/client.h>
#include <gamestream/errors.h>
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *key_dir(void) {
    const char *d = getenv("TVMOON_DIR");
    return d && *d ? d : TVMOON_DIR;
}

double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

void tlog(const char *fmt, ...) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm;
    localtime_r(&t.tv_sec, &tm);
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%02d:%02d:%02d.%03ld %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec / 1000000, buf);
}

void tout(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

/* "192.168.1.20" or "192.168.1.20:47989" */
static int server_open(SERVER_DATA *srv, const char *arg, char *addr, size_t n) {
    snprintf(addr, n, "%s", arg);
    unsigned short port = 0;
    char *c = strrchr(addr, ':');
    if (c && !strchr(addr, ']') && strchr(addr, ':') == c) { *c = 0; port = (unsigned short)atoi(c + 1); }
    mkdir(key_dir(), 0700);
    memset(srv, 0, sizeof *srv);
    /* unsupported=true: Sunshine does not report all resolutions/versions like GFE, but handles them */
    int r = gs_init(srv, addr, port, key_dir(), getenv("TVMOON_DEBUG") ? 2 : 0, true);
    if (r != GS_OK) tlog("gs_init %s: %d (%s)", addr, r, gs_error ? gs_error : "?");
    return r;
}

/* ---------- discover PCs with Sunshine/GFE via mDNS (_nvstream._tcp) ---------- */

typedef struct { char name[64], addr[48]; int port; } Found;
static Found found[16];
static int n_found, pending;
static bool all_for_now;
static AvahiClient *av_client;

static void resolve_cb(AvahiServiceResolver *r, AvahiIfIndex iface, AvahiProtocol proto, AvahiResolverEvent ev,
                       const char *name, const char *type, const char *domain, const char *host,
                       const AvahiAddress *a, uint16_t port, AvahiStringList *txt, AvahiLookupResultFlags fl, void *ud) {
    if (ev == AVAHI_RESOLVER_FOUND && a && n_found < (int)(sizeof found / sizeof found[0])) {
        char s[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(s, sizeof s, a);
        bool dup = false;
        for (int i = 0; i < n_found; i++) dup |= !strcmp(found[i].addr, s);
        if (!dup) {
            Found *f = &found[n_found++];
            snprintf(f->name, sizeof f->name, "%s", name);
            snprintf(f->addr, sizeof f->addr, "%s", s);
            f->port = port;
        }
    }
    avahi_service_resolver_free(r);
    pending--;
}

static void browse_cb(AvahiServiceBrowser *b, AvahiIfIndex iface, AvahiProtocol proto, AvahiBrowserEvent ev,
                      const char *name, const char *type, const char *domain, AvahiLookupResultFlags fl, void *ud) {
    if (ev == AVAHI_BROWSER_NEW) {
        if (avahi_service_resolver_new(av_client, iface, proto, name, type, domain, AVAHI_PROTO_INET, 0, resolve_cb, NULL))
            pending++;
    } else if (ev == AVAHI_BROWSER_ALL_FOR_NOW || ev == AVAHI_BROWSER_FAILURE) {
        all_for_now = true;
    }
}

/* ---------- fallback: port scan of our own subnet for 47989 (when mDNS is blocked, e.g. by the PC's firewall) ---------- */

static size_t grow_cb(char *p, size_t sz, size_t n, void *ud) {
    char *b = ud;
    size_t l = strlen(b), add = sz * n;
    if (l + add < 4096) { memcpy(b + l, p, add); b[l + add] = 0; }
    return sz * n;
}

/* the PC's name from Sunshine's /serverinfo (answers over HTTP without pairing); false = not a GameStream host */
static bool host_name(const char *ip, char *out, size_t n) {
    char url[96], buf[4096] = "";
    snprintf(url, sizeof url, "http://%s:47989/serverinfo", ip);
    CURL *c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, grow_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, buf);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK || !strstr(buf, "<root")) return false;
    const char *h = strstr(buf, "<hostname>");
    snprintf(out, n, "%s", ip);
    if (h) {
        h += 10;
        size_t l = strcspn(h, "<");
        if (l > 0 && l < n) { memcpy(out, h, l); out[l] = 0; }
    }
    return true;
}

static void port_scan(void) {
    enum { MAXH = 1024, BATCH = 256 };
    static uint32_t hosts[MAXH];
    int nh = 0;
    struct ifaddrs *ifa;
    if (getifaddrs(&ifa) < 0) return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        uint32_t self = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        uint32_t mask = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        if (~mask + 1u > 1024u) mask = 0xffffff00; /* bigger than /22: only our own /24 */
        uint32_t net = ntohl(self) & mask, size = ~mask + 1u;
        for (uint32_t h = 1; h + 1 < size && nh < MAXH; h++) {
            uint32_t ip = htonl(net + h);
            bool dup = ip == self;
            for (int k = 0; k < nh && !dup; k++) dup = hosts[k] == ip;
            if (!dup) hosts[nh++] = ip;
        }
    }
    freeifaddrs(ifa);
    /* non-blocking connect in batches of 256, 0.8 s per batch (a /24 = one batch) */
    for (int base = 0; base < nh; base += BATCH) {
        int cnt = nh - base < BATCH ? nh - base : BATCH, np = 0, left_n;
        struct pollfd pf[BATCH];
        uint32_t ipof[BATCH];
        for (int k = 0; k < cnt; k++) {
            int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (s < 0) break;
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(47989), .sin_addr.s_addr = hosts[base + k]};
            if (connect(s, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(s); continue; }
            pf[np] = (struct pollfd){s, POLLOUT, 0};
            ipof[np++] = hosts[base + k];
        }
        double end = now_s() + 0.8;
        left_n = np;
        while (left_n > 0) {
            int ms = (int)((end - now_s()) * 1000);
            if (ms <= 0 || poll(pf, np, ms) <= 0) break;
            for (int k = 0; k < np; k++) {
                if (pf[k].fd < 0 || !pf[k].revents) continue;
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(pf[k].fd, SOL_SOCKET, SO_ERROR, &err, &el);
                close(pf[k].fd);
                pf[k].fd = -1;
                left_n--;
                if (err || !(pf[k].revents & POLLOUT) || n_found >= (int)(sizeof found / sizeof found[0])) continue;
                char ip[INET_ADDRSTRLEN], name[64];
                inet_ntop(AF_INET, &ipof[k], ip, sizeof ip);
                if (!host_name(ip, name, sizeof name)) continue; /* something else listens on 47989 */
                Found *f = &found[n_found++];
                snprintf(f->name, sizeof f->name, "%s", name);
                snprintf(f->addr, sizeof f->addr, "%s", ip);
                f->port = 47989;
                tlog("discover: %s (%s) found by port scan", ip, name);
            }
        }
        for (int k = 0; k < np; k++) if (pf[k].fd >= 0) close(pf[k].fd);
    }
}

/* scan: 1 = port scan the subnet if mDNS found nothing */
static int cmd_discover(int ms, bool scan) {
    AvahiSimplePoll *sp = avahi_simple_poll_new();
    int err = 0;
    av_client = sp ? avahi_client_new(avahi_simple_poll_get(sp), 0, NULL, NULL, &err) : NULL;
    if (!av_client) {
        tout("error\tavahi: %s", avahi_strerror(err));
        if (sp) avahi_simple_poll_free(sp);
        return 1;
    }
    AvahiServiceBrowser *sb = avahi_service_browser_new(av_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, "_nvstream._tcp",
                                                        NULL, 0, browse_cb, NULL);
    double end = now_s() + ms / 1000.0, quiet = now_s() + 0.4;
    /* Avahi answers from its cache right away ("all for now"); wait briefly for new answers from the network */
    while (sb && now_s() < end && !(all_for_now && pending == 0 && now_s() > quiet))
        if (avahi_simple_poll_iterate(sp, 50) != 0) break;
    if (sb) avahi_service_browser_free(sb);
    avahi_client_free(av_client);
    avahi_simple_poll_free(sp);
    if (scan && n_found == 0) port_scan();
    /* status per PC: paired, running game (serverinfo over HTTP/HTTPS) */
    for (int i = 0; i < n_found; i++) {
        SERVER_DATA srv;
        char a[64], arg[80];
        snprintf(arg, sizeof arg, "%s:%d", found[i].addr, found[i].port);
        int r = server_open(&srv, arg, a, sizeof a);
        tout("server\t%s\t%s\t%d\t%s\t%d", found[i].name, found[i].addr, found[i].port,
             r != GS_OK ? "offline" : srv.paired ? "paired" : "unpaired", r == GS_OK ? srv.currentGame : 0);
    }
    return 0;
}

static int cmd_info(const char *host) {
    SERVER_DATA srv;
    char a[64];
    if (server_open(&srv, host, a, sizeof a) != GS_OK) { tout("error\t%s", gs_error ? gs_error : T("no connection")); return 1; }
    tout("info\t%s\t%d\t%s\t0x%x", srv.paired ? "paired" : "unpaired", srv.currentGame,
         srv.serverInfo.serverInfoAppVersion ? srv.serverInfo.serverInfoAppVersion : "?",
         srv.serverInfo.serverCodecModeSupport);
    return 0;
}

static int cmd_pair(const char *host) {
    SERVER_DATA srv;
    char a[64];
    if (server_open(&srv, host, a, sizeof a) != GS_OK) { tout("error\t%s", gs_error ? gs_error : T("no connection")); return 1; }
    if (srv.paired) { tout("paired"); return 0; }
    unsigned char rnd[2] = {0};
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { if (read(fd, rnd, 2) != 2) rnd[0] = (unsigned char)time(NULL); close(fd); }
    char pin[5];
    snprintf(pin, sizeof pin, "%04u", ((rnd[0] << 8) | rnd[1]) % 10000u);
    /* the PIN is shown on the TV; the user types it on Sunshine's web page (https://<pc>:47990) */
    tout("pin\t%s", pin);
    int r = gs_pair(&srv, pin);
    if (r != GS_OK) { tout("error\t%s", gs_error ? gs_error : T("pairing failed")); return 1; }
    tout("paired");
    return 0;
}

static int cmd_unpair(const char *host) {
    SERVER_DATA srv;
    char a[64];
    if (server_open(&srv, host, a, sizeof a) != GS_OK) { tout("error\t%s", gs_error ? gs_error : T("no connection")); return 1; }
    int r = gs_unpair(&srv);
    tout(r == GS_OK ? "unpaired" : "error\t%s", T("could not unpair"));
    return r == GS_OK ? 0 : 1;
}

static int cmd_apps(const char *host) {
    SERVER_DATA srv;
    char a[64];
    if (server_open(&srv, host, a, sizeof a) != GS_OK) { tout("error\t%s", gs_error ? gs_error : T("no connection")); return 1; }
    if (!srv.paired) { tout("error\t%s", T("not paired")); return 1; }
    PAPP_LIST list = NULL;
    if (gs_applist(&srv, &list) != GS_OK) { tout("error\t%s", gs_error ? gs_error : T("the app list failed")); return 1; }
    tout("running\t%d", srv.currentGame);
    for (PAPP_LIST l = list; l; l = l->next) tout("app\t%d\t%s", l->id, l->name ? l->name : "?");
    return 0;
}

static int cmd_quit(const char *host) {
    SERVER_DATA srv;
    char a[64];
    if (server_open(&srv, host, a, sizeof a) != GS_OK) { tout("error\t%s", gs_error ? gs_error : T("no connection")); return 1; }
    int r = gs_quit_app(&srv);
    tout(r == GS_OK ? "quit" : "error\t%s", T("could not quit the game"));
    return r == GS_OK ? 0 : 1;
}

/* ---------- streaming ---------- */

static volatile sig_atomic_t got_signal;
static volatile bool conn_ended, conn_started;
static volatile int conn_error;

static void on_signal(int s) { got_signal = s; }

static void cl_stage_failed(int stage, int err) {
    tlog("forbindelse: trin '%s' fejlede (%d)", LiGetStageName(stage), err);
    { char m[200]; snprintf(m, sizeof m, T("%s failed (%d)"), LiGetStageName(stage), err); tout("error\t%s", m); }
}
static void cl_started(void) { conn_started = true; tlog("forbindelse: streamer"); tout("status\tstreaming"); }
static void cl_terminated(int err) {
    tlog("forbindelse: afsluttet (%d)", err);
    conn_error = err;
    conn_ended = true;
}
static void cl_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t l = strlen(buf);
    while (l && buf[l - 1] == '\n') buf[--l] = 0;
    tlog("ml: %s", buf);
}
static void cl_status(int st) { tlog("forbindelse: %s", st == CONN_STATUS_POOR ? "DÅRLIG" : "ok igen"); }
static void cl_hdr(bool on) { tlog("værten: HDR %s", on ? "til" : "fra"); video_set_hdr(on); }

typedef struct {
    int w, h, fps, bitrate;
    const char *codec; /* "auto", "hevc", "h264" */
    bool hdr;
    const char *mode;
} StreamOpts;

/* is there a hardware H.264 decoder (the Pi 4's bcm2835-codec via v4l2m2m)? The Pi 5 has none, only HEVC */
static bool h264_hw(void) {
    for (int i = 0; i < 32; i++) {
        char p[64], n[64] = "";
        snprintf(p, sizeof p, "/sys/class/video4linux/video%d/name", i);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        bool hit = fgets(n, sizeof n, f) && strstr(n, "codec-decode");
        fclose(f);
        if (hit) return true;
    }
    return false;
}

static int cmd_stream(const char *host, int appid, const StreamOpts *o) {
    SERVER_DATA srv;
    char a[64];
    tout("status\tconnecting");
    if (server_open(&srv, host, a, sizeof a) != GS_OK) { tout("error\t%s", gs_error ? gs_error : T("no connection")); return 1; }
    if (!srv.paired) { tout("error\t%s", T("not paired")); return 1; }

    STREAM_CONFIGURATION sc;
    LiInitializeStreamConfiguration(&sc);
    sc.width = o->w;
    sc.height = o->h;
    sc.fps = o->fps;
    sc.bitrate = o->bitrate;
    sc.packetSize = 1392;
    sc.streamingRemotely = STREAM_CFG_AUTO;
    sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    sc.clientRefreshRateX100 = o->fps * 100;
    /* Pi 4: HEVC goes through the stateless v4l2-request decoder (low latency, up to 4K),
       H.264 through v4l2m2m (max 1080p). Both are announced; the host prefers HEVC.
       Pi 5: only HEVC has a hardware decoder, so H.264 is never asked for there. */
    bool h264 = h264_hw();
    if (!h264 && strcmp(o->codec, "hevc") && strcmp(o->codec, "auto")) tlog("stream: ingen H.264-dekoder (Pi 5) - bruger HEVC");
    if (!strcmp(o->codec, "h264") && h264) sc.supportedVideoFormats = VIDEO_FORMAT_H264;
    else if (!strcmp(o->codec, "hevc") || !h264) sc.supportedVideoFormats = VIDEO_FORMAT_H265;
    else sc.supportedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;
    if (o->hdr) sc.supportedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
    sc.colorSpace = o->hdr ? COLORSPACE_REC_2020 : COLORSPACE_REC_709;
    sc.colorRange = COLOR_RANGE_LIMITED;
    /* Pi 4 has no AES instructions: only audio is encrypted (input is always encrypted) */
    sc.encryptionFlags = ENCFLG_AUDIO;

    int r = gs_start_app(&srv, &sc, appid, false, false, 1);
    if (r != GS_OK) {
        tlog("gs_start_app: %d (%s)", r, gs_error ? gs_error : "?");
        { char m[300]; snprintf(m, sizeof m, T("could not start the app (%s)"), gs_error ? gs_error : "?"); tout("error\t%s", m); }
        return 1;
    }
    tlog("app %d startet på %s: %dx%d@%d, %d kbit/s, codec %s%s", appid, a, o->w, o->h, o->fps, o->bitrate,
         o->codec, o->hdr ? " HDR" : "");

    VideoOpts vo = {.mode = o->mode};
    video_configure(&vo);

    CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeConnectionCallbacks(&cl);
    cl.stageFailed = cl_stage_failed;
    cl.connectionStarted = cl_started;
    cl.connectionTerminated = cl_terminated;
    cl.logMessage = cl_log;
    cl.connectionStatusUpdate = cl_status;
    cl.setHdrMode = cl_hdr;

    r = LiStartConnection(&srv.serverInfo, &sc, &cl, &video_callbacks, &audio_callbacks, NULL, 0, NULL, 0);
    if (r != 0) {
        tlog("LiStartConnection: %d", r);
        return 1;
    }
    input_start();
    double last = now_s();
    VStat vst = {0};
    uint32_t rtt = 0, var = 0;
    while (!got_signal && !conn_ended && !quit_requested) {
        usleep(20000);
        bool redraw = false;
        static bool stats_on;
        if (stats_toggle) { stats_toggle = false; stats_on = !stats_on; redraw = true; }
        if (menu_pick >= 0) {
            int pick = menu_pick;
            menu_pick = -1;
            if (pick == 1) stats_on = !stats_on;
            if (pick == 2) { tlog("menu: afslut"); quit_requested = true; break; }
            redraw = true;
        }
        if (menu_dirty) { menu_dirty = false; redraw = true; }
        if (menu_open) {
            if (redraw) {
                char st[48];
                snprintf(st, sizeof st, "%s", stats_on ? T("Hide stats") : T("Show stats"));
                const char *lines[6] = {T("NotaTV · games"), T("Continue the game"), st, T("Quit and go back to NotaTV"), "", T("OK select  ·  Back/Red closes")};
                overlay_set_text(lines, 6, 1 + menu_sel, vst.hdr);
                overlay_show(true);
            }
            continue;
        }
        if (redraw) overlay_show(stats_on);
        if (now_s() - last >= 1) {
            last = now_s();
            char vs[256], as[128];
            video_stats(vs, sizeof vs, &vst);
            audio_stats(as, sizeof as);
            LiGetEstimatedRttInfo(&rtt, &var);
            tlog("stat: %s | %s | rtt %u±%u ms", vs, as, rtt, var);
            redraw = true;
        }
        if (redraw && stats_on) {
            char l[7][96];
            snprintf(l[0], sizeof l[0], T("%dx%d · %s%s · display %dp %d Hz"), vst.w, vst.h, vst.hevc ? "HEVC" : "H.264",
                     vst.hdr ? " HDR" : "", vst.disp_h, vst.hz);
            snprintf(l[1], sizeof l[1], T("FPS: %d shown / %d received (%d skipped)"), vst.shown, vst.rx, vst.skip);
            snprintf(l[2], sizeof l[2], "Data: %.1f Mbit/s   RTT: %u ms", vst.mbit, rtt);
            snprintf(l[3], sizeof l[3], T("PC (Sunshine): %.1f ms"), vst.host_ms);
            snprintf(l[4], sizeof l[4], T("Decoding: %.1f ms"), vst.dec_ms);
            snprintf(l[5], sizeof l[5], T("Received → display: %.1f ms (max %.0f)"), vst.total_ms, vst.max_ms);
            snprintf(l[6], sizeof l[6], T("Total before the TV: approx. %.0f ms"), vst.host_ms + rtt / 2.0 + vst.total_ms);
            const char *lines[7] = {l[0], l[1], l[2], l[3], l[4], l[5], l[6]};
            overlay_set_text(lines, 7, -1, vst.hdr);
        }
    }
    tlog("stopper (%s)", got_signal ? "signal" : quit_requested ? "afslut-kombination" : "forbindelsen sluttede");
    input_stop();
    LiStopConnection();
    if (conn_ended && conn_error != ML_ERROR_GRACEFUL_TERMINATION) {
        { char m[200]; snprintf(m, sizeof m, T("the connection was lost (%d)"), conn_error); tout("error\t%s", m); }
        return 2;
    }
    tout("status\tstopped");
    return 0;
}

int videotest_run(const char *path, int fps, const char *mode); /* videotest.c */

static void usage(void) {
    fprintf(stderr,
            "brug: tvmoon discover [ms] [scan]\n"
            "      tvmoon info|pair|unpair|apps|quit <pc[:port]>\n"
            "      tvmoon stream <pc[:port]> <app-id> [-r 1920x1080] [-f 60] [-b 20000] [-c auto|hevc|h264] [--hdr] [-m WxH]\n"
            "      tvmoon videotest <fil.h264|fil.hevc> [-f 60] [-m WxH]\n");
}

int main(int argc, char **argv) {
    umask(077); /* key and certificate for root only */
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    { /* language: tvshell sets NOTATV_LANG; the language files live with tvshell */
        const char *d = getenv("TVSHELL_ASSETS");
        i18n_load(d ? d : "/usr/share/tvshell", getenv("NOTATV_LANG"));
    }
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];
    /* only stream/videotest stop cleanly on a signal (got_signal ends their loop); the short commands must
     * die at once - "pair" waits in gs_pair until the PIN is typed on the PC, and Back on the TV sends SIGTERM */
    if (strcmp(cmd, "stream") && strcmp(cmd, "videotest")) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
    }
    if (!strcmp(cmd, "discover")) return cmd_discover(argc > 2 ? atoi(argv[2]) : 1500, argc > 3 && !strcmp(argv[3], "scan"));
    if (argc < 3) { usage(); return 1; }
    if (!strcmp(cmd, "info")) return cmd_info(argv[2]);
    if (!strcmp(cmd, "pair")) return cmd_pair(argv[2]);
    if (!strcmp(cmd, "unpair")) return cmd_unpair(argv[2]);
    if (!strcmp(cmd, "apps")) return cmd_apps(argv[2]);
    if (!strcmp(cmd, "quit")) return cmd_quit(argv[2]);
    StreamOpts o = {.w = 1920, .h = 1080, .fps = 60, .bitrate = 20000, .codec = "auto"};
    int first = !strcmp(cmd, "stream") ? 4 : 3;
    for (int i = first; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(argv[i], "-r")) { sscanf(v, "%dx%d", &o.w, &o.h); i++; }
        else if (!strcmp(argv[i], "-f")) { o.fps = atoi(v); i++; }
        else if (!strcmp(argv[i], "-b")) { o.bitrate = atoi(v); i++; }
        else if (!strcmp(argv[i], "-c")) { o.codec = v; i++; }
        else if (!strcmp(argv[i], "-m")) { o.mode = v; i++; }
        else if (!strcmp(argv[i], "--hdr")) o.hdr = true;
        else if (!strcmp(argv[i], "--stats")) setenv("TVMOON_STATS", "1", 1);
    }
    if (!strcmp(cmd, "stream") && argc >= 4) return cmd_stream(argv[2], atoi(argv[3]), &o);
    if (!strcmp(cmd, "videotest")) return videotest_run(argv[2], o.fps, o.mode);
    usage();
    return 1;
}

/* for videotest.c: stop on signal */
bool tvmoon_signalled(void) { return got_signal != 0 || quit_requested; }
