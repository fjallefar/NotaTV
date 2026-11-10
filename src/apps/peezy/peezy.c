/* NotaTV app package "peezy" - Plex, Jellyfin and Emby with Plezy's UI (Flutter, run by flutter-pi from this package)
 * and NotaTV's own player. See docs/peezy.md.
 *
 * Opening the app gives the display to flutter-pi (like Games gives it to tvmoon). When Plezy plays a video, the
 * nota_mpv plugin in flutter-pi lets go of the display and sends "play" over a unix socket; this file plays it with
 * player_run() and says "started" (mpv's IPC socket is up, the plugin follows it from there) and "ended". Everything
 * the app needs is in its package folder; what it saves is in its data folder (forget() wipes it). */
#define _GNU_SOURCE
#include "tv.h"
#include "vendor/cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CTL_SOCK "/tmp/notatv-peezy.sock"
#define LOG_FILE "/tmp/peezy.log"

extern const NotaApp nota_app; /* at the bottom */

typedef struct {
    int conn;          /* flutter-pi's plugin */
    char buf[65536];
    size_t len;
    bool started_sent; /* "started" for this playback */
    PlaySkip skip;
    bool has_skip;
} Session;

static void ctl_send(Session *se, const char *s) {
    if (se->conn < 0) return;
    size_t n = strlen(s);
    while (n) {
        ssize_t w = send(se->conn, s, n, MSG_NOSIGNAL);
        if (w <= 0) { if (errno == EINTR) continue; return; }
        s += w;
        n -= (size_t)w;
    }
}

/* Plezy's markers (from our Dart patch): {"markers":[{"start":s,"end":s,"credits":bool}], "next":"title"} */
static void skip_parse(Session *se, const cJSON *j) {
    memset(&se->skip, 0, sizeof se->skip);
    se->has_skip = false;
    if (!j) return;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(j, "markers")) {
        if (se->skip.n >= (int)(sizeof se->skip.m / sizeof se->skip.m[0])) break;
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(m, "start"), *b = cJSON_GetObjectItemCaseSensitive(m, "end");
        if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b) || b->valuedouble <= a->valuedouble) continue;
        se->skip.m[se->skip.n++] = (PlayMarker){ a->valuedouble, b->valuedouble,
                                                  cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "credits")) };
    }
    se->has_skip = se->skip.n > 0;
}

static void on_skip_info(void *ud, PlaySkip *out) {
    Session *se = ud;
    if (se->has_skip) *out = se->skip;
}

/* lines from the plugin while a video plays (skip markers may come late) */
static void ctl_poll_playing(Session *se) {
    if (se->conn < 0) return;
    for (;;) {
        if (se->len >= sizeof se->buf - 1) se->len = 0;
        ssize_t r = recv(se->conn, se->buf + se->len, sizeof se->buf - 1 - se->len, MSG_DONTWAIT);
        if (r <= 0) break;
        se->len += (size_t)r;
    }
    se->buf[se->len] = 0;
    char *nl;
    while ((nl = memchr(se->buf, '\n', se->len))) {
        *nl = 0;
        if (!strncmp(se->buf, "skip\t", 5)) {
            cJSON *j = cJSON_Parse(se->buf + 5);
            skip_parse(se, j);
            cJSON_Delete(j);
        }
        size_t used = (size_t)(nl + 1 - se->buf);
        memmove(se->buf, nl + 1, se->len - used);
        se->len -= used;
    }
}

static void on_tick(void *ud) {
    Session *se = ud;
    /* player_run calls this once mpv runs: its IPC socket is there for the plugin */
    if (!se->started_sent) {
        se->started_sent = true;
        ctl_send(se, "started\n");
    }
    ctl_poll_playing(se);
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && *v->valuestring ? v->valuestring : NULL;
}

static void play(Session *se, const char *json) {
    cJSON *j = cJSON_Parse(json);
    const char *url = j ? jstr(j, "url") : NULL;
    if (!url) {
        cJSON_Delete(j);
        ctl_send(se, "ended\t0\n");
        return;
    }
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(j, "meta");
    const cJSON *extra = cJSON_GetObjectItemCaseSensitive(j, "extra");
    int n = cJSON_GetArraySize(extra), k = 0;
    const char **argv = calloc((size_t)n + 1, sizeof *argv);
    const cJSON *e;
    cJSON_ArrayForEach(e, extra) if (cJSON_IsString(e) && !strncmp(e->valuestring, "--", 2)) argv[k++] = e->valuestring;
    skip_parse(se, cJSON_GetObjectItemCaseSensitive(j, "skip"));
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "start");
    PlayRequest rq = {
        .url = url,
        .title = meta && jstr(meta, "title") ? jstr(meta, "title") : jstr(j, "title"),
        .subtitle = meta ? jstr(meta, "subtitle") : NULL,
        .description = meta ? jstr(meta, "description") : NULL,
        .poster = meta ? jstr(meta, "poster") : NULL,
        .start = cJSON_IsNumber(st) ? st->valuedouble : 0,
        .extra = argv,
        .tick = on_tick,
        .skip_info = on_skip_info,
        .ud = se,
    };
    const cJSON *hdr = meta ? cJSON_GetObjectItemCaseSensitive(meta, "hdr") : NULL;
    if (cJSON_IsBool(hdr)) rq.media_hdr = cJSON_IsTrue(hdr) ? 2 : 1;
    const cJSON *w = meta ? cJSON_GetObjectItemCaseSensitive(meta, "width") : NULL;
    if (cJSON_IsNumber(w)) rq.media_w = (int)w->valuedouble;
    se->started_sent = false;
    /* flutter-pi has let go of the display: tvshell takes it, and player_run hands it to mpv as for any app */
    app_video_resume();
    PlayResult res = player_run(&rq);
    app_video_suspend();
    char line[64];
    snprintf(line, sizeof line, "ended\t%d\n", res.eof ? 1 : 0);
    ctl_send(se, line);
    free(argv);
    cJSON_Delete(j);
}

