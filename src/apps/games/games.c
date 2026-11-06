/* NotaTV app package "games" - Games: streaming from a PC with Sunshine via tvmoon (simple test version).
 * Finds PCs via mDNS, pairs with a PIN, shows apps and starts the stream. */
#define _GNU_SOURCE
#include "games.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXROW 32

enum { M_SERVERS, M_PAIR, M_APPS };

typedef struct {
    char name[64], addr[64], status[16];
    int id;
} Row;

typedef struct {
    int mode, sel;
    pthread_mutex_t mu;
    bool busy;          /* background job running */
    bool quiet;         /* the running job is a silent re-search (no spinner, input stays free) */
    double next_scan;   /* when to search again while no PC answers (0 = do not) */
    int scans;          /* re-searches since the screen opened (for the back-off) */
    bool scan;          /* the next search also port-scans the subnet if mDNS finds nothing */
    unsigned gen;       /* bumped by every job start; a search whose gen is stale drops its result */
    Row rows[MAXROW];
    int n;
    int running;        /* running app id on the PC (0 = none) */
    char host[64], host_name[64];
    char pin[8], msg[160];
    pid_t pair_pid;
    bool pair_done, pair_ok;
} Games;

static char *field(char **p) {
    char *s = *p, *t = s ? strchr(s, '\t') : NULL;
    if (t) { *t = 0; *p = t + 1; } else *p = NULL;
    return s ? s : "";
}

static double mono_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---------- background jobs ---------- */

static void *discover_job(void *arg) {
    Games *g = arg;
    pthread_mutex_lock(&g->mu);
    unsigned gen = g->gen;
    bool scan = g->scan;
    g->scan = false;
    pthread_mutex_unlock(&g->mu);
    const char *av[] = {"/usr/bin/tvmoon", "discover", "2000", scan ? "scan" : NULL, NULL};
    char *out = run_capture_argv(av);
    pthread_mutex_lock(&g->mu);
    if (gen != g->gen || g->mode != M_SERVERS) {
        /* the user moved on (OK on a PC, pairing) while a silent search ran: drop the result */
        if (gen == g->gen) g->busy = g->quiet = false;
        pthread_mutex_unlock(&g->mu);
        free(out);
        return NULL;
    }
    g->n = 0;
    char *sv_line;
    for (char *line = strtok_r(out, "\n", &sv_line); line && g->n < MAXROW; line = strtok_r(NULL, "\n", &sv_line)) {
        if (strncmp(line, "server\t", 7) != 0) continue;
        char *p = line + 7;
        Row *r = &g->rows[g->n++];
        snprintf(r->name, sizeof r->name, "%s", field(&p));
        snprintf(r->addr, sizeof r->addr, "%s", field(&p));
        char *port = field(&p);
        if (*port && strcmp(port, "47989")) { size_t l = strlen(r->addr); snprintf(r->addr + l, sizeof r->addr - l, ":%s", port); }
        snprintf(r->status, sizeof r->status, "%s", field(&p));
        r->id = atoi(field(&p));
    }
    /* last used PC, if mDNS did not find it */
    const char *last = cfg_get("moon_host", "");
    bool have = false;
    for (int i = 0; i < g->n; i++) have |= !strcmp(g->rows[i].addr, last);
    if (*last && !have && g->n < MAXROW) {
        Row *r = &g->rows[g->n++];
        snprintf(r->name, sizeof r->name, T("Last used"));
        snprintf(r->addr, sizeof r->addr, "%s", last);
        snprintf(r->status, sizeof r->status, "?");
    }
    if (g->sel >= g->n) g->sel = g->n > 0 ? g->n - 1 : 0;
    /* Keep searching while no PC answers: one small mDNS query, 3 s apart at first, then every 10 s.
     * It stops when a PC that answers shows up, or when the screen is left (update only runs on top).
     * The port scan (one connect per address on our subnet) only runs on open, on "Search again"
     * and on every 3rd re-search (~every 30 s), so the network is not flooded. */
    bool reachable = false;
    for (int i = 0; i < g->n; i++)
        reachable |= !strcmp(g->rows[i].status, "paired") || !strcmp(g->rows[i].status, "unpaired");
    g->next_scan = reachable ? 0 : mono_s() + (g->scans < 5 ? 3 : 10);
    g->busy = g->quiet = false;
    pthread_mutex_unlock(&g->mu);
    free(out);
    return NULL;
}

