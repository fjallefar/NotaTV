/* NotaTV shell - player: mpv as a child process, controlled via JSON IPC */
#define _GNU_SOURCE /* execvpe */
#include "tv.h"
#include "vendor/cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SOCK_PATH "/tmp/notatv-mpv.sock"

static int ipc_fd = -1;

/* the whole line, also when the (non-blocking) socket only takes part of it; waits at most ~0.5 s */
static void ipc_send(const char *json) {
    if (ipc_fd < 0) return;
    size_t n = strlen(json);
    char *line = malloc(n + 1);
    if (!line) return;
    memcpy(line, json, n);
    line[n++] = '\n';
    size_t off = 0;
    for (int tries = 0; off < n && tries < 50;) {
        ssize_t w = write(ipc_fd, line + off, n - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
        struct pollfd p = {ipc_fd, POLLOUT, 0};
        poll(&p, 1, 10);
        tries++;
    }
    if (off < n) SDL_Log("ipc: write fejlede");
    free(line);
}

static void ipc_cmd(const char *const *args) {
    cJSON *o = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(o, "command");
    for (int i = 0; args[i]; i++) cJSON_AddItemToArray(a, cJSON_CreateString(args[i]));
    char *s = cJSON_PrintUnformatted(o);
    ipc_send(s);
    free(s);
    cJSON_Delete(o);
}
#define CMD(...) ipc_cmd((const char *const[]){__VA_ARGS__, NULL})

void player_message(const char *msg, const char *arg) { CMD("script-message", msg, arg ? arg : ""); }

/* remote control during playback (DLNA etc., from rq->tick on the main thread) */
static const void *cur_state; /* MpvState for the current playback, otherwise NULL */
void player_cmd(const char *const *args) { if (cur_state && ipc_fd >= 0) ipc_cmd(args); }

void player_osd(const char *text, int ms) {
    char d[16];
    snprintf(d, sizeof d, "%d", ms);
    CMD("script-message", "notatv-toast", text, d); /* the same pill as the menus' messages (notatv-osd.lua) */
}

/* frame rate: find the connected display's preferred resolution */
static bool preferred_res(char *out, size_t n) {
    for (int card = 0; card < 4; card++) {
        char path[256];
        {
            const char *types[] = {"HDMI-A-1", "HDMI-A-2", "DP-1", "DP-2", "DP-3", "Virtual-1", "eDP-1"};
            for (size_t t = 0; t < sizeof types / sizeof types[0]; t++) {
                snprintf(path, sizeof path, "/sys/class/drm/card%d-%s/status", card, types[t]);
                FILE *f = fopen(path, "r");
                if (!f) continue;
                char st[32] = "";
                if (!fgets(st, sizeof st, f)) st[0] = 0;
                fclose(f);
                if (strncmp(st, "connected", 9) != 0) continue;
                snprintf(path, sizeof path, "/sys/class/drm/card%d-%s/modes", card, types[t]);
                f = fopen(path, "r");
                if (!f) continue;
                char m[64] = "";
                if (fgets(m, sizeof m, f)) {
                    str_trim(m);
                    snprintf(out, n, "%s", m);
                    fclose(f);
                    return out[0] != 0;
                }
                fclose(f);
            }
        }
    }
    return false;
}

static double g_probe_fps; /* result of probe_video_wait (HDR: PQ/HLG; YouTube/IPTV are not probed = SDR) */
static bool g_probe_hdr;
static int g_probe_w; /* video width (0 = unknown) */

/* frame rate and HDR in one ffprobe call (before: two calls in a row over the network, up to 12 s
 * freeze before playback). The last result is kept, since both ask about the same URL. */
static bool probe_skip(const char *url) {
    return !url || strstr(url, "youtube.com") || strstr(url, "youtu.be") || strstr(url, "googlevideo.com") ||
           strstr(url, "ytdl://") || strstr(url, "twitch") || strstr(url, ".m3u") || strstr(url, "/live/") ||
           !strncmp(url, "fd://", 5);
}

/* the ffprobe call itself (thread safe: no shared state) */
static void probe_raw(const char *url, double *fps_out, bool *hdr_out, int *w_out) {
    double fps = 0;
    bool hdr = false;
    int w = 0;
    if (!probe_skip(url)) {
        const char *argv[] = {"timeout", "6", "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                              "stream=width,avg_frame_rate,r_frame_rate,color_transfer", "-of", "default=nw=1", "--", url, NULL};
        char *out = run_capture_argv(argv);
        int a = 0, b = 1;
        double avg = 0, r = 0;
        char *p;
        /* "r_frame_rate=24000/1001", "avg_frame_rate=24000/1001", "color_transfer=smpte2084" */
        if ((p = strstr(out, "avg_frame_rate=")) && sscanf(p + 15, "%d/%d", &a, &b) == 2 && b) avg = (double)a / b;
        if ((p = strstr(out, "r_frame_rate=")) && sscanf(p + 13, "%d/%d", &a, &b) == 2 && b) r = (double)a / b;
        fps = avg >= 1 && avg <= 200 ? avg : r;
        if (fps <= 1 || fps >= 200) fps = 0;
        hdr = strstr(out, "smpte2084") || strstr(out, "arib-std-b67");
        if ((p = strstr(out, "width="))) w = atoi(p + 6);
        free(out);
    }
    *fps_out = fps;
    *hdr_out = hdr;
    *w_out = w;
}

/* ffprobe in a background thread, while the UI shows "Loading…" and Back can cancel (before: frozen screen for up to 6 s) */
typedef struct { char *url; double fps; bool hdr; int w; SDL_AtomicInt done, refs; } ProbeJob;
static void probe_unref(ProbeJob *j) {
    if (SDL_AddAtomicInt(&j->refs, -1) == 1) { free(j->url); free(j); }
}
static int probe_thread(void *arg) {
    ProbeJob *j = arg;
    probe_raw(j->url, &j->fps, &j->hdr, &j->w);
    SDL_SetAtomicInt(&j->done, 1);
    probe_unref(j);
    return 0;
}

/* false = the user cancelled */
static bool probe_video_wait(const char *url) {
    static char *last_url;
    static double last_fps;
    static bool last_hdr;
    static int last_w;
    if (last_url && url && !strcmp(last_url, url)) { g_probe_fps = last_fps; g_probe_hdr = last_hdr; g_probe_w = last_w; return true; }
    double fps = 0;
    bool hdr = false;
    int w = 0;
    if (!probe_skip(url)) {
        ProbeJob *j = calloc(1, sizeof *j);
        j->url = str_dup(url);
        SDL_SetAtomicInt(&j->refs, 2);
        SDL_Thread *t = SDL_CreateThread(probe_thread, "probe", j);
        if (!t) { probe_unref(j); probe_unref(j); return true; }
        SDL_DetachThread(t);
        bool cancelled = false;
        double t0 = app_time();
        while (!SDL_GetAtomicInt(&j->done) && !cancelled)
            cancelled = app_wait_frame(app_time() - t0 > 0.3 ? T("Loading…") : NULL); /* short probe: no spinner flash */
        fps = j->fps;
        hdr = j->hdr;
        w = j->w;
        probe_unref(j);
        if (cancelled) return false;
    }
    free(last_url);
    /* only a result with a frame rate is kept: a failed probe (e.g. the network was down) is tried again next time */
    last_url = url && fps > 0 ? str_dup(url) : NULL;
    last_fps = fps;
    last_hdr = hdr;
    last_w = w;
    g_probe_fps = fps;
    g_probe_hdr = hdr;
    g_probe_w = w;
    return true;
}

/* "always HDR" on the Pi: SDR video is drawn with the GPU and converted to HDR (PQ/BT.2020), so the TV does not
   switch to SDR; HDR video goes unchanged directly onto the display plane as always */
static bool g_sdr_to_hdr;
/* Pi, the TV gets no HDR signal (display without HDR, or HDR turned off): HDR video is drawn with the GPU and
   tone-mapped to SDR, because the direct display plane would show it unconverted (washed out) */
static bool g_hdr_to_sdr;

static double display_rate_for(double fps) {
    if (fps <= 0) return 0;
    if (fabs(fps - 23.976) < 0.02) return 23.976;
    if (fabs(fps - 24) < 0.02) return 24;
    if (fabs(fps - 25) < 0.05) return 50;
    if (fabs(fps - 29.97) < 0.03) return 59.94;
    if (fabs(fps - 30) < 0.05) return 60;
    if (fabs(fps - 50) < 0.05) return 50;
    if (fabs(fps - 59.94) < 0.03) return 59.94;
    if (fabs(fps - 60) < 0.05) return 60;
    return 0;
}

static pid_t spawn_mpv(char **argv) {
    /* static HDR light data for the TV (read by our mpv patch on the Pi; ignored otherwise).
       Read before fork: cfg uses a mutex that another thread may hold. */
    bool apple_hdr = strcmp(cfg_get("hdr_meta", "apple"), "film") != 0;
    bool keep_hdr = hdr_cfg("hdr_always", 0);
    /* mpv's environment is built here, before fork: setenv in the child of a program with many threads is not safe */
    extern char **environ;
    size_t ne = 0;
    while (environ[ne]) ne++;
    char **env = malloc(sizeof(char *) * (ne + 3));
    if (!env) return -1;
    size_t k = 0;
    for (size_t i = 0; i < ne; i++)
        if (strncmp(environ[i], "NOTATV_HDR_STATIC=", 18) && strncmp(environ[i], "NOTATV_KEEP_HDR=", 16)) env[k++] = environ[i];
    if (apple_hdr) env[k++] = "NOTATV_HDR_STATIC=1000,0.005,4000,1000";
    /* "always HDR": mpv must not set the TV back to SDR when it closes */
    if (keep_hdr) env[k++] = "NOTATV_KEEP_HDR=1";
    env[k] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0);
        int lf = open("/tmp/mpv-out.log", O_WRONLY | O_CREAT | O_TRUNC, 0600); /* has URLs with tokens */
        if (lf >= 0) { fchmod(lf, 0600); dup2(lf, 1); dup2(lf, 2); }
        setsid();
        execvpe("mpv", argv, env);
        _exit(127);
    }
    free(env);
    return pid;
}