static int listen_sock(void) {
    unlink(CTL_SOCK);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CTL_SOCK);
    mode_t old = umask(077);
    int ok = bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(fd, 1) == 0;
    umask(old);
    if (!ok) { close(fd); return -1; }
    return fd;
}

static void stop_child(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 30; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* the folders Plezy keeps its sign-in, settings and cache in (all inside the app's data folder) */
static void env_dirs(const char *data) {
    static const char *const sub[] = { "home", "config", "share", "cache", "home/Documents", "home/Downloads", "bin", NULL };
    for (int i = 0; sub[i]; i++) {
        char *p = str_printf("%s/%s", data, sub[i]);
        mkdir(p, 0700);
        free(p);
    }
    /* Plezy asks for the documents folder; there is no xdg-user-dir on the box, so it is written down here */
    char *f = str_printf("%s/config/user-dirs.dirs", data);
    FILE *fp = fopen(f, "w");
    if (fp) {
        fprintf(fp, "XDG_DOCUMENTS_DIR=\"$HOME/Documents\"\nXDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n");
        fclose(fp);
    }
    free(f);
    /* ... and Flutter's path_provider runs the program xdg-user-dir to read it: a tiny stand-in (PATH, see child_exec) */
    f = str_printf("%s/bin/xdg-user-dir", data);
    fp = fopen(f, "w");
    if (fp) {
        fprintf(fp, "#!/bin/sh\ncase \"$1\" in\nDOCUMENTS) echo \"$HOME/Documents\" ;;\n"
                    "DOWNLOAD) echo \"$HOME/Downloads\" ;;\n*) echo \"$HOME\" ;;\nesac\n");
        fclose(fp);
        chmod(f, 0700);
    }
    free(f);
}

static void child_exec(const char *pkg, const char *data) {
    prctl(PR_SET_PDEATHSIG, SIGTERM); /* tvshell died: the display must become free */
    int dn = open("/dev/null", O_RDONLY);
    if (dn >= 0) dup2(dn, 0);
    int lf = open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
    setsid();
    char *v;
    setenv("NOTA_PEEZY_SOCK", CTL_SOCK, 1);
    setenv("HOME", (v = str_printf("%s/home", data)), 1);
    setenv("XDG_CONFIG_HOME", (v = str_printf("%s/config", data)), 1);
    setenv("XDG_DATA_HOME", (v = str_printf("%s/share", data)), 1);
    setenv("XDG_CACHE_HOME", (v = str_printf("%s/cache", data)), 1);
    setenv("XKB_CONFIG_ROOT", (v = str_printf("%s/xkb", pkg)), 1);
    const char *lang = i18n_lang();
    setenv("LANG", !strcmp(lang, "da") ? "da_DK.UTF-8" : "en_US.UTF-8", 1);
    setenv("LC_ALL", !strcmp(lang, "da") ? "da_DK.UTF-8" : "en_US.UTF-8", 1);
    const char *path = getenv("PATH");
    setenv("PATH", (v = str_printf("%s/bin:%s", data, path ? path : "/usr/bin:/bin")), 1);
    setenv("XKB_DEFAULT_LAYOUT", !strcmp(lang, "da") ? "dk" : "us", 1);
    char *bin = str_printf("%s/flutter-pi", pkg);
    char *bundle = str_printf("%s/bundle", pkg);
    /* the engine opens Plezy's native libraries (sqlite3) as ./<name>: relative to the working folder */
    if (chdir(bundle) != 0) _exit(126);
    execl(bin, "flutter-pi", "--release", bundle, (char *)NULL);
    _exit(127);
}

static void peezy_run(void) {
    const char *pkg = app_pkg_dir(&nota_app);
    char *bin = str_printf("%s/flutter-pi", pkg);
    if (!*pkg || access(bin, X_OK) != 0) {
        free(bin);
        app_toast(T("Peezy is incomplete - install it again in the App Store"));
        return;
    }
    free(bin);
    const char *data = app_data(&nota_app);
    env_dirs(data);
    int lfd = listen_sock();
    if (lfd < 0) { app_toast(T("Peezy could not start")); return; }
    audio_bg_take();
    bool video_off = app_video_suspend();
    pid_t pid = fork();
    if (pid == 0) child_exec(pkg, data);
    Session se = { .conn = -1 };
    double t0 = app_time();
    int status = 0;
    bool gone = pid < 0, home = false;
    while (!gone) {
        struct pollfd p[42];
        int fds[38];
        int nf = input_fds(fds, 38);
        for (int i = 0; i < nf; i++) p[i] = (struct pollfd){ fds[i], POLLIN, 0 };
        int il = nf, ic = -1;
        p[nf++] = (struct pollfd){ lfd, POLLIN, 0 };
        if (se.conn >= 0) { ic = nf; p[nf++] = (struct pollfd){ se.conn, POLLIN, 0 }; }
        poll(p, (nfds_t)nf, 250);
        /* flutter-pi reads the remote and keyboards itself; tvshell only watches for Home */
        input_poll();
        InputEvent ev;
        while (input_next(&ev)) {
            input_touch();
            if (ev.a == A_HOME && !ev.repeat) home = true;
        }
        if (home) { SDL_Log("peezy: Hjem - lukker"); stop_child(pid); break; }
        if (p[il].revents & POLLIN) {
            int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) {
                if (se.conn >= 0) close(se.conn);
                se.conn = c;
                se.len = 0;
            }
        }
        if (ic >= 0 && (p[ic].revents & (POLLIN | POLLHUP))) {
            if (se.len >= sizeof se.buf - 1) se.len = 0;
            ssize_t r = recv(se.conn, se.buf + se.len, sizeof se.buf - 1 - se.len, MSG_DONTWAIT);
            if (r > 0) {
                se.len += (size_t)r;
                se.buf[se.len] = 0;
                char *nl;
                while ((nl = memchr(se.buf, '\n', se.len))) {
                    *nl = 0;
                    char *line = strdup(se.buf);
                    size_t used = (size_t)(nl + 1 - se.buf);
                    memmove(se.buf, nl + 1, se.len - used);
                    se.len -= used;
                    if (!strncmp(line, "play\t", 5)) play(&se, line + 5);
                    free(line);
                }
            } else if (r == 0) {
                close(se.conn);
                se.conn = -1;
            }
        }
        if (waitpid(pid, &status, WNOHANG) == pid) gone = true;
    }
    if (se.conn >= 0) close(se.conn);
    close(lfd);
    unlink(CTL_SOCK);
    if (video_off) app_video_resume();
    InputEvent ev;
    while (input_next(&ev)) {}
    input_touch();
    if (gone && !home) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        SDL_Log("peezy: flutter-pi sluttede (%d) efter %.0f s", code, app_time() - t0);
        if (code != 0) app_toast(T("Peezy stopped unexpectedly"));
    }
}

