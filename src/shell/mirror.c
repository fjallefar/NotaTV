/* NotaTV shell - screen mirroring on the TV.
 * AirPlay (iPhone/iPad/Mac): UxPlay (open source) runs as a child process and handles the AirPlay protocol and audio;
 * the video (H.264) is sent out on 127.0.0.1:7310 (tcpserversink) and shown by our mpv, so HDR, display modes
 * and switching between the UI and video are the same as for all other playback.
 * Android: Pixel phones have no Miracast, so the box accepts mirroring from our own (planned) app:
 * TCP port 7400 (mDNS _notatv-mirror._tcp), an MPEG-TS stream (H.264/H.265 + AAC/Opus), which mpv plays directly. */
#define _GNU_SOURCE
#include "tv.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define AIRPLAY_PORT 7300 /* UxPlay: TCP 7300-7302, UDP 7300-7302 (shairport-sync has 7000) */
#define VIDEO_PORT 7310   /* UxPlay -> tvshell: H.264 Annex-B */
#define ANDROID_PORT 7400

static SDL_Mutex *mu;
static bool started;
static pid_t ux_pid = -1;
static double ux_next_try;
static char ux_name[128];
static struct {
    bool airplay_start;  /* video came from UxPlay */
    int sink;            /* write end of the pipe to mpv (-1 = no player) */
    unsigned char *keep; /* video since the latest SPS, for a player that starts in the middle of the stream */
    size_t nkeep;
    int android_fd;      /* pending Android connection */
    double last_data;
} M = {.sink = -1, .android_fd = -1};

/* ---------- AirPlay: UxPlay as a child process ---------- */
static void ux_stop(void) {
    if (ux_pid > 0) { kill(ux_pid, SIGTERM); waitpid(ux_pid, NULL, 0); }
    ux_pid = -1;
}

static void ux_start(void) {
    if (access("/usr/bin/uxplay", X_OK) != 0) return;
    snprintf(ux_name, sizeof ux_name, "%s", cfg_get("device_name", "NotaTV"));
    char name[160], port[16];
    snprintf(name, sizeof name, T("%s (screen)"), ux_name); /* AirPlay audio has the name without "(screen)" (shairport-sync) */
    snprintf(port, sizeof port, "%d", AIRPLAY_PORT);
    char vrtp[256];
    snprintf(vrtp, sizeof vrtp, "config-interval=-1 ! rtph264depay ! h264parse config-interval=-1 ! "
                                "video/x-h264,stream-format=byte-stream,alignment=au ! tcpserversink host=127.0.0.1 port=%d sync=false",
             VIDEO_PORT);
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0);
        int lf = open("/tmp/uxplay.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
        setsid();
        /* die together with tvshell: a leftover UxPlay holds port 7300/7310, so the next one cannot start */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(0);
        execl("/usr/bin/uxplay", "uxplay", "-n", name, "-nh", "-p", port, "-s", "1920x1080@60", "-fps", "60", "-nohold",
              "-as", "alsasink device=default", "-vrtp", vrtp, (char *)NULL);
        _exit(127);
    }
    ux_pid = pid;
    SDL_Log("mirror: UxPlay startet som \"%s\"", name);
}

/* is an AirPlay client connected? (established TCP connections to UxPlay's port) */
static bool airplay_connected(void) {
    const char *files[] = {"/proc/net/tcp", "/proc/net/tcp6"};
    char want[8];
    snprintf(want, sizeof want, ":%04X", AIRPLAY_PORT);
    for (int f = 0; f < 2; f++) {
        FILE *fp = fopen(files[f], "r");
        if (!fp) continue;
        char line[512];
        while (fgets(line, sizeof line, fp)) {
            char local[128], remote[128], st[8];
            if (sscanf(line, "%*d: %127s %127s %7s", local, remote, st) != 3) continue;
            char *c = strrchr(local, ':');
            if (c && !strcmp(c, want) && !strcmp(st, "01")) { fclose(fp); return true; }
        }
        fclose(fp);
    }
    return false;
}

/* finds the latest SPS (NAL type 7) in Annex B data; -1 = none */
static long last_sps(const unsigned char *d, size_t n) {
    for (long i = (long)n - 5; i >= 0; i--)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1f) == 7) return i > 0 && d[i - 1] == 0 ? i - 1 : i;
    return -1;
}

/* reads the video from UxPlay all the time; keeps it from the latest SPS, or passes it on to mpv */
static int video_thread(void *arg) {
    (void)arg;
    unsigned char buf[65536];
    for (;;) {
        int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(VIDEO_PORT)};
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
            if (s >= 0) close(s);
            SDL_Delay(1000);
            continue;
        }
        for (;;) {
            ssize_t r = recv(s, buf, sizeof buf, 0);
            if (r <= 0) break;
            SDL_LockMutex(mu);
            M.last_data = app_time();
            int sink = M.sink;
            if (sink < 0) {
                M.keep = realloc(M.keep, M.nkeep + (size_t)r);
                memcpy(M.keep + M.nkeep, buf, (size_t)r);
                M.nkeep += (size_t)r;
                long k = last_sps(M.keep, M.nkeep);
                if (k > 0) { memmove(M.keep, M.keep + k, M.nkeep - (size_t)k); M.nkeep -= (size_t)k; }
                if (M.nkeep > (16u << 20)) M.nkeep = 0; /* no SPS in 16 MB: throw it away */
                M.airplay_start = true;
            }
            SDL_UnlockMutex(mu);
            if (sink >= 0) {
                const unsigned char *p = buf;
                size_t left = (size_t)r;
                while (left > 0) {
                    ssize_t w = write(sink, p, left);
                    if (w <= 0) break; /* mpv lukket */
                    p += w;
                    left -= (size_t)w;
                }
            }
        }
        close(s);
        SDL_Delay(500);
    }
    return 0;
}