static int ipc_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

typedef struct {
    double pos, dur;
    bool paused, eof, have_dur;
    int plpos;
    bool menu;             /* the settings menu in mpv is open */
    char quality[32];      /* anmodet kvalitet */
    bool subsearch, goto_channel;
    int subpick;           /* -1 = none */
    int zap;               /* live: channel being browsed to (-1 = none) */
    double zap_t;
    bool zap_dim;          /* the picture is fading out ahead of the switch */
    double scrub;          /* seeking: target time in seconds (-1 = none) */
    double scrub_t;
    int scrub_n, scrub_dir;
    PlaySkip skip;         /* markers and "next episode" (rq->skip_info) */
    bool skip_ready;       /* skip is filled in (the thread is done) */
    bool skip_probed;
    int prompt;            /* button on screen: 0 none, 1 skip intro, 2 skip credits, 3 next episode */
    double prompt_to;      /* where the button leads: seconds, or -1 = the end (finish) */
} MpvState;

bool player_state(double *pos, double *dur, bool *paused) {
    const MpvState *st = cur_state;
    if (!st) return false;
    if (pos) *pos = st->pos;
    if (dur) *dur = st->dur;
    if (paused) *paused = st->paused;
    return true;
}

/* seeking: presses are gathered into one jump. Quick presses in the same direction accelerate, and the target is shown on
   the timeline right away, but mpv only jumps when you stop (LG's CEC sends no "hold") */
static void scrub(MpvState *st, int dir, bool big) {
    /* smooth ramp-up: every ~0.25 s (LG hold) the jump gets a little bigger, without big leaps in speed
       (25 between 20 and 30: every speed comes about one step later, so it builds up a little slower) */
    static const double steps[] = {10, 10, 15, 20, 25, 30, 40, 60, 80, 120, 180, 300, 600};
    enum { N_STEPS = sizeof steps / sizeof steps[0] };
    double now = app_time();
    bool cont = st->scrub >= 0 && now - st->scrub_t < 1.5 && st->scrub_dir == dir;
    if (st->scrub < 0) st->scrub = st->pos;
    st->scrub_n = cont ? st->scrub_n + 1 : 0;
    st->scrub_dir = dir;
    int i = st->scrub_n + (big ? 7 : 0); /* FF/REW starts at 60 s as before */
    double step = steps[i < N_STEPS ? i : N_STEPS - 1];
    st->scrub += dir * step;
    if (st->scrub < 0) st->scrub = 0;
    if (st->have_dur && st->scrub > st->dur - 5) st->scrub = st->dur > 5 ? st->dur - 5 : 0;
    st->scrub_t = now;
    char a[32], b[16];
    snprintf(a, sizeof a, "%.1f", st->scrub);
    snprintf(b, sizeof b, "%.0f", dir * step);
    CMD("script-message", "notatv-scrub", a, b);
}

