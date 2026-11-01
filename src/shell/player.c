/* NotaTV shell - player: mpv as a child process, controlled via JSON IPC */
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

static void ipc_send(const char *json) {
    if (ipc_fd < 0) return;
    size_t n = strlen(json);
    if (write(ipc_fd, json, n) < 0 || write(ipc_fd, "\n", 1) < 0) SDL_Log("ipc: write fejlede");
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
    CMD("show-text", text, d);
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

/* frame rate and HDR in one ffprobe call (before: two calls in a row over the network, up to 12 s
 * freeze before playback). The last result is kept, since both ask about the same URL. */
static bool probe_skip(const char *url) {
    return !url || strstr(url, "youtube.com") || strstr(url, "youtu.be") || strstr(url, "googlevideo.com") ||
           strstr(url, "ytdl://") || strstr(url, "twitch") || strstr(url, ".m3u") || strstr(url, "/live/") ||
           !strncmp(url, "fd://", 5);
}

/* the ffprobe call itself (thread safe: no shared state) */
static void probe_raw(const char *url, double *fps_out, bool *hdr_out) {
    double fps = 0;
    bool hdr = false;
    if (!probe_skip(url)) {
        const char *argv[] = {"timeout", "6", "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                              "stream=avg_frame_rate,r_frame_rate,color_transfer", "-of", "default=nw=1", "--", url, NULL};
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
        free(out);
    }
    *fps_out = fps;
    *hdr_out = hdr;
}

/* ffprobe in a background thread, while the UI shows "Loading…" and Back can cancel (before: frozen screen for up to 6 s) */
typedef struct { char *url; double fps; bool hdr; SDL_AtomicInt done, refs; } ProbeJob;
static void probe_unref(ProbeJob *j) {
    if (SDL_AddAtomicInt(&j->refs, -1) == 1) { free(j->url); free(j); }
}
static int probe_thread(void *arg) {
    ProbeJob *j = arg;
    probe_raw(j->url, &j->fps, &j->hdr);
    SDL_SetAtomicInt(&j->done, 1);
    probe_unref(j);
    return 0;
}

/* false = the user cancelled */
static bool probe_video_wait(const char *url) {
    static char *last_url;
    static double last_fps;
    static bool last_hdr;
    if (last_url && url && !strcmp(last_url, url)) { g_probe_fps = last_fps; g_probe_hdr = last_hdr; return true; }
    double fps = 0;
    bool hdr = false;
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
        probe_unref(j);
        if (cancelled) return false;
    }
    free(last_url);
    last_url = url ? str_dup(url) : NULL;
    last_fps = fps;
    last_hdr = hdr;
    g_probe_fps = fps;
    g_probe_hdr = hdr;
    return true;
}

/* "always HDR" on the Pi: SDR video is drawn with the GPU and converted to HDR (PQ/BT.2020), so the TV does not
   switch to SDR; HDR video goes unchanged directly onto the display plane as always */
static bool g_sdr_to_hdr;

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
    bool keep_hdr = cfg_int("hdr_always", 0) != 0;
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0);
        int lf = open("/tmp/mpv-out.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
        setsid();
        if (apple_hdr) setenv("NOTATV_HDR_STATIC", "1000,0.005,4000,1000", 1);
        else unsetenv("NOTATV_HDR_STATIC");
        /* "always HDR": mpv must not set the TV back to SDR when it closes */
        if (keep_hdr) setenv("NOTATV_KEEP_HDR", "1", 1);
        else unsetenv("NOTATV_KEEP_HDR");
        execvp("mpv", argv);
        _exit(127);
    }
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
    double scrub;          /* seeking: target time in seconds (-1 = none) */
    double scrub_t;
    int scrub_n, scrub_dir;
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
    /* smooth ramp-up: every ~0.25 s (LG hold) the jump gets a little bigger, without big leaps in speed */
    static const double steps[] = {10, 10, 15, 20, 30, 40, 60, 80, 120, 180, 300, 600};
    enum { N_STEPS = sizeof steps / sizeof steps[0] };
    double now = app_time();
    bool cont = st->scrub >= 0 && now - st->scrub_t < 1.5 && st->scrub_dir == dir;
    if (st->scrub < 0) st->scrub = st->pos;
    st->scrub_n = cont ? st->scrub_n + 1 : 0;
    st->scrub_dir = dir;
    int i = st->scrub_n + (big ? 6 : 0);
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

/* live channel change: quick presses are gathered, and mpv only switches when you stop - otherwise
   mpv opens every stream in between and freezes */
static void zap(const PlayRequest *rq, MpvState *st, int d) {
    if (rq->playlist_n <= 0) { CMD(d > 0 ? "playlist-next" : "playlist-prev"); return; }
    int n = rq->playlist_n, base = st->zap >= 0 ? st->zap : st->plpos;
    st->zap = ((base + d) % n + n) % n;
    st->zap_t = app_time();
    if (rq->channel_preview) rq->channel_preview(rq->ud, st->zap);
}