static void *apps_job(void *arg) {
    Games *g = arg;
    const char *av[] = {"/usr/bin/tvmoon", "apps", g->host, NULL};
    char *out = run_capture_argv(av);
    pthread_mutex_lock(&g->mu);
    g->n = 0;
    g->running = 0;
    g->msg[0] = 0;
    char *sv_line;
    for (char *line = strtok_r(out, "\n", &sv_line); line && g->n < MAXROW; line = strtok_r(NULL, "\n", &sv_line)) {
        char *p = line;
        char *kind = field(&p);
        if (!strcmp(kind, "running")) g->running = atoi(field(&p));
        else if (!strcmp(kind, "app")) {
            Row *r = &g->rows[g->n++];
            r->id = atoi(field(&p));
            snprintf(r->name, sizeof r->name, "%s", field(&p));
        } else if (!strcmp(kind, "error")) snprintf(g->msg, sizeof g->msg, "%s", field(&p));
    }
    g->busy = false;
    pthread_mutex_unlock(&g->mu);
    free(out);
    return NULL;
}

static void *pair_job(void *arg) {
    Games *g = arg;
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) != 0) return NULL;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0);
        dup2(dn, 2);
        execl("/usr/bin/tvmoon", "tvmoon", "pair", g->host, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    pthread_mutex_lock(&g->mu);
    g->pair_pid = pid;
    pthread_mutex_unlock(&g->mu);
    FILE *f = fdopen(fd[0], "r");
    char line[256];
    bool ok = false;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *p = line;
        char *kind = field(&p);
        pthread_mutex_lock(&g->mu);
        if (!strcmp(kind, "pin")) snprintf(g->pin, sizeof g->pin, "%s", field(&p));
        else if (!strcmp(kind, "paired")) ok = true;
        else if (!strcmp(kind, "error")) snprintf(g->msg, sizeof g->msg, "%s", field(&p));
        pthread_mutex_unlock(&g->mu);
    }
    if (f) fclose(f);
    waitpid(pid, NULL, 0);
    pthread_mutex_lock(&g->mu);
    g->pair_pid = 0;
    g->pair_ok = ok;
    g->pair_done = true;
    pthread_mutex_unlock(&g->mu);
    return NULL;
}

static void start_job_q(Games *g, void *(*fn)(void *), bool quiet) {
    pthread_t t;
    g->busy = true;
    g->quiet = quiet;
    g->gen++;
    g->next_scan = 0;
    if (pthread_create(&t, NULL, fn, g) == 0) pthread_detach(t);
    else g->busy = g->quiet = false;
}

static void start_job(Games *g, void *(*fn)(void *)) { start_job_q(g, fn, false); }

static void open_apps(Games *g) {
    g->mode = M_APPS;
    g->sel = 0;
    g->n = 0;
    cfg_set("moon_host", g->host);
    start_job(g, apps_job);
}

/* ---------- screen ---------- */

static void games_enter(Screen *s) {
    Games *g = s->st;
    g->mode = M_SERVERS;
    g->sel = 0;
    g->scans = 0;
    g->scan = true;
    start_job(g, discover_job);
}

static void games_update(Screen *s, float dt) {
    Games *g = s->st;
    pthread_mutex_lock(&g->mu);
    if (g->mode == M_PAIR && g->pair_done) {
        g->pair_done = false;
        if (g->pair_ok) { pthread_mutex_unlock(&g->mu); app_toast(T("Paired with %s"), g->host_name); open_apps(g); return; }
        g->mode = M_SERVERS;
        g->next_scan = mono_s() + 3;
        if (!g->msg[0]) snprintf(g->msg, sizeof g->msg, T("Pairing interrupted"));
        app_toast("%s", tr_msg(g->msg));
    }
    if (g->mode == M_SERVERS && !g->busy && g->next_scan > 0 && mono_s() >= g->next_scan) {
        g->scans++;
        g->scan = g->scans % 3 == 0; /* ~9 s, ~25 s, ~55 s after opening, then every 30 s */
        start_job_q(g, discover_job, true);
    }
    pthread_mutex_unlock(&g->mu);
}