/* skip intro / skip credits / next episode: which button belongs at the current position */
static int skip_kind(const MpvState *st, double *to) {
    const PlaySkip *k = &st->skip;
    *to = -1;
    if (!st->have_dur || st->dur <= 0) return 0;
    bool credits_seen = false;
    for (int i = 0; i < k->n; i++) {
        const PlayMarker *m = &k->m[i];
        credits_seen |= m->credits;
        if (st->pos < m->start || st->pos >= m->end - 1.0) continue;
        if (!m->credits) { *to = m->end; return 1; }
        if (m->end >= st->dur - 30) return k->has_next ? 3 : 2; /* the final credits: to the end */
        *to = m->end;
        return 2;
    }
    if (k->has_next && !credits_seen && st->dur > 300 && st->pos >= st->dur - 25) return 3;
    return 0;
}

static void skip_update(MpvState *st) {
    double to = -1;
    int kind = st->menu ? 0 : skip_kind(st, &to);
    st->prompt_to = to;
    if (kind == st->prompt) return;
    st->prompt = kind;
    if (!kind) { CMD("script-message", "notatv-prompt", "", ""); return; }
    CMD("script-message", "notatv-prompt", kind == 1 ? T("Skip intro") : kind == 2 ? T("Skip credits") : T("Next episode"),
        kind == 3 ? st->skip.next_title : "");
}

/* OK on the button */
static void skip_press(MpvState *st, bool *quit) {
    if (st->prompt_to >= 0) {
        char a[32];
        snprintf(a, sizeof a, "%.1f", st->prompt_to);
        CMD("seek", a, "absolute");
    } else { /* finished: the source continues with the next episode (if any) */
        st->eof = true;
        *quit = true;
    }
    st->prompt = 0;
    CMD("script-message", "notatv-prompt", "", "");
}

/* live channel change: quick presses are gathered, and mpv only switches when you stop - otherwise
   mpv opens every stream in between and freezes */
static void zap(const PlayRequest *rq, MpvState *st, int d) {
    if (rq->playlist_n <= 0) { CMD(d > 0 ? "playlist-next" : "playlist-prev"); return; }
    int n = rq->playlist_n, base = st->zap >= 0 ? st->zap : st->plpos;
    st->zap = ((base + d) % n + n) % n;
    st->zap_t = app_time();
    if (st->zap_dim) { CMD("script-message", "notatv-zapcancel"); st->zap_dim = false; } /* still browsing */
    if (rq->channel_preview) rq->channel_preview(rq->ud, st->zap);
}

static char ipc_buf[65536];
static size_t ipc_blen; /* reset with every new connection (no half line from the previous mpv) */
static bool ipc_skip;   /* a line longer than the buffer: dropped up to its end */
static void ipc_read(MpvState *st, const PlayRequest *rq) {
    char *buf = ipc_buf;
    for (;;) {
        if (ipc_blen >= sizeof ipc_buf - 1) { ipc_blen = 0; ipc_skip = true; } /* full without a line end */
        ssize_t n = read(ipc_fd, buf + ipc_blen, sizeof ipc_buf - ipc_blen - 1);
        if (n <= 0) break;
        ipc_blen += n;
        buf[ipc_blen] = 0;
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
            if (ipc_skip) { ipc_skip = false; line = nl + 1; continue; } /* the end of a too long line */
            cJSON *j = cJSON_Parse(line);
            if (j) {
                cJSON *ev = cJSON_GetObjectItem(j, "event");
                cJSON *name = cJSON_GetObjectItem(j, "name");
                cJSON *data = cJSON_GetObjectItem(j, "data");
                if (cJSON_IsString(ev) && strcmp(ev->valuestring, "property-change") == 0 && cJSON_IsString(name)) {
                    const char *nm = name->valuestring;
                    if (!strcmp(nm, "time-pos") && cJSON_IsNumber(data)) st->pos = data->valuedouble;
                    else if (!strcmp(nm, "duration") && cJSON_IsNumber(data)) { st->dur = data->valuedouble; st->have_dur = true; }
                    else if (!strcmp(nm, "pause")) st->paused = cJSON_IsTrue(data);
                    else if (!strcmp(nm, "eof-reached")) st->eof = cJSON_IsTrue(data);
                    else if (!strcmp(nm, "playlist-pos") && cJSON_IsNumber(data)) {
                        int p = data->valueint;
                        if (p != st->plpos && p >= 0) {
                            st->plpos = p;
                            if (rq->channel_changed) rq->channel_changed(rq->ud, p);
                        }
                    }
                } else if (cJSON_IsString(ev) && !strcmp(ev->valuestring, "client-message")) {
                    /* messages from notatv-osd.lua */
                    cJSON *args = cJSON_GetObjectItem(j, "args");
                    cJSON *a0 = cJSON_GetArrayItem(args, 0), *a1 = cJSON_GetArrayItem(args, 1);
                    const char *m = cJSON_IsString(a0) ? a0->valuestring : "";
                    const char *v = cJSON_IsString(a1) ? a1->valuestring : "";
                    if (!strcmp(m, "notatv-menu")) st->menu = !strcmp(v, "1");
                    else if (!strcmp(m, "notatv-quality")) snprintf(st->quality, sizeof st->quality, "%s", v);
                    else if (!strcmp(m, "notatv-subsearch")) st->subsearch = true;
                    else if (!strcmp(m, "notatv-subpick")) st->subpick = atoi(v);
                    else if (!strcmp(m, "notatv-channel")) st->goto_channel = true;
                    else if (!strcmp(m, "notatv-scrub-stop") && st->scrub >= 0 && *v) {
                        /* the key was released: the OSD has braked the marker - jump there shortly */
                        st->scrub = atof(v);
                        st->scrub_t = app_time() - 0.55;
                    }
                } else if (cJSON_IsString(ev) && strcmp(ev->valuestring, "end-file") == 0) {
                    cJSON *reason = cJSON_GetObjectItem(j, "reason");
                    if (cJSON_IsString(reason) && strcmp(reason->valuestring, "eof") == 0) st->eof = true;
                }
                cJSON_Delete(j);
            }
            line = nl + 1;
        }
        ipc_blen = strlen(line);
        memmove(buf, line, ipc_blen + 1);
    }
}