static void ipc_read(MpvState *st, const PlayRequest *rq) {
    static char buf[65536];
    static size_t blen;
    for (;;) {
        ssize_t n = read(ipc_fd, buf + blen, sizeof buf - blen - 1);
        if (n <= 0) break;
        blen += n;
        buf[blen] = 0;
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
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
        blen = strlen(line);
        memmove(buf, line, blen + 1);
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
    add_arg(&argv, n, "--log-file=/tmp/mpv.log");
    if (rq->title) { char *s = str_printf("--force-media-title=%s", rq->title); add_arg(&argv, n, s); free(s); }
    if (rq->start > 1) { char *s = str_printf("--start=%.1f", rq->start); add_arg(&argv, n, s); free(s); }
    if (rq->playlist_start > 0) { char *s = str_printf("--playlist-start=%d", rq->playlist_start); add_arg(&argv, n, s); free(s); }
    if (mode && *mode) { char *s = str_printf("--drm-mode=%s", mode); add_arg(&argv, n, s); free(s); }

    /* audio */
    const char *adev = cfg_get("audio_device", "");
    if (*adev) { char *s = str_printf("--audio-device=%s", adev); add_arg(&argv, n, s); free(s); }
    if (cfg_int("audio_passthrough", 0)) {
        char codecs[64] = "ac3,eac3";
        if (cfg_int("pt_dts", 1)) strcat(codecs, ",dts");
        if (cfg_int("pt_hd", 0)) strcat(codecs, ",dts-hd,truehd");
        char *s = str_printf("--audio-spdif=%s", codecs);
        add_arg(&argv, n, s);
        free(s);
        /* ARC (without HD passthrough): multichannel audio that cannot be sent unchanged (TrueHD, AAC/FLAC/PCM 5.1)
         * is re-encoded to Dolby Digital 5.1 - stereo stays PCM (minch=3) */
        if (!cfg_int("pt_hd", 0)) add_arg(&argv, n, "--af=lavcac3enc=tospdif=yes:bitrate=640:minch=3");
    }
    int delay = cfg_int("audio_delay_ms", 0);
    if (delay) { char *s = str_printf("--audio-delay=%.3f", delay / 1000.0); add_arg(&argv, n, s); free(s); }
    /* picture */
    if (g_sdr_to_hdr) {
        add_arg(&argv, n, "--gpu-hwdec-interop=drmprime");
        add_arg(&argv, n, "--target-trc=pq");
        add_arg(&argv, n, "--target-prim=bt.2020");
    }
    if (cfg_int("hdr", 1)) add_arg(&argv, n, "--target-colorspace-hint=yes");
    else add_arg(&argv, n, "--tone-mapping=auto");
    /* sprog */
    { char *s = str_printf("--slang=%s", cfg_get("sub_lang", lang_media_default("sub_lang"))); add_arg(&argv, n, s); free(s); }
    { char *s = str_printf("--alang=%s", cfg_get("audio_lang", lang_media_default("audio_lang"))); add_arg(&argv, n, s); free(s); }
    if (rq->is_live) {
        add_arg(&argv, n, "--cache=yes");
        add_arg(&argv, n, "--demuxer-max-back-bytes=64MiB");
        add_arg(&argv, n, "--deinterlace=auto");
    }
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
    case A_NEXT: CMD("playlist-next"); break;
    case A_PREV: CMD("playlist-prev"); break;
    case A_INFO: CMD("script-message", "notatv-show", "8"); break;
    case A_SUBTITLE:
        CMD("cycle", "sub");
        CMD("show-text", T("Subtitles: ${current-tracks/sub/lang:${?sid==no:Off}} ${current-tracks/sub/title:}"), "2500");
        break;
    case A_MENU: CMD("script-message", "notatv-menu-open"); break;
    case A_AUDIO:
        CMD("cycle", "audio");
        CMD("show-text", T("Audio: ${current-tracks/audio/lang:} ${current-tracks/audio/title:} (${current-tracks/audio/codec:})"), "2500");
        break;
    case A_YELLOW: /* Yellow = action on what is shown: Twitch chat on/off (remembered). Aspect ratio and technical info are in the menu (Red) */
        if (rq->chat) {
            int on = !cfg_int("tw_chat", 1);
            cfg_set_int("tw_chat", on);
            CMD("script-message", "notatv-chat-show", on ? "1" : "0");
            CMD("show-text", on ? T("Chat: on") : T("Chat: off"), "1500");
        }
        break;
    case A_VOLUP: CMD("add", "volume", "5"); CMD("show-text", T("Volume: ${volume}%")); break;
    case A_VOLDOWN: CMD("add", "volume", "-5"); CMD("show-text", T("Volume: ${volume}%")); break;
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

PlayResult player_run(const PlayRequest *rq) {
    PlayResult res = {0};
    unlink(SOCK_PATH);
    bt_media("Pause"); /* Bluetooth: pause the phone, so the music does not keep playing into nothing */
    run_cmd("%s/music pause >/dev/null 2>&1", helper_path());

    /* frame rate and HDR are only checked if they are used */
    bool want_rate = cfg_int("match_rate", 1) && input_evdev_active() && !rq->is_live;
    bool want_hdr = board_is_rpi() && cfg_int("hdr_always", 0);
    g_probe_fps = 0;
    g_probe_hdr = false;
    if ((want_rate || want_hdr) && !probe_video_wait(rq->url)) {
        res.cancelled = true;
        return res;
    }

    /* billedfrekvens-match */
    char modes[3][48] = {"", "", ""};
    int n_modes = 0;
    if (cfg_int("match_rate", 1) && input_evdev_active()) {
        char res_s[32];
        if (preferred_res(res_s, sizeof res_s)) {
            double rate = rq->is_live ? cfg_int("iptv_rate", 50) : display_rate_for(g_probe_fps);
            if (rate > 0) {
                snprintf(modes[n_modes++], 48, "%s@%.3g", res_s, rate);
                if (rate != floor(rate)) snprintf(modes[n_modes++], 48, "%s@%.0f", res_s, ceil(rate));
            }
        }
    }
    snprintf(modes[n_modes++], 48, "%s", "");
    g_sdr_to_hdr = want_hdr && !g_probe_hdr;
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
        int pw, ph;
        /* two versions: the OSD script picks the HDR version when the TV is in HDR (PQ) */
        if (rq->poster && img_export_bgra(rq->poster, "/tmp/notatv-poster.bgra", 420, 630, &pw, &ph)) {
            if (board_is_rpi() && img_export_bgra_ex(rq->poster, "/tmp/notatv-poster-hdr.bgra", 420, 630, &pw, &ph, true))
                cJSON_AddStringToObject(m, "poster_hdr", "/tmp/notatv-poster-hdr.bgra");
            cJSON_AddStringToObject(m, "poster", "/tmp/notatv-poster.bgra");
            cJSON_AddNumberToObject(m, "poster_w", pw);
            cJSON_AddNumberToObject(m, "poster_h", ph);
        }
        char *js = cJSON_PrintUnformatted(m);
        FILE *f = fopen("/tmp/notatv-nowplaying.json", "w");
        if (f) { fputs(js, f); fclose(f); }
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
            poll(pfd, nf, 100);
            if (ipc_fd >= 0) ipc_read(&st, rq);
            input_poll();
            if (!input_evdev_active()) { /* PC development: SDL events (mpv has its own window) */
                SDL_Event e;
                while (SDL_PollEvent(&e)) input_handle_sdl(&e);
            }
            InputEvent ev;
            while (input_next(&ev)) handle_action(&ev, rq, &st, &quit, digits, &digit_t);
            if (rq->tick) rq->tick(rq->ud);
            if (st.scrub >= 0 && app_time() - st.scrub_t > 0.7) {
                char a[32];
                snprintf(a, sizeof a, "%.1f", st.scrub);
                CMD("seek", a, "absolute+keyframes");
                CMD("script-message", "notatv-scrub", "", "");
                st.scrub = -1;
            }
            if (st.zap >= 0 && app_time() - st.zap_t > 0.35) {
                if (st.zap != st.plpos) {
                    char idx[16];
                    snprintf(idx, sizeof idx, "%d", st.zap);
                    CMD("playlist-play-index", idx);
                }
                st.zap = -1;
            }
            if (digits[0] && app_time() - digit_t > 1.6) {
                int num = atoi(digits);
                digits[0] = 0;
                if (num > 0) {
                    char idx[16];
                    snprintf(idx, sizeof idx, "%d", num - 1);
                    CMD("playlist-play-index", idx);
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
            if (st.subsearch && rq->sub_search) {
                st.subsearch = false;
                char **labels = NULL;
                int n = rq->sub_search(rq->ud, &labels); /* blocks briefly; mpv keeps playing */
                cJSON *arr = cJSON_CreateArray();
                for (int i = 0; i < n; i++) { cJSON_AddItemToArray(arr, cJSON_CreateString(labels[i])); free(labels[i]); }
                free(labels);
                char *js = cJSON_PrintUnformatted(arr);
                cJSON_Delete(arr);
                FILE *f = fopen("/tmp/notatv-subs.json", "w");
                if (f) { fputs(js, f); fclose(f); }
                free(js);
                CMD("script-message", "notatv-subresults");
            }
            if (st.subpick >= 0 && rq->sub_fetch) {
                int idx = st.subpick;
                st.subpick = -1;
                char *path = rq->sub_fetch(rq->ud, idx);
                if (path) { CMD("sub-add", path, "select"); player_osd(T("Subtitles loaded"), 2000); free(path); }
                else player_osd(T("Could not download subtitles"), 3000);
            }
            if (rq->progress && app_time() - last_prog > 10 && st.have_dur) {
                rq->progress(rq->ud, st.pos, st.dur, false, st.paused);
                last_prog = app_time();
            }
            if (waitpid(pid, &status, WNOHANG) == pid) pid = -1;
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
    run_cmd("%s/music resume >/dev/null 2>&1", helper_path());
    if (!user_quit && !res.quality[0] && !res.goto_channel && res.exit_code != 0 && !st.have_dur)
        app_toast(T("Could not play %s"), rq->title ? rq->title : "videoen");
    return res;
}