static void draw_rows(Games *g, const char *const *labels, const char *const *right, int n) {
    for (int i = 0; i < n; i++) {
        float y = 170 + i * 84;
        bool f = i == g->sel;
        if (f) fill_rrect(96, y, UI_W - 192, 74, 14, C_FOCUS);
        text_draw_clip(F_SEMI, 28, 146, y + 20, 1200, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, labels[i]);
        if (right && right[i]) text_draw(F_REG, 24, UI_W - 130, y + 23, f ? RGB(60, 60, 60) : C_TEXT3, AL_RIGHT, right[i]);
    }
}

static void games_draw(Screen *s) {
    Games *g = s->st;
    draw_background();
    pthread_mutex_lock(&g->mu);
    if (g->mode == M_PAIR) {
        draw_topbar(T("Games - pairing"));
        /* localhost, not the PC's IP: Sunshine's CSRF check only accepts its default origins (localhost),
         * so typing the PIN on https://<ip>:47990 fails unless csrf_allowed_origins lists that address */
        text_draw(F_SEMI, 32, UI_W / 2, 300, C_TEXT, AL_CENTER, T("On the PC, open https://localhost:47990 → PIN"));
        if (g->pin[0]) text_draw(F_BOLD, 140, UI_W / 2, 400, C_TEXT, AL_CENTER, g->pin);
        else draw_spinner(UI_W / 2, 480, 30, C_TEXT2);
        text_draw(F_REG, 26, UI_W / 2, 640, C_TEXT3, AL_CENTER, T("Enter the code on Sunshine's page. Back = cancel"));
    } else if (g->mode == M_SERVERS) {
        draw_topbar(T("Games"));
        text_draw(F_REG, 24, 96, 118, C_TEXT3, AL_LEFT, T("PCs with Sunshine on the network"));
        if (g->busy && !g->quiet) draw_spinner(UI_W / 2, 400, 30, C_TEXT2);
        else if (g->n == 0) draw_empty(T("No PC found"), T("Searching… Start Sunshine on the PC. Its firewall must allow TCP 47984-48010 and UDP 47998-48000 (UDP 5353 makes it found faster)."));
        else {
            const char *l[MAXROW], *r[MAXROW];
            char lb[MAXROW][160];
            for (int i = 0; i < g->n; i++) {
                snprintf(lb[i], sizeof lb[i], "%s  (%s)", g->rows[i].name, g->rows[i].addr);
                l[i] = lb[i];
                const char *st = g->rows[i].status;
                r[i] = !strcmp(st, "paired") ? T("Paired") : !strcmp(st, "unpaired") ? T("Not paired - OK to pair")
                       : !strcmp(st, "offline") ? T("Unreachable") : "";
            }
            draw_rows(g, l, r, g->n);
        }
    } else {
        draw_topbar(g->host_name[0] ? g->host_name : T("Games"));
        text_draw(F_REG, 24, 96, 118, C_TEXT3, AL_LEFT,
                  T("OK = start · While playing, Red opens the menu (continue, stats, quit)"));
        if (g->busy) draw_spinner(UI_W / 2, 400, 30, C_TEXT2);
        else if (g->n == 0) draw_empty(T("No apps"), g->msg[0] ? tr_msg(g->msg) : T("The PC sent no apps."));
        else {
            const char *l[MAXROW], *r[MAXROW];
            for (int i = 0; i < g->n; i++) {
                l[i] = g->rows[i].name;
                r[i] = g->rows[i].id == g->running ? T("Running - OK continues") : NULL;
            }
            draw_rows(g, l, r, g->n);
        }
    }
    pthread_mutex_unlock(&g->mu);
}