static void add_arg(char ***argv, int *n, const char *s) {
    *argv = realloc(*argv, sizeof(char *) * (*n + 2));
    (*argv)[(*n)++] = str_dup(s);
    (*argv)[*n] = NULL;
}

static void build_args(const PlayRequest *rq, const char *mode, char ***out, int *n) {
    char **argv = NULL;
    *n = 0;
    add_arg(&argv, n, "mpv");
    add_arg(&argv, n, "--fs");
    add_arg(&argv, n, "--input-ipc-server=" SOCK_PATH);
    add_arg(&argv, n, "--input-terminal=no");
    add_arg(&argv, n, "--force-window=immediate");
    add_arg(&argv, n, "--keep-open=no");
    { char *s = str_printf("--log-file=%s", mpv_log_path()); add_arg(&argv, n, s); free(s); }
    if (rq->title) { char *s = str_printf("--force-media-title=%s", rq->title); add_arg(&argv, n, s); free(s); }
    if (rq->start > 1) { char *s = str_printf("--start=%.1f", rq->start); add_arg(&argv, n, s); free(s); }
    if (rq->playlist_start > 0) { char *s = str_printf("--playlist-start=%d", rq->playlist_start); add_arg(&argv, n, s); free(s); }
    if (mode && *mode) { char *s = str_printf("--drm-mode=%s", mode); add_arg(&argv, n, s); free(s); }

    /* audio */
    const char *adev = cfg_get("audio_device", "");
    if (*adev) { char *s = str_printf("--audio-device=%s", adev); add_arg(&argv, n, s); free(s); }
    if (audio_cfg("audio_passthrough", 0, DA_AC3)) {
        /* only the formats the TV/receiver lists - an unsupported one (e.g. E-AC-3 to an AC-3-only receiver) gives silence */
        char codecs[64] = "ac3";
        bool pt_hd = audio_cfg("pt_hd", 0, DA_HD);
        if (display_audio_ok(DA_EAC3)) strcat(codecs, ",eac3");
        if (audio_cfg("pt_dts", 1, DA_DTS)) strcat(codecs, ",dts");
        if (pt_hd && display_audio_ok(DA_DTSHD)) strcat(codecs, ",dts-hd");
        if (pt_hd && display_audio_ok(DA_TRUEHD)) strcat(codecs, ",truehd");
        char *s = str_printf("--audio-spdif=%s", codecs);
        add_arg(&argv, n, s);
        free(s);
        /* ARC (without HD passthrough): multichannel audio that cannot be sent unchanged (TrueHD, AAC/FLAC/PCM 5.1)
         * is re-encoded to Dolby Digital 5.1 - stereo stays PCM (minch=3) */
        if (!pt_hd) add_arg(&argv, n, "--af=lavcac3enc=tospdif=yes:bitrate=640:minch=3");
    }
    int delay = cfg_int("audio_delay_ms", 0);
    if (delay) { char *s = str_printf("--audio-delay=%.3f", delay / 1000.0); add_arg(&argv, n, s); free(s); }
    /* picture */
    if (g_sdr_to_hdr) {
        add_arg(&argv, n, "--gpu-hwdec-interop=drmprime");
        add_arg(&argv, n, "--target-trc=pq");
        add_arg(&argv, n, "--target-prim=bt.2020");
    }
    if (g_hdr_to_sdr) {
        add_arg(&argv, n, "--gpu-hwdec-interop=drmprime");
        add_arg(&argv, n, "--target-trc=srgb");
        add_arg(&argv, n, "--target-prim=bt.709");
    }
    if (hdr_cfg("hdr", 1)) add_arg(&argv, n, "--target-colorspace-hint=yes");
    else { /* the Pi's mpv.conf turns the hint on, so it must be turned off explicitly */
        add_arg(&argv, n, "--target-colorspace-hint=no");
        add_arg(&argv, n, "--tone-mapping=auto");
    }
    if (g_hdr_to_sdr) { /* the cheapest path: the Pi 4's GPU only just manages 4K (mobius/reinhard drop frames) */
        static const char *const cheap[] = {"--tone-mapping=clip", "--gamut-mapping-mode=clip", "--hdr-compute-peak=no",
            "--dither-depth=no", "--scale=bilinear", "--cscale=bilinear", "--dscale=bilinear", "--correct-downscaling=no",
            "--linear-downscaling=no", "--sigmoid-upscaling=no", "--deband=no"};
        for (size_t i = 0; i < SDL_arraysize(cheap); i++) add_arg(&argv, n, cheap[i]);
        /* 4K: draw in 900p and let the display plane scale up (39 ms -> ~32 ms per frame, room for subtitles) */
        if (g_probe_w == 0 || g_probe_w > 1920) add_arg(&argv, n, "--drm-draw-surface-size=1600x900");
        /* brightness: 100 nits become full white (HDR reference white is 203). HDR movies are mastered darker
           than SDR; this looks closest to an SDR version, at the cost of clipping the brightest highlights */
        add_arg(&argv, n, "--target-peak=100");
    }
    /* sprog */
    { char *s = str_printf("--slang=%s", cfg_get("sub_lang", lang_media_default("sub_lang"))); add_arg(&argv, n, s); free(s); }
    { char *s = str_printf("--alang=%s", cfg_get("audio_lang", lang_media_default("audio_lang"))); add_arg(&argv, n, s); free(s); }
    if (rq->is_live) {
        add_arg(&argv, n, "--cache=yes");
        add_arg(&argv, n, "--demuxer-max-back-bytes=64MiB");
        add_arg(&argv, n, "--deinterlace=auto");
    }
    /* yt-dlp only for what needs it (YouTube, Twitch: they pass --ytdl-... options or a ytdl:// address).
       Otherwise mpv's ytdl hook tries yt-dlp after a failed open - for a server that does not answer that is
       20-25 s more of "Loading…" before the error is shown */
    bool ytdl = rq->url && (strstr(rq->url, "ytdl://") || strstr(rq->url, "youtube.com") || strstr(rq->url, "youtu.be") || strstr(rq->url, "twitch.tv"));
    for (int i = 0; rq->extra && rq->extra[i] && !ytdl; i++) ytdl = !strncmp(rq->extra[i], "--ytdl", 6) && strcmp(rq->extra[i], "--ytdl=no");
    if (!ytdl) add_arg(&argv, n, "--ytdl=no");
    for (int i = 0; rq->extra && rq->extra[i]; i++) add_arg(&argv, n, rq->extra[i]);
    add_arg(&argv, n, "--");
    add_arg(&argv, n, rq->url);
    *out = argv;
}

