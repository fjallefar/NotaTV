/* mediarun - starts the programs that handle media from the network: mpv, ffprobe, yt-dlp and UxPlay.
 *
 * /usr/bin/mpv, /usr/bin/ffprobe, /usr/bin/yt-dlp and /usr/bin/uxplay are symlinks to this program (post-build.sh),
 * so every caller - tvshell, the apps, mpv's own yt-dlp hook - goes through it. The real programs live in
 * /usr/libexec/notatv/media/ (yt-dlp is a tool from the App Store in <data>/tools/yt-dlp, not in the image).
 *
 * By default (media_user=1; media_user=0 turns it off for troubleshooting) a program started by root runs as the user "media" with
 * only the groups video and audio: a bug in a decoder (ffmpeg), in yt-dlp or in YouTube's JavaScript that yt-dlp runs
 * then no longer gives root on the box. Before it switches user, it gives the program what it needs and could not
 * reach as "media":
 *   --log-file=<path>                       opened here and handed over as /proc/self/fd/<n> (the log folders are root's)
 *   --input-ipc-server=<path>               an old socket left by root is removed (mpv could not replace it)
 *   cookies=<path> / --cookies <path>       copied into media's home (YouTube logins; the copy goes at sign-out)
 *   -reg <path>                             UxPlay's list of trusted phones is kept in media's home
 *   yt-dlp                                  gets its cache in media's home
 *   /tmp/notatv-*.json/.bgra/.m3u/-sub-*.srt  readable for the group media (now playing, channels, posters, subtitles)
 * With media_user=0, or when "media" is missing, the program just runs as before (as root). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define DATA "/data/notatv"
#define CONF DATA "/notatv.conf"
#define HOME_DIR DATA "/media"
#define TOOLS DATA "/tools"
#define REAL "/usr/libexec/notatv/media/"

static void warn(const char *fmt, const char *a) {
    fprintf(stderr, "mediarun: ");
    fprintf(stderr, fmt, a ? a : "");
    fputc('\n', stderr);
}

/* "key=1" in notatv.conf (as in tvshell: the first line with the key counts) */
static int cfg_on(const char *key, int def) {
    FILE *f = fopen(CONF, "r");
    if (!f) return def;
    char *line = NULL;
    size_t cap = 0, kl = strlen(key);
    int on = def;
    while (getline(&line, &cap, f) >= 0)
        if (!strncmp(line, key, kl) && line[kl] == '=') { on = atoi(line + kl + 1) != 0; break; }
    free(line);
    fclose(f);
    return on;
}

static void read_small(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t r = fread(out, 1, n - 1, f);
    out[r] = 0;
    fclose(f);
}

/* "key": "value" from a small JSON file (the tool manifest; written by our own scripts/apps-catalog.py) */
static int json_str(const char *txt, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(txt, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), '"');
    if (!p) return 0;
    const char *e = strchr(++p, '"');
    if (!e || (size_t)(e - p) >= n) return 0;
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return 1;
}

/* yt-dlp: the tool from the App Store (<data>/tools/yt-dlp, see src/tools). Since F1 step 1 (audit 9/11) the image
   has none; the box installs it with the first catalog after start. 0 = not there (yet) */
static int ytdlp_path(char *out, size_t n) {
    char man[2048], bin[64];
    read_small(TOOLS "/yt-dlp/manifest.json", man, sizeof man);
    if (!json_str(man, "bin", bin, sizeof bin) || bin[0] == '.') return 0;
    for (const char *c = bin; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("._-", *c))) return 0;
    snprintf(out, n, TOOLS "/yt-dlp/%s", bin);
    return access(out, X_OK) == 0;
}

/* a copy of src that only media can read, in media's home (dst gets the name tag) */
static char *stage_copy(const char *src, const char *tag, uid_t uid, gid_t gid) {
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char name[200];
    snprintf(name, sizeof name, "%s", base);
    for (char *c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-')) *c = '_';
    char *dst = NULL;
    if (asprintf(&dst, HOME_DIR "/%s-%s", tag, name) < 0) return NULL;
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) { free(dst); return NULL; }
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (out < 0) { close(in); free(dst); return NULL; }
    char buf[65536];
    ssize_t r;
    int ok = 1;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)r) != r) { ok = 0; break; }
    if (r < 0) ok = 0;
    close(in);
    if (fchown(out, uid, gid) != 0) ok = 0;
    if (close(out) != 0) ok = 0;
    if (!ok || rename(tmp, dst) != 0) { unlink(tmp); free(dst); return NULL; }
    return dst;
}

/* tvshell's files in /tmp that mpv's OSD script and subtitles need: group media may read them */
static void share_tmp(gid_t gid) {
    static const char *const pats[] = {"/tmp/notatv-*.json", "/tmp/notatv-*.bgra", "/tmp/notatv-*.m3u", "/tmp/notatv-sub-*.srt", NULL};
    for (int i = 0; pats[i]; i++) {
        glob_t g;
        if (glob(pats[i], 0, NULL, &g) != 0) continue;
        for (size_t k = 0; k < g.gl_pathc; k++) {
            int fd = open(g.gl_pathv[k], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            struct stat st;
            if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0) {
                if (fchown(fd, 0, gid) == 0) fchmod(fd, 0640);
            }
            if (fd >= 0) close(fd);
        }
        globfree(&g);
    }
}