static void stream(Games *g, int appid) {
    char id[16];
    snprintf(id, sizeof id, "%d", appid);
    char res[24], br[16], fps[8], codec[8], mode[24];
    snprintf(res, sizeof res, "%s", cfg_get("moon_res", "1920x1080"));
    snprintf(br, sizeof br, "%d", cfg_int("moon_bitrate", 20000));
    snprintf(fps, sizeof fps, "%d", cfg_int("moon_fps", 60));
    snprintf(codec, sizeof codec, "%s", cfg_get("moon_codec", "auto"));
    snprintf(mode, sizeof mode, "%s", cfg_get("moon_mode", "1920x1080@120"));
    char *argv[16] = {"tvmoon", "stream", g->host, id, "-r", res, "-b", br, "-f", fps, "-c", codec};
    int n = 12;
    if (mode[0]) { argv[n++] = "-m"; argv[n++] = mode; }
    if (hdr_cfg("moon_hdr", 0)) argv[n++] = "--hdr";
    argv[n] = NULL;
    int r = moon_run(argv, NULL, NULL);
    if (r != 0) app_toast(T("The stream stopped with an error (see /tmp/tvmoon.log)"));
    open_apps(g);
}

static void *quit_job(void *arg) {
    Games *g = arg;
    const char *av[] = {"/usr/bin/tvmoon", "quit", g->host, NULL};
    free(run_capture_argv(av));
    apps_job(g);
    return NULL;
}

/* Red = menu (the same meaning everywhere in NotaTV) */
static void games_menu_cb(void *ud, int idx) {
    Games *g = ud;
    if (idx < 0 || (g->busy && !g->quiet)) return;
    if (g->mode == M_SERVERS) { g->sel = 0; g->scan = true; start_job(g, discover_job); return; }
    if (g->running && idx == 0) { app_toast(T("Quitting the game on the PC…")); start_job(g, quit_job); return; }
    start_job(g, apps_job); /* "Refresh the list" */
}

static bool games_input(Screen *s, const InputEvent *ev) {
    Games *g = s->st;
    if (g->mode == M_PAIR) {
        if (ev->a == A_BACK || ev->a == A_LEFT) {
            pthread_mutex_lock(&g->mu);
            /* SIGKILL: tvmoon up to 1.0.10 catches SIGTERM and kept waiting for the PIN, so Back did nothing */
            if (g->pair_pid > 0) kill(g->pair_pid, SIGKILL);
            pthread_mutex_unlock(&g->mu);
        }
        return true;
    }
    if (g->busy && !g->quiet && ev->a != A_BACK) return ev->a != A_HOME;
    switch (ev->a) {
    case A_UP: if (g->sel > 0) g->sel--; return true;
    case A_DOWN: if (g->sel < g->n - 1) g->sel++; return true;
    case A_MENU:
        if (g->mode == M_SERVERS) {
            const char *items[] = {T("Search for PCs again")};
            choice_open(T("Games"), items, 1, 0, games_menu_cb, g);
        } else if (g->running) {
            const char *items[] = {T("Quit the running game on the PC"), T("Refresh the list")};
            choice_open(g->host_name[0] ? g->host_name : T("Games"), items, 2, 0, games_menu_cb, g);
        } else {
            static const char *items[] = {N_("Refresh the list")};
            choice_open(g->host_name[0] ? g->host_name : T("Games"), items, 1, 0, games_menu_cb, g);
        }
        return true;
    case A_BACK: case A_LEFT:
        if (g->mode == M_APPS) { g->mode = M_SERVERS; g->sel = 0; g->scan = true; start_job(g, discover_job); return true; }
        app_pop();
        return true;
    case A_OK:
        if (g->n == 0) return true;
        if (g->mode == M_SERVERS) {
            pthread_mutex_lock(&g->mu); /* a silent search may be rewriting the rows */
            if (g->sel >= g->n) { pthread_mutex_unlock(&g->mu); return true; }
            Row *r = &g->rows[g->sel];
            snprintf(g->host, sizeof g->host, "%s", r->addr);
            snprintf(g->host_name, sizeof g->host_name, "%s", r->name);
            bool pair = !strcmp(r->status, "unpaired");
            if (pair) g->mode = M_PAIR;
            else g->gen++; /* open_apps below; any silent search drops its result */
            pthread_mutex_unlock(&g->mu);
            if (pair) {
                g->pin[0] = g->msg[0] = 0;
                g->pair_done = false;
                pthread_t t;
                if (pthread_create(&t, NULL, pair_job, g) == 0) pthread_detach(t);
            } else open_apps(g);
        } else stream(g, g->rows[g->sel].id);
        return true;
    default: return false;
    }
}