static void free_args(char **argv) {
    for (int i = 0; argv && argv[i]; i++) free(argv[i]);
    free(argv);
}

static void handle_action(const InputEvent *ev, const PlayRequest *rq, MpvState *st, bool *quit,
                          char *digits, double *digit_t) {
    if (st->menu) { /* the menu in mpv is controlled with the arrow keys */
        const char *k = NULL;
        switch (ev->a) {
        case A_UP: k = "up"; break;
        case A_DOWN: k = "down"; break;
        case A_LEFT: k = "left"; break;
        case A_RIGHT: k = "right"; break;
        case A_OK: k = ev->repeat ? NULL : "ok"; break;
        case A_BACK: case A_MENU: k = "back"; break;
        case A_HOME: *quit = true; input_push(A_HOME, 0, false); return;
        default: break;
        }
        if (k) CMD("script-message", "notatv-key", k);
        return;
    }
    switch (ev->a) {
    case A_OK: case A_PLAYPAUSE:
        if (ev->repeat) break; /* held OK: no pause/play over and over */
        if (ev->a == A_OK && st->prompt) { skip_press(st, quit); break; }
        CMD("cycle", "pause");
        CMD("script-message", "notatv-show");
        break;
    case A_PLAY: CMD("set", "pause", "no"); CMD("script-message", "notatv-show"); break;
    case A_PAUSE: CMD("set", "pause", "yes"); CMD("script-message", "notatv-show"); break;
    case A_BACK: case A_STOP: *quit = true; break;
    case A_HOME: *quit = true; input_push(A_HOME, 0, false); break;
    case A_LEFT:
        if (!rq->is_live) scrub(st, -1, false);
        else CMD("script-message", "notatv-show");
        break;
    case A_RIGHT:
        if (!rq->is_live) scrub(st, 1, false);
        else CMD("script-message", "notatv-show");
        break;
    case A_FF: if (!rq->is_live) scrub(st, 1, true); break;
    case A_REW: if (!rq->is_live) scrub(st, -1, true); break;
    case A_UP:
        if (rq->is_live) zap(rq, st, -1);
        else CMD("script-message", "notatv-show");
        break;
    case A_DOWN:
        if (rq->is_live) zap(rq, st, 1);
        else CMD("script-message", "notatv-menu-open");
        break;
    case A_CHUP:
        if (rq->is_live) zap(rq, st, -1);
        else CMD("add", "chapter", "1");
        break;
    case A_CHDOWN:
        if (rq->is_live) zap(rq, st, 1);
        else CMD("add", "chapter", "-1");
        break;
    case A_NEXT:
        if (!rq->is_live && st->skip.has_next) { st->eof = true; *quit = true; }
        else CMD("playlist-next");
        break;
    case A_PREV: CMD("playlist-prev"); break;
    case A_INFO: CMD("script-message", "notatv-show", "8"); break;
    case A_SUBTITLE:
        CMD("cycle", "sub");
        CMD("script-message", "notatv-track-osd", "sub"); /* names as in the menu (notatv-osd.lua) */
        break;
    case A_MENU: CMD("script-message", "notatv-menu-open"); break;
    case A_AUDIO:
        CMD("cycle", "audio");
        CMD("script-message", "notatv-track-osd", "audio");
        break;
    case A_YELLOW: /* Yellow = action on what is shown: Twitch chat on/off (remembered). Aspect ratio and technical info are in the menu (Red) */
        if (rq->chat) {
            int on = !cfg_int("tw_chat", 1);
            cfg_set_int("tw_chat", on);
            CMD("script-message", "notatv-chat-show", on ? "1" : "0");
            CMD("script-message", "notatv-toast", on ? T("Chat: on") : T("Chat: off"), "1500");
        }
        break;
    case A_VOLUP: CMD("add", "volume", "5"); CMD("script-message", "notatv-toast", T("Volume: ${volume}%"), "1500"); break;
    case A_VOLDOWN: CMD("add", "volume", "-5"); CMD("script-message", "notatv-toast", T("Volume: ${volume}%"), "1500"); break;
    case A_MUTE: CMD("cycle", "mute"); break;
    case A_DIGIT: case A_CHAR:
        if (!rq->is_live && ev->ch >= '0' && ev->ch <= '9' && st->have_dur) { /* 1-9: 10-90 %, 0: start */
            st->scrub = (ev->ch - '0') * 0.1 * st->dur;
            st->scrub_t = app_time() - 0.3; /* jump almost right away */
            st->scrub_n = 0;
            char a[32];
            snprintf(a, sizeof a, "%.1f", st->scrub);
            CMD("script-message", "notatv-scrub", a, "0");
            break;
        }
        if (rq->is_live && ev->ch >= '0' && ev->ch <= '9') {
            size_t l = strlen(digits);
            if (l < 4) { digits[l] = (char)ev->ch; digits[l + 1] = 0; }
            *digit_t = app_time();
            char t[32];
            snprintf(t, sizeof t, T("Channel %s_"), digits);
            player_osd(t, 1600);
        }
        break;
    default: break;
    }
    (void)st;
}