/* ---------- screen: black while flutter-pi has the display ---------- */
typedef struct { bool ran; } PeezyView;

static void cn_update(Screen *s, float dt) {
    (void)dt;
    PeezyView *v = s->st;
    if (v->ran) return;
    v->ran = true;
    peezy_run();
    if (app_close) app_close(s);
    else app_pop();
}
static void cn_draw(Screen *s) {
    (void)s;
    fill_rect(0, 0, UI_W, UI_H, (Color){ 0, 0, 0, 255 });
}
static bool cn_input(Screen *s, const InputEvent *ev) { (void)s; (void)ev; return true; }
static void cn_destroy(Screen *s) { free(s->st); free(s); }
static Screen *peezy_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "peezy";
    s->st = calloc(1, sizeof(PeezyView));
    s->update = cn_update;
    s->draw = cn_draw;
    s->input = cn_input;
    s->destroy = cn_destroy;
    return s;
}

/* sign-in, servers, settings, cache and watch history: everything Plezy saved is in the data folder */
static void peezy_forget(void) {
    wipe_dir(app_data(&nota_app));
    wipe_file(LOG_FILE);
    wipe_session_traces();
}

static void peezy_logout(void) { app_ask_signout("Peezy", peezy_forget); }
static const Item peezy_items[] = {
    {N_("Sign out of Peezy"), IT_ACTION, .action = peezy_logout,
     .help = N_("Deletes all servers, accounts and settings Peezy has saved on this TV")},
};

const NotaApp nota_app = {
    NOTA_API, "peezy", "Peezy", N_("Plex, Jellyfin and Emby - based on Plezy"), APP_VERSION, 22, APP_DEFAULT_OFF,
    ICON_FILM, {58, 46, 120, 255}, {24, 18, 56, 255}, {255, 255, 255, 255},
    .open = peezy_screen,
    .forget = peezy_forget,
    .settings = peezy_items,
    .n_settings = sizeof peezy_items / sizeof peezy_items[0],
};