/* the state lives all the time (background jobs can still write to it when the screen closes) */
static Games *G;

static void games_destroy(Screen *s) { free(s); }

Screen *games_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    if (!G) {
        G = calloc(1, sizeof *G);
        pthread_mutex_init(&G->mu, NULL);
    }
    Games *g = G;
    s->name = "games";
    s->st = g;
    s->enter = games_enter;
    s->update = games_update;
    s->draw = games_draw;
    s->input = games_input;
    s->destroy = games_destroy;
    return s;
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
static const char *const mres_o[] = {"1280x720", "1920x1080", "2560x1440", "3840x2160", NULL};
static const char *const mres_l[] = {"720p", "1080p", "1440p", "4K", NULL};
static const char *const mfps_o[] = {"30", "60", "90", "120", NULL}, *const mfps_l[] = {"30", "60", "90", "120", NULL};
static const char *const mbr_o[] = {"5000", "10000", "15000", "20000", "30000", "40000", "60000", "80000", NULL};
static const char *const mbr_l[] = {"5 Mbit/s", "10 Mbit/s", "15 Mbit/s", "20 Mbit/s", "30 Mbit/s", "40 Mbit/s", "60 Mbit/s", "80 Mbit/s", NULL};
static const char *const mcod_o[] = {"auto", "hevc", "h264", NULL}, *const mcod_l[] = {N_("Automatic (HEVC)"), "HEVC", N_("H.264 (slow)"), NULL};
static const char *const mmode_o[] = {"", "1920x1080@120", "1920x1080@60", "3840x2160@60", NULL};
static const char *const mmode_l[] = {N_("Keep the UI's"), N_("1080p 120 Hz (lowest latency)"), "1080p 60 Hz", "4K 60 Hz", NULL};
/* the Pi 5 has no hardware H.264 decoder: there tvmoon always streams HEVC */
static const char *codec_locked(void) { return board_pi && board_pi() == 5 ? T("HEVC (the Pi 5 has no H.264 decoder)") : NULL; }
static const Item game_items[] = {
    {N_("Resolution"), IT_CYCLE, "moon_res", "1920x1080", mres_o, mres_l, .help = N_("The resolution the PC streams in")},
    {N_("Frames per second"), IT_CYCLE, "moon_fps", "60", mfps_o, mfps_l, .help = N_("Above 60 requires the display mode to be 120 Hz")},
    {"Bitrate", IT_CYCLE, "moon_bitrate", "20000", mbr_o, mbr_l, .help = N_("Higher = sharper picture, but more data on the network")},
    {"Codec", IT_CYCLE, "moon_codec", "auto", mcod_o, mcod_l, .help = N_("HEVC decodes in ~4 ms, H.264 takes ~67 ms on a Pi 4"), .locked = codec_locked},
    {N_("Display mode while gaming"), IT_CYCLE, "moon_mode", "1920x1080@120", mmode_o, mmode_l, .help = N_("120 Hz halves the wait for the next frame. The TV switches mode at start")},
    {"HDR", IT_TOGGLE, "moon_hdr", "0", set_onoff, .help = N_("HEVC 10-bit HDR, if the PC's display is in HDR"), .locked = set_hdr_locked},
};

const NotaApp nota_app = {
    NOTA_API, "games", N_("Games"), N_("Play games from your PC via Sunshine (Moonlight)"), APP_VERSION, 80, 0,
    ICON_PLAY, {30, 160, 90, 255}, {12, 90, 50, 255}, {255, 255, 255, 255},
    .open = games_screen,
    .settings = game_items, .n_settings = sizeof game_items / sizeof game_items[0],
};