/* /tmp/notatv-nowplaying.json for the OSD script (written whole, so it never reads half a file) */
static void write_nowplaying(const char *js) {
    FILE *f = fopen("/tmp/notatv-nowplaying.json.tmp", "w");
    if (!f) return;
    fputs(js, f);
    if (fclose(f) == 0) rename("/tmp/notatv-nowplaying.json.tmp", "/tmp/notatv-nowplaying.json");
}
static SDL_AtomicInt np_gen; /* counted up per playback: a late poster never lands in the next one's file */
typedef struct { char *url, *json; int gen; } PosterJob;
static int poster_thread(void *arg) {
    PosterJob *pj = arg;
    int pw, ph;
    /* two versions: the OSD script picks the HDR version when the TV is in HDR (PQ) */
    if (img_export_bgra(pj->url, "/tmp/notatv-poster.bgra", 420, 630, &pw, &ph)) {
        cJSON *m = cJSON_Parse(pj->json);
        if (m) {
            if (img_export_bgra_ex(pj->url, "/tmp/notatv-poster-hdr.bgra", 420, 630, &pw, &ph, true))
                cJSON_AddStringToObject(m, "poster_hdr", "/tmp/notatv-poster-hdr.bgra");
            cJSON_AddStringToObject(m, "poster", "/tmp/notatv-poster.bgra");
            cJSON_AddNumberToObject(m, "poster_w", pw);
            cJSON_AddNumberToObject(m, "poster_h", ph);
            char *js = cJSON_PrintUnformatted(m);
            if (js && SDL_GetAtomicInt(&np_gen) == pj->gen) write_nowplaying(js);
            free(js);
            cJSON_Delete(m);
        }
    }
    free(pj->url);
    free(pj->json);
    free(pj);
    return 0;
}

/* rq->skip_info (markers, next episode) in a thread; the result is copied in the playback loop */
static struct {
    bool running;
    SDL_Thread *t;
    SDL_AtomicInt done;
    const PlayRequest *rq;
    PlaySkip out;
} skipjob;
static int skip_thread(void *arg) {
    (void)arg;
    skipjob.rq->skip_info(skipjob.rq->ud, &skipjob.out);
    SDL_SetAtomicInt(&skipjob.done, 1);
    return 0;
}
static void skip_start(const PlayRequest *rq) {
    memset(&skipjob.out, 0, sizeof skipjob.out);
    skipjob.rq = rq;
    SDL_SetAtomicInt(&skipjob.done, 0);
    skipjob.t = SDL_CreateThread(skip_thread, "skipinfo", NULL);
    skipjob.running = skipjob.t != NULL;
}

/* OpenSubtitles search/download for the playback (one thread at a time) */
static struct {
    bool running, fetch;
    SDL_Thread *t;
    SDL_AtomicInt done;
    const PlayRequest *rq;
    int idx, n;
    char **labels, *path;
} subjob;
static int sub_thread(void *arg) {
    (void)arg;
    if (subjob.fetch) subjob.path = subjob.rq->sub_fetch(subjob.rq->ud, subjob.idx);
    else subjob.n = subjob.rq->sub_search(subjob.rq->ud, &subjob.labels);
    SDL_SetAtomicInt(&subjob.done, 1);
    return 0;
}
static void sub_start(const PlayRequest *rq, bool fetch, int idx) {
    subjob.rq = rq;
    subjob.fetch = fetch;
    subjob.idx = idx;
    subjob.n = 0;
    subjob.labels = NULL;
    subjob.path = NULL;
    SDL_SetAtomicInt(&subjob.done, 0);
    subjob.t = SDL_CreateThread(sub_thread, "subs", NULL);
    subjob.running = subjob.t != NULL;
    if (!subjob.running) player_osd(T("Could not download subtitles"), 3000);
}

static PlayResult player_run_inner(const PlayRequest *rq);

/* AirPlay/Bluetooth music pauses while a video plays. In the background (shell + script + D-Bus took time off
   the start of every video and the way back to the menu). */
/* background audio (e.g. Radio): the Pi's HDMI audio device takes one player at a time, so the app that plays while
   the UI runs is stopped before video or game streaming starts. Its stop function must have released the device
   when it returns. */
static void (*bg_stop)(void);
void audio_bg_set(void (*stop)(void)) { bg_stop = stop; }
void audio_bg_take(void) {
    void (*f)(void) = bg_stop;
    bg_stop = NULL;
    if (f) { SDL_Log("player: stopper baggrundslyd"); f(); }
}

PlayResult player_run(const PlayRequest *rq) {
    static bool active;
    if (active) { /* e.g. a background job that wants to play while something plays: never two players at once */
        SDL_Log("player: spiller allerede - %s ignoreret", rq && rq->title ? rq->title : "afspilning");
        PlayResult r = {0};
        r.cancelled = true;
        return r;
    }
    active = true;
    audio_bg_take();
    run_cmd("%s/music pause >/dev/null 2>&1 &", helper_path());
    PlayResult res = player_run_inner(rq);
    run_cmd("%s/music resume >/dev/null 2>&1 &", helper_path());
    active = false;
    return res;
}