int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    char real[300];
    if (!strcmp(name, "yt-dlp")) {
        if (!ytdlp_path(real, sizeof real)) {
            /* the line the apps show: the box fetches it by itself when it is online */
            fprintf(stderr, "ERROR: yt-dlp is still being downloaded - try again in a minute\n");
            return 127;
        }
    }
    else if (!strcmp(name, "mpv") || !strcmp(name, "ffprobe") || !strcmp(name, "uxplay")) snprintf(real, sizeof real, REAL "%s", name);
    else { warn("unknown program %s", name); return 127; }

    /* room for the arguments plus the two yt-dlp adds */
    char **nargv = calloc((size_t)argc + 3, sizeof(char *));
    if (!nargv) return 127;
    int na = 0;
    nargv[na++] = argv[0];
    int as_root = getuid() == 0;
    struct passwd *pw = getpwnam("media");
    /* only root switches (and only root can read the setting) */
    int drop = as_root && cfg_on("media_user", 1); /* on by default since audit 9/11; media_user=0 = as root */
    if (drop && !pw) { warn("media_user=1, but there is no user \"media\" - running %s as root", name); drop = 0; }
    uid_t uid = pw ? pw->pw_uid : 0;
    gid_t gid = pw ? pw->pw_gid : 0;
    if (drop) {
        /* media's home, and the data folder only traversable for others (nothing in it is readable for them) */
        struct stat st;
        if (stat(DATA, &st) == 0 && !(st.st_mode & S_IXOTH)) chmod(DATA, (st.st_mode & 07777) | S_IXGRP | S_IXOTH);
        mkdir(HOME_DIR, 0700);
        if (lstat(HOME_DIR, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_uid != uid || (st.st_mode & 077))) {
            if (chown(HOME_DIR, uid, gid) == 0) chmod(HOME_DIR, 0700);
        }
        share_tmp(gid);
    }
    int media = drop || (!as_root && pw && getuid() == pw->pw_uid); /* runs as media: switched now, or started by mpv */
    if (media && !strcmp(name, "yt-dlp")) { nargv[na++] = "--cache-dir"; nargv[na++] = HOME_DIR "/yt-dlp-cache"; }
    for (int i = 1; i < argc; i++) {
        char *a = argv[i], *r = NULL;
        if (drop) {
            if (!strncmp(a, "--log-file=", 11) && a[11]) {
                int fd = open(a + 11, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600); /* inherited by the program */
                if (fd >= 0 && fchown(fd, uid, gid) == 0 && asprintf(&r, "--log-file=/proc/self/fd/%d", fd) < 0) r = NULL;
            } else if (!strncmp(a, "--input-ipc-server=", 19) && a[19]) {
                unlink(a + 19);
            } else if (!strncmp(a, "--ytdl-raw-options-append=cookies=", 34) && a[34]) {
                char *c = stage_copy(a + 34, "cookies", uid, gid);
                if (c && asprintf(&r, "--ytdl-raw-options-append=cookies=%s", c) < 0) r = NULL;
                free(c);
            } else if (!strcmp(name, "yt-dlp") && !strncmp(a, "--cookies=", 10) && a[10]) {
                char *c = stage_copy(a + 10, "cookies", uid, gid);
                if (c && asprintf(&r, "--cookies=%s", c) < 0) r = NULL;
                free(c);
            } else if (!strcmp(name, "yt-dlp") && !strcmp(a, "--cookies") && i + 1 < argc) {
                nargv[na++] = a;
                a = argv[++i];
                r = stage_copy(a, "cookies", uid, gid);
            } else if (!strcmp(name, "uxplay") && !strcmp(a, "-reg") && i + 1 < argc) {
                nargv[na++] = a;
                a = argv[++i];
                const char *dst = HOME_DIR "/uxplay.register";
                if (strcmp(a, dst)) {
                    if (access(dst, F_OK) != 0) { char *c = stage_copy(a, "uxplay", uid, gid); if (c) { rename(c, dst); free(c); } }
                    int fd = open(dst, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600); /* UxPlay wants it to exist */
                    if (fd >= 0) { if (fchown(fd, uid, gid) != 0) warn("could not hand %s to media", dst); close(fd); }
                    r = strdup(dst);
                }
            }
        }
        nargv[na++] = r ? r : a;
    }
    nargv[na] = NULL;

    if (drop) {
        gid_t groups[3];
        int ng = 0;
        groups[ng++] = gid;
        struct group *g;
        if ((g = getgrnam("video"))) groups[ng++] = g->gr_gid;
        if ((g = getgrnam("audio"))) groups[ng++] = g->gr_gid;
        int sig = 0;
        prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0); /* cleared by the user switch; UxPlay must still die with tvshell */
        if (setgroups((size_t)ng, groups) != 0 || setgid(gid) != 0 || setuid(uid) != 0 || getuid() != uid || geteuid() != uid) {
            warn("could not switch to the user media (%s)", strerror(errno));
            return 126; /* never fall back to root here: something is wrong */
        }
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (sig) prctl(PR_SET_PDEATHSIG, sig, 0, 0, 0);
        setenv("HOME", HOME_DIR, 1);
        setenv("XDG_CACHE_HOME", HOME_DIR "/.cache", 1);
        setenv("XDG_CONFIG_HOME", HOME_DIR "/.config", 1);
        unsetenv("XDG_RUNTIME_DIR"); /* root's (0700) */
        umask(077);
    }
    execv(real, nargv);
    warn("cannot start %s", real);
    return 127;
}