/* ---------- Android: our own app sends MPEG-TS on port 7400 ---------- */
static int android_thread(void *arg) {
    (void)arg;
    int s = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0), off = 0, one = 1; /* not inherited by mpv/UxPlay */
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(ANDROID_PORT), .sin6_addr = in6addr_any};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 2) < 0) {
        SDL_Log("mirror: kan ikke lytte på port %d", ANDROID_PORT);
        if (s >= 0) close(s);
        return 0;
    }
    for (;;) {
        int fd = accept(s, NULL, NULL); /* without CLOEXEC: mpv inherits the connection */
        if (fd < 0) { SDL_Delay(200); continue; }
        if (!cfg_int("mirror_android", 1)) { close(fd); continue; }
        SDL_LockMutex(mu);
        if (M.android_fd >= 0) close(M.android_fd);
        M.android_fd = fd; /* newest wins */
        SDL_UnlockMutex(mu);
    }
    return 0;
}

/* ---------- playback (main thread) ---------- */
static bool stop_airplay;
static double since_disconnect;

static void airplay_tick(void *ud) {
    (void)ud;
    /* end when the phone has disconnected (no TCP connection for 2 s) */
    if (airplay_connected()) since_disconnect = 0;
    else if (since_disconnect == 0) since_disconnect = app_time();
    else if (app_time() - since_disconnect > 2) stop_airplay = true;
    if (stop_airplay) player_cmd((const char *[]){"quit", NULL});
}

static const char *const LIVE_ARGS[] = {
    "--profile=low-latency", "--untimed", "--cache=no", "--demuxer-lavf-o-add=fflags=+nobuffer",
    "--demuxer-lavf-analyzeduration=0.2", "--demuxer-lavf-probesize=65536", "--keep-open=no", NULL, NULL, NULL};

static void play_airplay(void) {
    int p[2];
    if (pipe(p) < 0) return;
    fcntl(p[1], F_SETFD, FD_CLOEXEC); /* only the read end goes to mpv */
    /* saved video (from the latest SPS) first, so the picture comes right away */
    SDL_LockMutex(mu);
    size_t n = M.nkeep;
    unsigned char *keep = M.keep;
    M.keep = NULL;
    M.nkeep = 0;
    M.airplay_start = false;
    SDL_UnlockMutex(mu);
    fcntl(p[1], F_SETPIPE_SZ, 1 << 20);
    if (n && write(p[1], keep, n) < 0) n = 0;
    free(keep);
    SDL_LockMutex(mu);
    M.sink = p[1];
    SDL_UnlockMutex(mu);
    char url[32];
    snprintf(url, sizeof url, "fd://%d", p[0]);
    const char *extra[12];
    int k = 0;
    for (; LIVE_ARGS[k]; k++) extra[k] = LIVE_ARGS[k];
    extra[k++] = "--demuxer-lavf-format=h264";
    extra[k++] = "--no-audio"; /* UxPlay plays the audio itself */
    extra[k] = NULL;
    PlayRequest rq = {0};
    rq.url = url;
    rq.title = T("Screen mirroring");
    rq.subtitle = "AirPlay";
    rq.is_live = false;
    rq.extra = extra;
    rq.tick = airplay_tick;
    stop_airplay = false;
    since_disconnect = 0;
    SDL_Log("mirror: AirPlay-spejling starter (%zu byte gemt)", n);
    player_run(&rq);
    SDL_LockMutex(mu);
    M.sink = -1;
    M.airplay_start = false;
    M.nkeep = 0;
    SDL_UnlockMutex(mu);
    close(p[0]);
    close(p[1]);
    /* the user closed with Back while the phone is still mirroring: wait for new video before starting again */
}

static void play_android(int fd) {
    char url[32];
    snprintf(url, sizeof url, "fd://%d", fd);
    PlayRequest rq = {0};
    rq.url = url;
    rq.title = T("Screen mirroring");
    rq.subtitle = "Android";
    rq.extra = LIVE_ARGS;
    SDL_Log("mirror: Android-spejling starter");
    player_run(&rq);
    close(fd);
}

void mirror_apply(void) {
    bool want = cfg_int("airplay_mirror", 1);
    if (!want || strcmp(ux_name, cfg_get("device_name", "NotaTV"))) ux_stop(); /* turned off or a new name */
    ux_next_try = 0;
}

void mirror_tick(void) {
    if (!started) {
        started = true;
        mu = SDL_CreateMutex();
        signal(SIGPIPE, SIG_IGN);
        SDL_Thread *t;
        if ((t = SDL_CreateThread(video_thread, "mirror-video", NULL))) SDL_DetachThread(t);
        /* the Android receiver (port 7400) only when turned on: there is no app yet */
        if (cfg_int("android_mirror", 0) && (t = SDL_CreateThread(android_thread, "mirror-android", NULL))) SDL_DetachThread(t);
    }
    /* UxPlay runs all the time (announces itself on the network); restart it if it dies */
    if (ux_pid > 0 && waitpid(ux_pid, NULL, WNOHANG) == ux_pid) { SDL_Log("mirror: UxPlay stoppede"); ux_pid = -1; }
    if (ux_pid < 0 && cfg_int("airplay_mirror", 1) && app_time() > ux_next_try) {
        ux_next_try = app_time() + 15;
        ux_start();
    }
    SDL_LockMutex(mu);
    bool ap = M.airplay_start && app_time() - M.last_data < 2;
    int afd = M.android_fd;
    M.android_fd = -1;
    SDL_UnlockMutex(mu);
    if (afd >= 0) play_android(afd);
    else if (ap && airplay_connected()) play_airplay();
}