static PlayResult player_run_inner(const PlayRequest *rq) {
    PlayResult res = {0};
    unlink(SOCK_PATH);
    { /* the previous playback's log is kept as .1 (e.g. the one that failed, before you tried again) */
        char *old = str_printf("%s.1", mpv_log_path());
        rename(mpv_log_path(), old);
        free(old);
    }
    bt_media("Pause"); /* Bluetooth: pause the phone, so the music does not keep playing into nothing */

    /* frame rate and HDR are only checked if they are used */
    bool want_rate = cfg_int("match_rate", 1) && input_evdev_active() && !rq->is_live;
    bool want_hdr = hdr_cfg("hdr_always", 0);
    g_probe_fps = 0;
    g_probe_hdr = false;
    g_probe_w = 0;
    /* the TV gets no HDR signal (display without HDR, or HDR turned off), so HDR video must be converted by the
       GPU - the direct display plane would show it unconverted (washed out). Live TV is SDR. */
    bool sdr_out = !rq->is_live && !hdr_cfg("hdr", 1);
    if (!want_rate && rq->media_hdr) { /* the source already knows (e.g. Plex): no ffprobe needed */
        g_probe_hdr = rq->media_hdr == 2;
        g_probe_w = rq->media_w;
    } else if ((want_rate || want_hdr || sdr_out) && !probe_video_wait(rq->url)) {
        res.cancelled = true;
        return res;
    }

    /* frame rate match: only a mode the display really has (display_mode_for reads its mode list), so mpv
       does not have to fail and start again. The empty mode last is the safety net if mpv still fails. */
    char modes[2][48] = {"", ""};
    int n_modes = 0;
    if (cfg_int("match_rate", 1) && input_evdev_active()) {
        char res_s[32];
        int w = 0, h = 0;
        if (preferred_res(res_s, sizeof res_s) && sscanf(res_s, "%dx%d", &w, &h) == 2) {
            double rate = rq->is_live ? cfg_int("iptv_rate", 50) : display_rate_for(g_probe_fps);
            if (rate > 0 && display_mode_for(w, h, rate, modes[n_modes], sizeof modes[n_modes])) n_modes++;
        }
    }
    snprintf(modes[n_modes++], 48, "%s", "");
    g_sdr_to_hdr = want_hdr && !g_probe_hdr;
    g_hdr_to_sdr = sdr_out && g_probe_hdr;
    if (g_hdr_to_sdr) SDL_Log("player: HDR-video tone-mappes til SDR (skærmen får ikke HDR)");
    if (g_sdr_to_hdr) SDL_Log("player: SDR-video omregnes til HDR");

    /* metadata for media controls/pause screen (notatv-osd.lua) */
    {
        cJSON *m = cJSON_CreateObject();
        if (rq->title) cJSON_AddStringToObject(m, "title", rq->title);
        if (rq->subtitle) cJSON_AddStringToObject(m, "subtitle", rq->subtitle);
        if (rq->description) cJSON_AddStringToObject(m, "description", rq->description);
        cJSON_AddBoolToObject(m, "live", rq->is_live);
        cJSON_AddBoolToObject(m, "sdr_to_hdr", g_sdr_to_hdr); /* video is converted -> the TV in HDR */
        if (rq->qualities) {
            cJSON *qa = cJSON_AddArrayToObject(m, "qualities");
            for (int i = 0; rq->qualities[i]; i++) cJSON_AddItemToArray(qa, cJSON_CreateString(rq->qualities[i]));
            cJSON_AddStringToObject(m, "quality", rq->quality ? rq->quality : rq->qualities[0]);
        }
        if (rq->speed) cJSON_AddBoolToObject(m, "speed", true);
        if (rq->channel) cJSON_AddStringToObject(m, "channel", rq->channel);
        if (rq->sub_search) cJSON_AddBoolToObject(m, "subsearch", true);
        /* without a key the menu explains where to add it instead of searching */
        if (rq->sub_search && !*cfg_get("os_apikey", "")) cJSON_AddBoolToObject(m, "subnokey", true);
        char *js = cJSON_PrintUnformatted(m);
        write_nowplaying(js);
        /* the poster (for the pause screen) is made in a thread while mpv starts - it may have to be fetched */
        int gen = SDL_AddAtomicInt(&np_gen, 1) + 1;
        if (rq->poster) {
            PosterJob *pj = calloc(1, sizeof *pj);
            if (pj) {
                pj->url = str_dup(rq->poster);
                pj->json = js;
                js = NULL;
                pj->gen = gen;
                SDL_Thread *t = SDL_CreateThread(poster_thread, "poster", pj);
                if (t) SDL_DetachThread(t);
                else { free(pj->url); free(pj->json); free(pj); }
            }
        }
        free(js);
        cJSON_Delete(m);
    }
    bool video_off = app_video_suspend();
    MpvState st = {.plpos = rq->playlist_start};
    cur_state = &st;
    int attempt = 0, status = 0;
    bool user_quit = false;
    for (; attempt < n_modes; attempt++) {
        memset(&st, 0, sizeof st);
        st.plpos = rq->playlist_start;
        st.subpick = -1;
        st.zap = -1;
        st.scrub = -1;
        status = 0;
        char **argv;
        int argc;
        build_args(rq, modes[attempt], &argv, &argc);
        { char *lu = log_url(rq->url); SDL_Log("player: mpv %s (mode '%s')", lu, modes[attempt]); free(lu); }
        unlink(SOCK_PATH);
        pid_t pid = spawn_mpv(argv);
        free_args(argv);
        double started = app_time();
        /* connect IPC (up to 8 s) */
        ipc_fd = -1;
        ipc_blen = 0;
        ipc_skip = false;
        while (app_time() - started < 8) {
            ipc_fd = ipc_connect();
            if (ipc_fd >= 0) break;
            if (waitpid(pid, &status, WNOHANG) == pid) { pid = -1; break; }
            usleep(50000);
        }
        if (pid > 0 && ipc_fd >= 0) {
            /* observe_property needs a numeric id: send raw JSON */
            ipc_send("{\"command\":[\"observe_property\",1,\"time-pos\"]}");
            ipc_send("{\"command\":[\"observe_property\",2,\"duration\"]}");
            ipc_send("{\"command\":[\"observe_property\",3,\"pause\"]}");
            ipc_send("{\"command\":[\"observe_property\",4,\"eof-reached\"]}");
            ipc_send("{\"command\":[\"observe_property\",5,\"playlist-pos\"]}");
        }
        bool quit = false;
        double last_prog = app_time();
        char digits[8] = "";
        double digit_t = 0;
        while (pid > 0) {
            struct pollfd pfd[40];
            int fds[40];
            int nf = input_fds(fds, 38);
            for (int i = 0; i < nf; i++) pfd[i] = (struct pollfd){fds[i], POLLIN, 0};
            if (ipc_fd >= 0) pfd[nf++] = (struct pollfd){ipc_fd, POLLIN, 0};
            poll(pfd, nf, st.zap >= 0 ? 15 : 100); /* channel change: on time for the fade */
            if (ipc_fd >= 0) ipc_read(&st, rq);
            /* markers and next episode: fetched in a thread (a slow server must not freeze the keys) */
            if (!rq->is_live && rq->skip_info && !skipjob.running && !st.skip_probed && st.have_dur && st.pos > 3) {
                st.skip_probed = true;
                skip_start(rq);
            }
            if (skipjob.running && SDL_GetAtomicInt(&skipjob.done)) {
                SDL_WaitThread(skipjob.t, NULL);
                skipjob.running = false;
                st.skip = skipjob.out;
                st.skip_ready = true;
            }
            if (st.skip_ready) skip_update(&st);
            input_poll();
            if (!input_evdev_active()) { /* PC development: SDL events (mpv has its own window) */
                SDL_Event e;
                while (SDL_PollEvent(&e)) input_handle_sdl(&e);
            }
            InputEvent ev;
            while (input_next(&ev)) handle_action(&ev, rq, &st, &quit, digits, &digit_t);
            if (rq->tick) rq->tick(rq->ud);
            app_background_tick(true); /* jobs, phone page, Bluetooth, App Store installs keep running */
            if (st.scrub >= 0 && app_time() - st.scrub_t > 0.7) {
                char a[32];
                snprintf(a, sizeof a, "%.1f", st.scrub);
                CMD("seek", a, "absolute+keyframes");
                CMD("script-message", "notatv-scrub", "", "");
                st.scrub = -1;
            }
            /* the fade-out starts while the old channel still plays, so the picture is black when mpv
               closes it (closing removes the picture at once) */
            if (st.zap >= 0 && !st.zap_dim && st.zap != st.plpos && app_time() - st.zap_t > 0.1) {
                CMD("script-message", "notatv-zapfade");
                st.zap_dim = true;
            }
            if (st.zap >= 0 && app_time() - st.zap_t > 0.25) {
                st.zap_dim = false;
                if (st.zap != st.plpos) {
                    char idx[16];
                    snprintf(idx, sizeof idx, "%d", st.zap);
                    CMD("script-message", "notatv-zap", idx); /* the OSD script fades out, then switches */
                }
                st.zap = -1;
            }
            if (digits[0] && app_time() - digit_t > 1.6) {
                int num = atoi(digits);
                digits[0] = 0;
                if (num > 0 && (rq->playlist_n <= 0 || num <= rq->playlist_n)) {
                    char idx[16];
                    snprintf(idx, sizeof idx, "%d", num - 1);
                    if (rq->is_live) CMD("script-message", "notatv-zap", idx);
                    else CMD("playlist-play-index", idx);
                }
            }
            if (quit) {
                user_quit = true;
                /* the picture fades to black before mpv closes (the UI fades in from black afterwards) */
                CMD("script-message", "notatv-fadeout");
                usleep(260000);
                CMD("quit");
                /* give mpv time, otherwise kill it */
                for (int i = 0; i < 40 && waitpid(pid, &status, WNOHANG) == 0; i++) usleep(50000);
                if (waitpid(pid, &status, WNOHANG) == 0) { kill(pid, SIGTERM); waitpid(pid, &status, 0); }
                pid = -1;
                break;
            }
            /* menuens anmodninger */
            if (st.quality[0] || st.goto_channel) {
                snprintf(res.quality, sizeof res.quality, "%s", st.quality);
                res.goto_channel = st.goto_channel;
                quit = true;
                continue;
            }
            /* OpenSubtitles: search and download in a thread (up to 15 s) - the keys and mpv's menu keep working.
               One at a time; the thread's result is taken here when it is done. */
            if (!subjob.running && st.subsearch && rq->sub_search) {
                st.subsearch = false;
                sub_start(rq, false, 0);
            } else if (!subjob.running && st.subpick >= 0 && rq->sub_fetch) {
                int idx = st.subpick;
                st.subpick = -1;
                sub_start(rq, true, idx);
            }
            if (subjob.running && SDL_GetAtomicInt(&subjob.done)) {
                SDL_WaitThread(subjob.t, NULL);
                subjob.running = false;
                if (!subjob.fetch) {
                    cJSON *arr = cJSON_CreateArray();
                    for (int i = 0; i < subjob.n; i++) { cJSON_AddItemToArray(arr, cJSON_CreateString(subjob.labels[i])); free(subjob.labels[i]); }
                    free(subjob.labels);
                    char *js = cJSON_PrintUnformatted(arr);
                    cJSON_Delete(arr);
                    FILE *f = fopen("/tmp/notatv-subs.json", "w");
                    if (f) { fputs(js, f); fclose(f); }
                    free(js);
                    CMD("script-message", "notatv-subresults");
                } else if (subjob.path) { CMD("sub-add", subjob.path, "select"); player_osd(T("Subtitles loaded"), 2000); free(subjob.path); }
                else player_osd(T("Could not download subtitles"), 3000);
                subjob.labels = NULL;
                subjob.path = NULL;
            }
            if (rq->progress && app_time() - last_prog > 10 && st.have_dur) {
                rq->progress(rq->ud, st.pos, st.dur, false, st.paused);
                last_prog = app_time();
            }
            if (waitpid(pid, &status, WNOHANG) == pid) pid = -1;
        }
        if (skipjob.running) { SDL_WaitThread(skipjob.t, NULL); skipjob.running = false; } /* rq->ud outlives it */
        if (subjob.running) { /* rq->ud must outlive the thread */
            SDL_WaitThread(subjob.t, NULL);
            subjob.running = false;
            for (int i = 0; i < subjob.n; i++) free(subjob.labels[i]);
            free(subjob.labels);
            free(subjob.path);
            subjob.labels = NULL;
            subjob.path = NULL;
        }
        if (ipc_fd >= 0) close(ipc_fd);
        ipc_fd = -1;
        res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        /* mpv failed before playback got going (e.g. the display mode does not exist): try the next one */
        if (res.quality[0] || res.goto_channel) break;
        bool failed_early = !user_quit && res.exit_code != 0 && !st.have_dur && app_time() - started < 20;
        if (failed_early && attempt < n_modes - 1) {
            SDL_Log("player: mpv fejlede (kode %d) med mode '%s' - prøver igen", res.exit_code, modes[attempt]);
            continue;
        }
        break;
    }
    cur_state = NULL;
    res.pos = st.pos;
    res.dur = st.dur;
    res.eof = st.eof || (st.dur > 0 && st.pos > st.dur - 3);
    if (rq->progress && st.have_dur) rq->progress(rq->ud, st.pos, st.dur, true, false);
    if (video_off) app_video_resume();
    if (!user_quit && !res.quality[0] && !res.goto_channel && res.exit_code != 0 && !st.have_dur) {
        /* mpv's log says why: a server that never answered is worth saying (it is not the box) */
        bool no_answer = false, http_error = false; /* an answer like 401/404 is not "did not answer" */
        FILE *lf = fopen(mpv_log_path(), "r");
        if (lf) {
            char line[1024];
            while (fgets(line, sizeof line, lf))
                if (strstr(line, "][e][stream] Failed to open http") || strstr(line, "Connection timed out") || strstr(line, "Connection refused"))
                    no_answer = true;
                else if (strstr(line, "HTTP error")) http_error = true;
            fclose(lf);
        }
        if (no_answer && !http_error) app_toast(T("The server did not send %s in time - check that it is running and the network"), rq->title ? rq->title : "videoen");
        else app_toast(T("Could not play %s"), rq->title ? rq->title : "videoen");
    }
    return res;
}
