/* NotaTV app package "radio" - Radio: stations from radio-browser.info (free, open, no login), plays in the
 * background while you use the rest of the box.
 * - The stations offered first follow the box's country (cfg tz_country from the setup guide); another country can be
 *   chosen in the app (cfg radio_country). Genres, search, favourites (Yellow), recently played.
 * - Playback: our own mpv without video (audio only), controlled over its IPC socket. The Pi's HDMI audio takes one
 *   player at a time, so the system stops the radio before video or game streaming (audio_bg_set, NotaTV 1.0.7+).
 * - "Now playing" box in the top bar while you are elsewhere, the station on the home screen's big top, and the
 *   home row "Radio" = recently played (OK plays at once).
 * Built as app.so (see src/apps/README.md); the descriptor nota_app is at the bottom. */
#include "tv.h"
#include "vendor/cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

extern const NotaApp nota_app;

#define RD_ACCENT RGB(255, 150, 40)
#define SOCK_PATH "/tmp/notatv-radio.sock"
#define MAX_ST 150
#define MAX_FAV 60
#define MAX_RECENT 20
#define TILE 200
#define GCOLS 7
#define CELL_W (TILE + 36)
#define CELL_H (TILE + 96)

/* ---------- stations ---------- */
typedef struct {
    char uuid[40], name[96], url[400], logo[400], tags[96], cc[4];
    int bitrate;
} Station;

static void copy_s(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

/* tidy names from the database: no surrounding/double spaces */
static void clean_name(char *s) {
    char *w = s;
    bool sp = true;
    for (char *r = s; *r; r++) {
        bool is_sp = isspace((unsigned char)*r);
        if (is_sp && sp) continue;
        *w++ = is_sp ? ' ' : *r;
        sp = is_sp;
    }
    if (w > s && w[-1] == ' ') w--;
    *w = 0;
}

/* the first two tags, nicer to read: "pop, rock" */
static void short_tags(const char *tags, char *out, size_t n) {
    out[0] = 0;
    if (!tags) return;
    int k = 0;
    const char *p = tags;
    while (*p && k < 2) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0 && len < 30) {
            size_t ol = strlen(out);
            snprintf(out + ol, n - ol, "%s%.*s", k ? ", " : "", (int)len, p);
            k++;
        }
        if (!e) break;
        p = e + 1;
    }
}

static bool station_from_json(const cJSON *o, Station *s) {
    memset(s, 0, sizeof *s);
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(o, "url_resolved"));
    if (!url || !*url) url = cJSON_GetStringValue(cJSON_GetObjectItem(o, "url"));
    const char *uuid = cJSON_GetStringValue(cJSON_GetObjectItem(o, "stationuuid"));
    if (!url || !*url || !uuid) return false;
    /* only network streams: mpv must never be given a local file or another protocol from the database */
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return false;
    copy_s(s->uuid, sizeof s->uuid, uuid);
    copy_s(s->url, sizeof s->url, url);
    copy_s(s->name, sizeof s->name, cJSON_GetStringValue(cJSON_GetObjectItem(o, "name")));
    clean_name(s->name);
    if (!s->name[0]) return false;
    const char *fav = cJSON_GetStringValue(cJSON_GetObjectItem(o, "favicon"));
    if (fav && (!strncmp(fav, "http://", 7) || !strncmp(fav, "https://", 8))) copy_s(s->logo, sizeof s->logo, fav);
    /* "tags" from the API is a long list; saved stations (our own file) already have the short form */
    short_tags(cJSON_GetStringValue(cJSON_GetObjectItem(o, "tags")), s->tags, sizeof s->tags);
    copy_s(s->cc, sizeof s->cc, cJSON_GetStringValue(cJSON_GetObjectItem(o, "countrycode")));
    cJSON *br = cJSON_GetObjectItem(o, "bitrate");
    s->bitrate = cJSON_IsNumber(br) ? br->valueint : 0;
    return true;
}

static cJSON *station_to_json(const Station *s) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "stationuuid", s->uuid);
    cJSON_AddStringToObject(o, "name", s->name);
    cJSON_AddStringToObject(o, "url_resolved", s->url);
    cJSON_AddStringToObject(o, "favicon", s->logo);
    cJSON_AddStringToObject(o, "tags", s->tags);
    cJSON_AddStringToObject(o, "countrycode", s->cc);
    cJSON_AddNumberToObject(o, "bitrate", s->bitrate);
    return o;
}

/* the same station is often in the database several times ("DR P1", "DR P1 (MP3)", other servers): the key is the
   name in lowercase letters and digits, without the format */
static void name_key(const char *name, char *out, size_t n) {
    char tmp[96];
    size_t k = 0;
    for (const char *p = name; *p && k + 1 < sizeof tmp; p++)
        if (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80) tmp[k++] = (char)tolower((unsigned char)*p);
    tmp[k] = 0;
    static const char *const drop[] = {"mp3", "aac", "ogg", "hls", "128", "320", "192", "256", "64", "hq", "lq", NULL};
    bool cut = true;
    while (cut) {
        cut = false;
        for (int i = 0; drop[i]; i++) {
            size_t dl = strlen(drop[i]), tl = strlen(tmp);
            if (tl > dl + 2 && !strcmp(tmp + tl - dl, drop[i])) { tmp[tl - dl] = 0; cut = true; }
        }
    }
    copy_s(out, n, tmp);
}

/* ---------- favourites and recently played (app data folder, radio.json) ---------- */
static Station favs[MAX_FAV], recents[MAX_RECENT];
static int n_fav, n_recent;
static bool store_loaded;

static void store_path(char *p, size_t n) { snprintf(p, n, "%s/radio.json", app_data(&nota_app)); }

static void store_load(void) {
    if (store_loaded) return;
    store_loaded = true;
    char p[300];
    store_path(p, sizeof p);
    char *txt = file_read_all(p, 1 << 20);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    cJSON *o;
    n_fav = n_recent = 0;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(j, "fav"))
        if (n_fav < MAX_FAV && station_from_json(o, &favs[n_fav])) {
            copy_s(favs[n_fav].tags, sizeof favs[n_fav].tags, cJSON_GetStringValue(cJSON_GetObjectItem(o, "tags")));
            n_fav++;
        }
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(j, "recent"))
        if (n_recent < MAX_RECENT && station_from_json(o, &recents[n_recent])) {
            copy_s(recents[n_recent].tags, sizeof recents[n_recent].tags, cJSON_GetStringValue(cJSON_GetObjectItem(o, "tags")));
            n_recent++;
        }
    cJSON_Delete(j);
}

static void store_save(void) {
    cJSON *j = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(j, "fav");
    for (int i = 0; i < n_fav; i++) cJSON_AddItemToArray(a, station_to_json(&favs[i]));
    a = cJSON_AddArrayToObject(j, "recent");
    for (int i = 0; i < n_recent; i++) cJSON_AddItemToArray(a, station_to_json(&recents[i]));
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return;
    char p[300], tmp[310];
    store_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(txt, f);
        if (fclose(f) == 0) rename(tmp, p);
        else unlink(tmp);
    }
    free(txt);
}

static int fav_index(const char *uuid) {
    for (int i = 0; i < n_fav; i++) if (!strcmp(favs[i].uuid, uuid)) return i;
    return -1;
}

static void fav_toggle(const Station *s) {
    store_load();
    int i = fav_index(s->uuid);
    if (i >= 0) {
        memmove(&favs[i], &favs[i + 1], sizeof(Station) * (n_fav - i - 1));
        n_fav--;
        app_toast(T("Removed from favourites"));
    } else {
        if (n_fav == MAX_FAV) { app_toast(T("You can have up to %d favourites"), MAX_FAV); return; }
        favs[n_fav++] = *s;
        app_toast(T("Added to favourites as number %d"), n_fav);
    }
    store_save();
}

static void recent_add(const Station *s) {
    store_load();
    int i;
    for (i = 0; i < n_recent; i++) if (!strcmp(recents[i].uuid, s->uuid)) break;
    if (i == n_recent && n_recent < MAX_RECENT) n_recent++;
    if (i >= MAX_RECENT) i = MAX_RECENT - 1;
    memmove(&recents[1], &recents[0], sizeof(Station) * i);
    recents[0] = *s;
    store_save();
}

/* ---------- radio-browser.info ---------- */
/* all.api is round robin over the servers; if it does not answer, two of the servers directly */
static const char *const rb_hosts[] = {"https://all.api.radio-browser.info", "https://de1.api.radio-browser.info",
                                       "https://fi1.api.radio-browser.info", NULL};

static cJSON *rb_get(const char *path) {
    for (int i = 0; rb_hosts[i]; i++) {
        char *url = str_printf("%s%s", rb_hosts[i], path);
        HttpBuf b = {0};
        bool ok = http_request("GET", url, NULL, NULL, &b, 12);
        free(url);
        if (ok && b.status == 200 && b.data) {
            cJSON *j = cJSON_Parse(b.data);
            http_free(&b);
            if (j) return j;
        } else http_free(&b);
    }
    return NULL;
}

/* the box's country for radio: the one chosen in the app, otherwise the country from the setup guide */
static const char *radio_cc(void) {
    const char *c = cfg_get("radio_country", "");
    if (*c) return c;
    return cfg_get("tz_country", "");
}
static const char *cc_name(const char *cc, const char *fallback) {
    const char *n = *cc ? tz_country_name(cc) : NULL;
    if (n && strcmp(n, cc)) return n;
    return fallback && *fallback ? fallback : cc;
}

/* ---------- playback (mpv without video) ---------- */
static struct {
    pid_t pid;
    int fd;              /* IPC socket, -1 = not connected */
    char buf[8192];
    size_t blen;
    double started, retry_at;
    bool stopping, audio;  /* audio: mpv has played (core-idle went false) */
    bool buffering;
    int retries;
    Station st;          /* the station that plays (st.uuid[0] = nothing) */
    char title[200];     /* "Artist - Track" from the stream (ICY) */
} pl = {.fd = -1};

static bool rd_playing(void) { return pl.st.uuid[0] != 0; } /* also while it connects again after a dropout */

static void ipc_close(void) {
    if (pl.fd >= 0) close(pl.fd);
    pl.fd = -1;
    pl.blen = 0;
}

/* stop mpv and wait until it is gone (the audio device must be free when this returns) */
static void mpv_kill(void) {
    if (pl.pid <= 0) return;
    ipc_close();
    kill(pl.pid, SIGTERM);
    for (int i = 0; i < 60; i++) {
        pid_t r = waitpid(pl.pid, NULL, WNOHANG);
        if (r == pl.pid || (r < 0 && errno == ECHILD)) { pl.pid = 0; break; }
        SDL_Delay(20);
    }
    if (pl.pid > 0) {
        kill(pl.pid, SIGKILL);
        waitpid(pl.pid, NULL, 0);
        pl.pid = 0;
    }
    unlink(SOCK_PATH);
}

static void rd_stop(void) {
    mpv_kill();
    pl.st.uuid[0] = 0;
    pl.title[0] = 0;
    pl.audio = pl.buffering = false;
    pl.retry_at = 0;
    if (audio_bg_set) audio_bg_set(NULL);
}
/* the system stops us before video/game streaming (audio_bg_set) */
static void rd_stop_for_video(void) {
    SDL_Log("radio: stoppet for video");
    rd_stop();
}

static bool mpv_spawn(const char *url) {
    char *argv[16];
    int n = 0;
    argv[n++] = "mpv";
    argv[n++] = "--no-config"; /* not the video player's mpv.conf/scripts: plain audio */
    argv[n++] = "--no-video";
    argv[n++] = "--no-terminal";
    argv[n++] = "--idle=no";
    argv[n++] = "--input-ipc-server=" SOCK_PATH;
    argv[n++] = "--cache=yes";
    argv[n++] = "--demuxer-max-bytes=4MiB";
    argv[n++] = "--network-timeout=15";
    char *adev = NULL;
    const char *ad = cfg_get("audio_device", "");
    if (*ad) argv[n++] = adev = str_printf("--audio-device=%s", ad);
    argv[n++] = "--";
    argv[n++] = (char *)url;
    argv[n] = NULL;
    unlink(SOCK_PATH);
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM); /* the radio stops if tvshell goes away */
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execvp("mpv", argv);
        _exit(127);
    }
    free(adev);
    if (pid < 0) return false;
    pl.pid = pid;
    pl.started = app_time();
    return true;
}

static void click_work(void *arg) { /* radio-browser counts plays (that is how "popular" is sorted) */
    char *p = str_printf("/json/url/%s", (char *)arg);
    cJSON_Delete(rb_get(p));
    free(p);
}
static void click_done(void *arg) { free(arg); }

static void rd_play(const Station *s) {
    mpv_kill();
    pl.st = *s;
    pl.title[0] = 0;
    pl.audio = false;
    pl.buffering = true;
    pl.retries = 0;
    pl.retry_at = 0;
    if (!mpv_spawn(s->url)) {
        app_toast(T("Could not start the radio"));
        pl.st.uuid[0] = 0;
        return;
    }
    if (audio_bg_set) audio_bg_set(rd_stop_for_video);
    SDL_Log("radio: spiller %s", s->name);
    job_submit(click_work, click_done, str_dup(s->uuid));
}

static void ipc_send(const char *line) {
    if (pl.fd < 0) return;
    size_t len = strlen(line);
    if (write(pl.fd, line, len) != (ssize_t)len) ipc_close();
}

static void ipc_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    copy_s(a.sun_path, sizeof a.sun_path, SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return; }
    pl.fd = fd;
    pl.blen = 0;
    ipc_send("{\"command\":[\"observe_property\",1,\"metadata\"]}\n"
             "{\"command\":[\"observe_property\",2,\"core-idle\"]}\n");
}

static void meta_title(const cJSON *md) {
    const char *t = NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, md) /* "icy-title" (Shoutcast/Icecast), sometimes "title" or "TITLE" */
        if (cJSON_IsString(e) && e->string && (!strcasecmp(e->string, "icy-title") || !strcasecmp(e->string, "title"))) {
            if (!t || !strcasecmp(e->string, "icy-title")) t = e->valuestring;
        }
    char tmp[200];
    const char *b = t ? t : "";
    while (*b == '/' || *b == '-' || *b == '|' || isspace((unsigned char)*b)) b++; /* DR sends "/ Artist - Track" */
    copy_s(tmp, sizeof tmp, b);
    clean_name(tmp);
    /* some stations send their own name, a URL or just " - " as the title */
    if (!strcmp(tmp, "-") || !strcasecmp(tmp, pl.st.name) || strstr(tmp, "://")) tmp[0] = 0;
    copy_s(pl.title, sizeof pl.title, tmp);
}

static void ipc_line(const char *line) {
    cJSON *j = cJSON_Parse(line);
    if (!j) return;
    const char *ev = cJSON_GetStringValue(cJSON_GetObjectItem(j, "event"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    if (ev && name && !strcmp(ev, "property-change")) {
        cJSON *d = cJSON_GetObjectItem(j, "data");
        if (!strcmp(name, "metadata")) meta_title(d);
        else if (!strcmp(name, "core-idle")) {
            bool idle = !cJSON_IsFalse(d);
            pl.buffering = idle;
            if (!idle && !pl.audio) {
                pl.audio = true;
                recent_add(&pl.st);
            }
            if (!idle) pl.retries = 0;
        }
    }
    cJSON_Delete(j);
}

static void ipc_read(void) {
    if (pl.fd < 0) return;
    for (;;) {
        if (pl.blen >= sizeof pl.buf - 1) pl.blen = 0; /* a line that long is not ours */
        ssize_t r = read(pl.fd, pl.buf + pl.blen, sizeof pl.buf - 1 - pl.blen);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { ipc_close(); return; }
        if (r < 0) return;
        pl.blen += (size_t)r;
        pl.buf[pl.blen] = 0;
        char *start = pl.buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            ipc_line(start);
            start = nl + 1;
        }
        pl.blen -= (size_t)(start - pl.buf);
        memmove(pl.buf, start, pl.blen);
    }
}

static bool box_sleeping(void) {
    struct stat sb;
    return stat("/run/notatv/sleeping", &sb) == 0;
}

static void radio_tick(void) {
    if (pl.pid <= 0) {
        if (pl.st.uuid[0] && pl.retry_at && app_time() >= pl.retry_at) { /* the stream dropped: connect again */
            pl.retry_at = 0;
            if (!mpv_spawn(pl.st.url)) rd_stop();
            else if (audio_bg_set) audio_bg_set(rd_stop_for_video);
        }
        return;
    }
    /* older NotaTV without audio_bg_set: stop when something else plays video; and always when the box sleeps */
    double pp, pd;
    bool pz;
    if ((!audio_bg_set && player_state(&pp, &pd, &pz)) || box_sleeping()) { rd_stop(); return; }
    if (pl.fd < 0 && app_time() - pl.started < 10) ipc_connect();
    ipc_read();
    int status = 0;
    pid_t r = waitpid(pl.pid, &status, WNOHANG);
    if (r == pl.pid || (r < 0 && errno == ECHILD)) {
        pl.pid = 0;
        ipc_close();
        if (!pl.st.uuid[0]) return;
        if (pl.audio && pl.retries < 3) { /* it played, then the stream dropped: try again in a moment */
            pl.retries++;
            pl.buffering = true;
            pl.retry_at = app_time() + 2;
            SDL_Log("radio: forbindelsen røg - prøver igen (%d)", pl.retries);
            return;
        }
        app_toast(T("%s is not answering right now"), pl.st.name);
        rd_stop();
    }
}

/* ---------- drawing helpers ---------- */
static Color name_color(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    float hue = (h % 360) / 60.0f, x = 1 - fabsf(fmodf(hue, 2) - 1);
    float r = 0, g = 0, b = 0;
    int k = (int)hue;
    if (k == 0) { r = 1; g = x; } else if (k == 1) { r = x; g = 1; } else if (k == 2) { g = 1; b = x; }
    else if (k == 3) { g = x; b = 1; } else if (k == 4) { r = x; b = 1; } else { r = 1; b = x; }
    /* dark and not too saturated, so white text stands on it */
    return RGB((uint8_t)(40 + 90 * r), (uint8_t)(40 + 90 * g), (uint8_t)(40 + 90 * b));
}

static Image *logo_img(const Station *s) {
    if (!s->logo[0]) return NULL;
    return img_get_logo ? img_get_logo(s->logo) : img_get(s->logo);
}

/* the station's logo on a light card (logos are made for white web pages), or its name on a colored card */
static void draw_logo(const Station *s, float x, float y, float w, float h, float r, float a) {
    Image *im = logo_img(s);
    if (im && img_state(im) == IMG_READY) {
        fill_rrect(x, y, w, h, r, color_alpha(RGB(236, 238, 242), a));
        float pad = w * 0.12f;
        img_draw(im, x + pad, y + pad, w - 2 * pad, h - 2 * pad, 0, a);
        return;
    }
    Color c = name_color(s->name);
    fill_rrect_vgrad(x, y, w, h, r, color_alpha(color_mix(c, RGB(255, 255, 255), 0.12f), a), color_alpha(c, a));
    if (w < 90) { /* small: the first letter */
        char l[8] = {0};
        int k = 0;
        for (const unsigned char *p = (const unsigned char *)s->name; *p && k < 4; p++) {
            if (k && (*p & 0xC0) != 0x80) break;
            l[k++] = (char)*p;
        }
        text_draw(F_BOLD, h * 0.45f, x + w / 2, y + h * 0.24f, color_alpha(C_TEXT, a), AL_CENTER, l);
        return;
    }
    float fs = w > 150 ? 30 : 22;
    text_draw_wrap(F_BOLD, fs, x + 14, y + h / 2 - fs * 1.2f, w - 28, 3, fs * 1.2f, color_alpha(C_TEXT, a), s->name);
}

static void draw_bars(float x, float base, float a, bool moving) {
    double now = app_time();
    for (int i = 0; i < 3; i++) {
        float bh = moving ? 6 + 14 * (0.5f + 0.5f * sinf((float)now * (5.0f + i * 1.3f) + i * 1.7f)) : 5;
        fill_rrect(x + i * 8, base - bh, 4, bh, 2, color_alpha(RD_ACCENT, a));
    }
}

/* ---------- burn-in protection (Settings → Apps → Radio, on by default like Music) ----------
   The radio can play for hours with the same screen: nothing may stand still. */
static bool oled_on(void) {
    static int gen = -1;
    static bool on;
    if (gen != cfg_gen()) { gen = cfg_gen(); on = cfg_int("radio_oled", 1) != 0; }
    return on;
}
/* a slow orbit of at most amp x 0.7*amp px, under 0.5 px/s. With gfx_drift it glides (fractions of a pixel); on an
   older NotaTV without it, it is rounded to whole pixels */
static void oled_offset(float amp, float px, float py, float *ox, float *oy) {
    float t = (float)app_time();
    *ox = amp * sinf(t * 6.2831853f / px);
    *oy = amp * 0.7f * sinf(t * 6.2831853f / py + 1);
}
/* everything drawn until view_shift(0, 0) is moved (the clip rectangles move along) */
static void view_shift(int ox, int oy) {
    SDL_Rect r = {ox, oy, UI_W, UI_H};
    SDL_SetRenderViewport(gfx_renderer(), ox || oy ? &r : NULL);
}

/* ---------- screen ---------- */
enum { V_POP, V_FAV, V_RECENT, V_GENRES, V_TAG, V_COUNTRIES, V_SEARCH };
enum { K_STATION, K_GENRE, K_COUNTRY };
typedef struct {
    int kind;
    Station s;          /* K_STATION */
    char label[96];     /* K_GENRE / K_COUNTRY */
    char key[64];       /* tag / country code */
    int count;          /* K_COUNTRY: number of stations */
} Tile;

static const struct { const char *tag, *label; } genres[] = {
    {"pop", N_("Pop")}, {"rock", N_("Rock")}, {"news", N_("News")}, {"talk", N_("Talk")},
    {"jazz", N_("Jazz")}, {"classical", N_("Classical")}, {"dance", N_("Dance")}, {"electronic", N_("Electronic")},
    {"hiphop", N_("Hip hop")}, {"80s", N_("80s")}, {"90s", N_("90s")}, {"oldies", N_("Oldies")},
    {"country", N_("Country")}, {"chillout", N_("Chill")}, {"christmas", N_("Christmas")}, {"sport", N_("Sport")},
};
#define N_GENRES (int)(sizeof genres / sizeof genres[0])

typedef struct {
    int view;
    Tile *t;
    int n;
    bool loading;
    char msg[160], heading[160], tag[64];
    int row, col;
    float sy;
    SideMenu side;
    double opened;  /* burn-in: the key hints go away 30 s after the last key press */
    float dim;      /* burn-in: the grid dims while you only listen */
} Rd;

static Rd *live;
static int rd_gen; /* replies for an old view are ignored */

typedef struct {
    int gen;
    int view;
    char path[400], path2[400];  /* path2: a second search appended (genre: the whole world after the country) */
    char cc[4];                  /* search: these first */
    Tile *t;
    int n;
    bool failed;
} Fetch;

static void fetch_work(void *arg) {
    Fetch *f = arg;
    f->t = calloc(MAX_ST, sizeof(Tile));
    if (!f->t) { f->failed = true; return; }
    char keys[MAX_ST][96];
    for (int pass = 0; pass < 2; pass++) {
        const char *path = pass ? f->path2 : f->path;
        if (!*path || (pass && f->n >= 12)) break;
        cJSON *j = rb_get(path);
        if (!j) { if (!pass) f->failed = true; break; }
        cJSON *o;
        cJSON_ArrayForEach(o, j) {
            if (f->n >= MAX_ST) break;
            Tile *t = &f->t[f->n];
            memset(t, 0, sizeof *t);
            if (f->view == V_COUNTRIES) {
                const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(o, "iso_3166_1"));
                cJSON *sc = cJSON_GetObjectItem(o, "stationcount");
                int cnt = cJSON_IsNumber(sc) ? sc->valueint : 0;
                if (!code || strlen(code) != 2 || cnt < 10) continue;
                t->kind = K_COUNTRY;
                copy_s(t->key, sizeof t->key, code);
                copy_s(t->label, sizeof t->label, cc_name(code, cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"))));
                t->count = cnt;
                f->n++;
                continue;
            }
            if (!station_from_json(o, &t->s)) continue;
            t->kind = K_STATION;
            name_key(t->s.name, keys[f->n], sizeof keys[f->n]);
            bool dup = false;
            for (int k = 0; k < f->n && !dup; k++) dup = !strcmp(keys[k], keys[f->n]);
            if (!dup) f->n++;
        }
        cJSON_Delete(j);
        f->failed = false;
    }
    if (f->view == V_COUNTRIES) { /* alphabetical by the shown name */
        for (int i = 1; i < f->n; i++)
            for (int k = i; k > 0 && strcasecmp(f->t[k - 1].label, f->t[k].label) > 0; k--) {
                Tile tmp = f->t[k]; f->t[k] = f->t[k - 1]; f->t[k - 1] = tmp;
            }
        for (int i = 1; i < f->n; i++) /* the box's own country first */
            if (!strcasecmp(f->t[i].key, f->cc)) {
                Tile tmp = f->t[i];
                memmove(&f->t[1], &f->t[0], sizeof(Tile) * i);
                f->t[0] = tmp;
                break;
            }
    }
    if (f->view == V_SEARCH && f->cc[0]) { /* stations from your own country first (stable) */
        int w = 0;
        for (int i = 0; i < f->n; i++)
            if (!strcasecmp(f->t[i].s.cc, f->cc)) {
                Tile tmp = f->t[i];
                memmove(&f->t[w + 1], &f->t[w], sizeof(Tile) * (i - w));
                f->t[w++] = tmp;
            }
    }
}

static void fetch_done(void *arg) {
    Fetch *f = arg;
    Rd *v = live;
    if (v && f->gen == rd_gen) {
        v->loading = false;
        free(v->t);
        v->t = f->t;
        v->n = f->n;
        f->t = NULL;
        if (f->failed && !v->n) snprintf(v->msg, sizeof v->msg, "%s", T("Cannot reach the radio directory - check the network"));
    }
    free(f->t);
    free(f);
}

static void set_view(Rd *v, int view, const char *heading) {
    rd_gen++;
    free(v->t);
    v->t = NULL;
    v->n = 0;
    v->row = v->col = 0;
    v->sy = 0;
    v->view = view;
    v->loading = false;
    v->msg[0] = 0;
    copy_s(v->heading, sizeof v->heading, heading);
}

static void fetch(Rd *v, const char *path, const char *path2) {
    Fetch *f = calloc(1, sizeof *f);
    if (!f) return;
    f->gen = rd_gen;
    f->view = v->view;
    copy_s(f->path, sizeof f->path, path);
    copy_s(f->path2, sizeof f->path2, path2);
    copy_s(f->cc, sizeof f->cc, v->view == V_COUNTRIES ? cfg_get("tz_country", "") : radio_cc());
    v->loading = true;
    job_submit(fetch_work, fetch_done, f);
}

#define ST_Q "hidebroken=true&order=clickcount&reverse=true&limit=120"

static void show_pop(Rd *v) {
    const char *cc = radio_cc();
    char h[160], p[200];
    if (*cc) {
        snprintf(h, sizeof h, T("Popular in %s"), cc_name(cc, NULL));
        snprintf(p, sizeof p, "/json/stations/search?countrycode=%s&" ST_Q, cc);
    } else {
        snprintf(h, sizeof h, "%s", T("Popular worldwide"));
        snprintf(p, sizeof p, "/json/stations/topclick/120?hidebroken=true");
    }
    set_view(v, V_POP, h);
    fetch(v, p, "");
}

static void show_list(Rd *v, int view) {
    store_load();
    set_view(v, view, view == V_FAV ? T("Favourites") : T("Recently played"));
    const Station *src = view == V_FAV ? favs : recents;
    int n = view == V_FAV ? n_fav : n_recent;
    v->t = calloc(n ? n : 1, sizeof(Tile));
    for (int i = 0; v->t && i < n; i++) { v->t[i].kind = K_STATION; v->t[i].s = src[i]; }
    v->n = v->t ? n : 0;
}

static void show_genres(Rd *v) {
    set_view(v, V_GENRES, T("Genres"));
    v->t = calloc(N_GENRES, sizeof(Tile));
    for (int i = 0; v->t && i < N_GENRES; i++) {
        v->t[i].kind = K_GENRE;
        copy_s(v->t[i].key, sizeof v->t[i].key, genres[i].tag);
        copy_s(v->t[i].label, sizeof v->t[i].label, T(genres[i].label));
    }
    v->n = v->t ? N_GENRES : 0;
}

static void show_tag(Rd *v, const char *tag, const char *label) {
    char *e = url_encode(tag);
    const char *cc = radio_cc();
    char p[300], p2[300];
    if (*cc) snprintf(p, sizeof p, "/json/stations/search?tag=%s&countrycode=%s&" ST_Q, e, cc);
    else snprintf(p, sizeof p, "/json/stations/search?tag=%s&" ST_Q, e);
    /* few in your own country: the rest of the world after them */
    if (*cc) snprintf(p2, sizeof p2, "/json/stations/search?tag=%s&" ST_Q, e);
    else p2[0] = 0;
    free(e);
    set_view(v, V_TAG, label);
    copy_s(v->tag, sizeof v->tag, tag);
    fetch(v, p, p2);
}

static void show_countries(Rd *v) {
    set_view(v, V_COUNTRIES, T("Choose a country"));
    fetch(v, "/json/countries?order=stationcount&reverse=true&hidebroken=true", "");
}

static void search_done(void *ud, const char *q) {
    Rd *v = ud;
    if (!q || v != live) return;
    char *t = str_dup(q);
    str_trim(t);
    if (*t) {
        char *e = url_encode(t), *p = str_printf("/json/stations/search?name=%s&" ST_Q, e), *h = str_printf(T("Search: %s"), t);
        set_view(v, V_SEARCH, h);
        fetch(v, p, "");
        free(e); free(p); free(h);
    }
    free(t);
}
static void open_search(Rd *v, const char *init) { keyboard_open(T("Search radio stations"), init, false, search_done, v); }

enum { M_FAV, M_POP, M_RECENT, M_GENRES, M_COUNTRY, M_SEARCH, M_SETTINGS };
static void build_side(Rd *v) {
    side_clear(&v->side);
    const char *cc = radio_cc();
    copy_s(v->side.subtitle, sizeof v->side.subtitle, *cc ? cc_name(cc, NULL) : T("Whole world"));
    side_add(&v->side, T("Favourites"), ICON_STAR, false); /* first: what you listen to (STATUS 100) */
    side_add(&v->side, T("Popular"), ICON_MUSIC, false);
    side_add(&v->side, T("Recently played"), ICON_CLOCK, false);
    side_add(&v->side, T("Genres"), ICON_FOLDER, false);
    side_add(&v->side, T("Change country"), ICON_TV, true);
    side_add(&v->side, T("Search"), ICON_SEARCH, false);
    if (app_settings_open) side_add(&v->side, T("Settings"), ICON_GEAR, true); /* the app's settings, right here */
}

static void side_activate(Rd *v, int item) {
    if (item != M_SEARCH && item != M_SETTINGS) v->side.active = item;
    switch (item) {
    case M_POP: show_pop(v); break;
    case M_FAV: show_list(v, V_FAV); break;
    case M_RECENT: show_list(v, V_RECENT); break;
    case M_GENRES: show_genres(v); break;
    case M_COUNTRY: show_countries(v); break;
    case M_SEARCH: open_search(v, ""); break;
    case M_SETTINGS: app_settings_open("radio"); break;
    default: break;
    }
}

static void choose_country(Rd *v, const char *code) {
    /* the box's own country: follow it again (also if it is changed later in Settings) */
    if (!strcmp(code, cfg_get("tz_country", ""))) cfg_unset("radio_country");
    else cfg_set("radio_country", code);
    build_side(v);
    v->side.active = M_POP;
    show_pop(v);
}

static void rd_enter(Screen *s) {
    Rd *v = s->st;
    live = v;
    v->opened = app_time();
    store_load();
    side_init(&v->side, T("Radio"), ICON_MUSIC, RD_ACCENT);
    build_side(v);
    /* the first time: popular in your country; with favourites: them */
    if (n_fav) { v->side.active = M_FAV; show_list(v, V_FAV); }
    else { v->side.active = M_POP; show_pop(v); }
}
static void rd_resume(Screen *s) { live = s->st; }
static void rd_destroy(Screen *s) {
    Rd *v = s->st;
    if (live == v) live = NULL;
    rd_gen++;
    free(v->t);
    free(v);
    free(s);
}

static int rows(const Rd *v) { return (v->n + GCOLS - 1) / GCOLS; }
static void rd_update(Screen *s, float dt) {
    Rd *v = s->st;
    side_update(&v->side, dt);
    float t = (v->row - 1) * CELL_H;
    v->sy = anim_to(v->sy, t > 0 ? t : 0, dt, 12);
    /* burn-in: 60 s without a key press while the radio plays - the stations dim, only what plays stays bright */
    double idle = app_time() - fmax(input_last_activity(), v->opened);
    bool want = oled_on() && rd_playing() && idle > 60 && !v->side.open;
    v->dim = anim_to(v->dim, want ? 1 : 0, dt, want ? 0.6f : 8);
}

static void draw_tile(const Rd *v, const Tile *t, float x, float y, bool f) {
    float fa = ui_focus(t, f), sc = 1 + 0.07f * fa, w = TILE * sc, h = TILE * sc, xx = x - (w - TILE) / 2, yy = y - (h - TILE) / 2;
    ui_focus_under(xx, yy, w, h, 16, fa);
    if (t->kind == K_STATION) {
        draw_logo(&t->s, xx, yy, w, h, 16, 1);
        bool now = rd_playing() && !strcmp(pl.st.uuid, t->s.uuid);
        if (now) { /* this one plays: dark corner with the bars */
            fill_rrect(xx + w - 54, yy + h - 40, 44, 30, 8, RGBA(0, 0, 0, 170));
            draw_bars(xx + w - 44, yy + h - 15, 1, !pl.buffering);
        }
        int fi = fav_index(t->s.uuid);
        if (fi >= 0) { /* favourite: its number (the digit keys play it) */
            char num[8];
            snprintf(num, sizeof num, "%d", fi + 1);
            fill_circle(xx + 24, yy + 24, 16, RGBA(0, 0, 0, 170));
            if (fi < 9) text_draw(F_BOLD, 20, xx + 24, yy + 12, RD_ACCENT, AL_CENTER, num);
            else draw_icon(ICON_STAR, xx + 24, yy + 24, 18, RD_ACCENT);
        }
    } else {
        Color c = name_color(t->label);
        fill_rrect_vgrad(xx, yy, w, h, 16, color_mix(c, RGB(255, 255, 255), 0.15f), c);
        text_draw_wrap(F_BOLD, 26, xx + 16, yy + h / 2 - 32, w - 32, 3, 32, C_TEXT, t->label);
    }
    ui_focus_over(xx, yy, w, h, 16, fa);
    const char *name = t->kind == K_STATION ? t->s.name : t->label;
    char sub[128] = "";
    if (t->kind == K_STATION) {
        if (v->view != V_POP && t->s.cc[0] && strcasecmp(t->s.cc, radio_cc())) snprintf(sub, sizeof sub, "%s%s%s", t->s.cc, t->s.tags[0] ? " · " : "", t->s.tags);
        else copy_s(sub, sizeof sub, t->s.tags);
    } else if (t->kind == K_COUNTRY) snprintf(sub, sizeof sub, T("%d stations"), t->count);
    if (t->kind != K_GENRE) text_draw_clip(F_SEMI, 22, x, y + TILE + 14, TILE, f ? C_TEXT : C_TEXT2, AL_LEFT, name);
    if (sub[0]) text_draw_clip(F_REG, 19, x, y + TILE + 44, TILE, C_TEXT3, AL_LEFT, sub);
}

/* what plays, at the right of the heading */
static void draw_now(float right, float y) {
    if (!rd_playing()) return;
    const char *line2 = pl.buffering ? T("Connecting…") : pl.title[0] ? pl.title : T("Live");
    float tw = text_width(F_SEMI, 24, pl.st.name), sw = text_width(F_REG, 20, line2);
    float textw = tw > sw ? tw : sw;
    if (textw > 520) textw = 520;
    float h = 76, w = 10 + 56 + 16 + textw + 20 + 22 + 20, x = right - w;
    fill_rrect(x, y, w, h, h / 2, RGBA(255, 255, 255, 18));
    draw_logo(&pl.st, x + 10, y + 10, 56, 56, 28, 1);
    text_draw_clip(F_SEMI, 24, x + 82, y + 10, textw, C_TEXT, AL_LEFT, pl.st.name);
    text_draw_clip(F_REG, 20, x + 82, y + 42, textw, C_TEXT2, AL_LEFT, line2);
    if (pl.buffering) draw_spinner(x + w - 38, y + h / 2, 10, C_TEXT2);
    else draw_bars(x + w - 46, y + h / 2 + 10, 1, true);
}

static const Tile *focused(const Rd *v) {
    int i = v->row * GCOLS + v->col;
    return i >= 0 && i < v->n ? &v->t[i] : NULL;
}

static void rd_draw(Screen *s) {
    Rd *v = s->st;
    draw_background();
    bool oled = oled_on();
    float ox = 0, oy = 0;
    if (oled) oled_offset(14, 173, 127, &ox, &oy);
    bool drift = oled && gfx_drift_begin && gfx_drift_begin(0, 0, UI_W, UI_H);
    if (oled && !drift) view_shift((int)lroundf(ox), (int)lroundf(oy));
    draw_topbar(NULL);
    float x0 = SIDE_W + 60, cx = x0 + (UI_W - x0) / 2;
    bool cf = !v->side.open;
    clip_push(SIDE_W, 120, UI_W, UI_H - 120);
    float top = 200 - v->sy;
    text_draw(F_BOLD, 44, x0, top - 50, C_TEXT, AL_LEFT, v->heading);
    if (v->loading && !v->n) draw_spinner(cx, UI_H / 2, 30, C_TEXT);
    else if (!v->n && v->msg[0]) { /* an error (U2): centered, with Try again (OK) */
        draw_icon(ICON_MUSIC, cx, UI_H / 2 - 90, 90, RD_ACCENT);
        text_wrap_centered(F_REG, 28, cx - 480, UI_H / 2, 960, 4, 40, C_TEXT2, v->msg);
        const char *b[1] = {T("Try again")};
        draw_buttons(cx, UI_H / 2 + 180, b, 1, cf ? 0 : -1);
    } else if (!v->n) {
        const char *m = v->msg[0] ? v->msg
                        : v->view == V_FAV ? T("No favourites yet. Choose a station and press Favourite at the bottom - the first nine get a number you can press on the remote.")
                        : v->view == V_RECENT ? T("The stations you listen to show up here.")
                        : T("Nothing found");
        text_draw_wrap(F_REG, 26, x0, top + 60, 1100, 4, 38, C_TEXT3, m);
    }
    for (int i = 0; i < v->n; i++) {
        float x = x0 + (i % GCOLS) * CELL_W, y = top + 40 + (i / GCOLS) * CELL_H;
        if (y > UI_H || y + CELL_H < 0) continue;
        draw_tile(v, &v->t[i], x, y, cf && v->row == i / GCOLS && v->col == i % GCOLS);
    }
    draw_now(UI_W - 96, top - 62);
    clip_pop();
    const Tile *t = focused(v);
    const char *ok = T("Open"), *yellow = NULL;
    if (t && t->kind == K_STATION) {
        bool now = rd_playing() && !strcmp(pl.st.uuid, t->s.uuid);
        ok = now ? T("Stop, favourite") : T("Listen, favourite");
    } else if (t && t->kind == K_COUNTRY) ok = T("Choose");
    (void)yellow;
    /* burn-in: the key hints (white, always in the same place) go away 30 s after the last key press.
       No button bar (it covered the stations and could not be reached): OK opens the station's sheet, ← the side menu */
    if (!oled || app_time() - fmax(input_last_activity(), v->opened) < 30) {
        draw_hint(x0, UI_H - 62, "OK", ok);
        draw_hint(x0 + text_width(F_SEMI, 20, "OK") + 30 + text_width(F_REG, 22, ok) + 44, UI_H - 62, "←", T("Menu and search"));
    }
    side_draw(&v->side);
    if (v->dim > 0.01f) { /* everything dims (menu icons and clock too); what plays stays bright on top */
        fill_rect(0, 0, UI_W, UI_H, RGBA(8, 9, 12, (uint8_t)(190 * v->dim)));
        clip_push(SIDE_W, 120, UI_W, UI_H - 120);
        draw_now(UI_W - 96, top - 62);
        clip_pop();
    }
    if (drift) gfx_drift_end(ox, oy);
    else if (oled) view_shift(0, 0);
}

static void play_or_stop(const Station *s) {
    if (rd_playing() && !strcmp(pl.st.uuid, s->uuid)) rd_stop();
    else rd_play(s);
}

/* OK on a station: its sheet, Listen (or Stop) first - OK, OK = listen (the same rule as everywhere, STATUS 104) */
static struct { Rd *v; Station st; int i; } sheet;
static void fav_toggle_at(Rd *v, Station *st, int i) {
    fav_toggle(st);
    if (v->view == V_FAV) { /* the favourites list changes under the focus */
        show_list(v, V_FAV);
        if (v->n) { v->row = (i < v->n ? i : v->n - 1) / GCOLS; v->col = (i < v->n ? i : v->n - 1) % GCOLS; }
    }
}
static void station_sheet_done(void *ud, int idx) {
    (void)ud;
    if (!sheet.v || idx < 0) return;
    if (idx == 0) play_or_stop(&sheet.st);
    else if (idx == 1) fav_toggle_at(sheet.v, &sheet.st, sheet.i);
}
static void open_station_sheet(Rd *v, const Station *st, int i) {
    sheet.v = v;
    sheet.st = *st;
    sheet.i = i;
    bool now = rd_playing() && !strcmp(pl.st.uuid, st->uuid);
    const char *items[2] = {now ? T("Stop") : T("Listen"), fav_index(st->uuid) >= 0 ? T("Remove favourite") : T("Favourite")};
    choice_open(st->name[0] ? st->name : T("Radio"), items, 2, 0, station_sheet_done, NULL);
}

static bool rd_input(Screen *s, const InputEvent *ev) {
    Rd *v = s->st;
    int act;
    if (side_input(&v->side, ev, &act)) {
        if (act >= 0) side_activate(v, act);
        return true;
    }
    if (!v->n && v->msg[0] && !v->loading && ev->a == A_OK) { /* Try again */
        if (!ev->repeat) side_activate(v, v->side.active);
        return true;
    }
    int i = v->row * GCOLS + v->col;
    const Tile *t = focused(v);
    switch (ev->a) {
    case A_UP: if (v->row > 0) v->row--; return true;
    case A_DOWN:
        if (v->row + 1 >= rows(v)) return false; /* bottom: the key bar */
        v->row++;
        if (v->row * GCOLS + v->col >= v->n && v->n) v->col = (v->n - 1) % GCOLS;
        return true;
    case A_LEFT:
        if (v->col > 0) v->col--;
        else side_open(&v->side);
        return true;
    case A_RIGHT: if (v->col < GCOLS - 1 && i + 1 < v->n) v->col++; return true;
    case A_MENU: side_open(&v->side); return true;
    case A_SEARCH: open_search(v, ""); return true;
    case A_CHAR: {
        if (v->view == V_COUNTRIES && ev->ch < 128) { /* a letter jumps to the first country with it */
            for (int k = 0; k < v->n; k++)
                if (tolower((unsigned char)v->t[k].label[0]) == tolower((int)ev->ch)) { v->row = k / GCOLS; v->col = k % GCOLS; break; }
            return true;
        }
        char init[8] = {(char)ev->ch, 0};
        open_search(v, init);
        return true;
    }
    case A_DIGIT: /* favourite number n */
        if (ev->repeat || ev->ch < '1' || ev->ch > '9') return true;
        store_load();
        if ((int)(ev->ch - '1') < n_fav) rd_play(&favs[ev->ch - '1']);
        else app_toast(T("No favourite number %c"), (char)ev->ch);
        return true;
    case A_YELLOW:
        if (!ev->repeat && t && t->kind == K_STATION) { Station st = t->s; fav_toggle_at(v, &st, i); }
        return true;
    case A_STOP: rd_stop(); return true;
    case A_PLAYPAUSE: case A_PAUSE:
        if (ev->repeat) return true;
        if (rd_playing()) { rd_stop(); return true; }
        if (t && t->kind == K_STATION) rd_play(&t->s);
        return true;
    case A_OK: case A_PLAY:
        if (ev->repeat || !t) return true;
        if (t->kind == K_STATION) {
            Station st = t->s;
            if (ev->a == A_PLAY) rd_play(&st);
            else open_station_sheet(v, &st, i);
        } else if (t->kind == K_GENRE) {
            char tag[64], label[96];
            copy_s(tag, sizeof tag, t->key);
            copy_s(label, sizeof label, t->label);
            show_tag(v, tag, label);
        } else if (t->kind == K_COUNTRY) {
            char code[8];
            copy_s(code, sizeof code, t->key);
            choose_country(v, code);
        }
        return true;
    case A_BACK:
        if (v->view == V_TAG) { v->side.active = M_GENRES; show_genres(v); return true; }
        return false; /* leave the app - the radio keeps playing */
    default: return false;
    }
}

static Screen *radio_screen(void) {
    Screen *s = xcalloc(1, sizeof *s);
    Rd *v = xcalloc(1, sizeof *v);
    s->name = "radio";
    s->st = v;
    s->enter = rd_enter;
    s->resume = rd_resume;
    s->update = rd_update;
    s->draw = rd_draw;
    s->input = rd_input;
    s->destroy = rd_destroy;
    return s;
}

/* ---------- home screen: row, big top and "Now playing" box ---------- */
static HomeItem row_items[MAX_RECENT];
static char row_keys[MAX_RECENT][16];
static int radio_row(HomeItem **items) {
    store_load();
    for (int i = 0; i < n_recent; i++) {
        snprintf(row_keys[i], sizeof row_keys[i], "radio:%d", i);
        row_items[i] = (HomeItem){recents[i].name, (char *)T("Radio"), recents[i].logo[0] ? recents[i].logo : NULL, row_keys[i], 0, NULL};
    }
    *items = row_items;
    return n_recent;
}
static void radio_row_open(const HomeItem *it, int idx) {
    (void)idx;
    if (!it->key || strncmp(it->key, "radio:", 6)) return;
    int i = atoi(it->key + 6);
    if (i < 0 || i >= n_recent) return;
    Station s = recents[i];
    play_or_stop(&s);
}

static bool radio_draw_shelf(float x, float y) {
    if (!rd_playing()) return false;
    draw_logo(&pl.st, x, y, 230, 230, 16, 1);
    text_draw(F_SEMI, 24, x + 270, y + 40, RD_ACCENT, AL_LEFT, pl.buffering ? T("CONNECTING") : T("ON THE RADIO NOW"));
    text_draw_clip(F_BOLD, 52, x + 270, y + 80, 1200, C_TEXT, AL_LEFT, pl.st.name);
    text_draw_clip(F_REG, 32, x + 270, y + 150, 1200, C_TEXT2, AL_LEFT, pl.title[0] ? pl.title : pl.st.tags);
    return true;
}

#define OV_LEFT 700
static void radio_overlay(void) {
    static float vis, bright = 1;
    static double last_change;
    static char last_key[300];
    Screen *top = app_top();
    double now = app_time();
    bool here = top && top->name && !strcmp(top->name, "radio");
    bool want = rd_playing() && !here;
    char key[300];
    snprintf(key, sizeof key, "%s|%s", pl.st.name, pl.title);
    if (strcmp(key, last_key) || (want && vis < 0.01f)) { copy_s(last_key, sizeof last_key, key); last_change = now; }
    bool oled = oled_on();
    float target = now - last_change < 8 ? 1 : oled ? 0.55f : 0.72f;
    vis += ((want ? 1 : 0) - vis) * 0.12f;
    bright += (target - bright) * 0.05f;
    if (vis < 0.01f) return;
    float a = vis * bright;
    const char *title = pl.st.name, *sub = pl.buffering ? T("Connecting…") : pl.title[0] ? pl.title : T("Radio");
    float h = 64, lg = 48, tw = text_width(F_SEMI, 22, title), sw = text_width(F_REG, 18, sub);
    float textw = tw > sw ? tw : sw;
    if (textw > 300) textw = 300;
    float w = 8 + lg + 14 + textw + 18 + 20 + 18;
    bool busy_bar = top && top->name && (!strcmp(top->name, "home") || !strcmp(top->name, "iptv"));
    float right = busy_bar ? UI_W - 560 : UI_W - 96 - text_width(F_SEMI, 34, "00:00") - 32;
    float avail = app_overlay_place ? app_overlay_place(right, 0) : right, row_y = 49;
    if (avail - w < OV_LEFT) { textw = -18; w = 8 + lg + 14 + 20 + 18; }
    bool row2 = avail - w < OV_LEFT;
    if (row2) { avail = right; row_y = 49 + h + 16; }
    float x = row2 || !app_overlay_place ? avail - w : app_overlay_place(avail, w * vis) - w * (1 - vis), y = row_y - (1 - vis) * 14;
    /* burn-in: the box can stand for hours on the home screen - it glides very slowly and steps back */
    float ox = 0, oy = 0;
    bool drift = false;
    if (oled) {
        oled_offset(6, 149, 109, &ox, &oy);
        drift = gfx_drift_begin && gfx_drift_begin(x - 24, y - 20, w + 48, h + 48);
        if (!drift) { x += lroundf(ox); y += lroundf(oy); }
    }
    draw_shadow(x, y, w, h, h / 2, 18, 0.6f * a);
    fill_rrect(x, y, w, h, h / 2, RGBA(18, 20, 28, (uint8_t)(215 * a)));
    stroke_rrect(x, y, w, h, h / 2, 1, RGBA(255, 255, 255, (uint8_t)(22 * a)));
    draw_logo(&pl.st, x + 8, y + 8, lg, lg, lg / 2, a);
    float tx = x + 8 + lg + 14;
    if (textw > 0) {
        text_draw_clip(F_SEMI, 22, tx, y + 9, textw, color_alpha(C_TEXT, a), AL_LEFT, title);
        text_draw_clip(F_REG, 18, tx, y + 36, textw, color_alpha(C_TEXT2, a), AL_LEFT, sub);
    }
    draw_bars(tx + textw + 18, y + h / 2 + 10, a, !pl.buffering);
    if (drift) gfx_drift_end(ox, oy);
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
static void radio_init(void) {
    if (app_set_overlay) app_set_overlay("radio", radio_overlay);
}
static void radio_apply(void) {
    if (!app_on("radio")) rd_stop();
}
static void radio_forget(void) {
    rd_stop();
    char p[300];
    store_path(p, sizeof p);
    wipe_file(p);
    n_fav = n_recent = 0;
    store_loaded = true;
    cfg_unset("radio_country");
}

static const Item radio_items[] = {
    {N_("Protect OLED screens from burn-in"), IT_TOGGLE, "radio_oled", "1", set_onoff,
     .help = N_("While the radio plays, everything on the Radio screen and the Now playing box moves very slowly, and the stations dim when you only listen. You will not notice it")},
};

const NotaApp nota_app = {
    NOTA_API, "radio", N_("Radio"), N_("Radio stations from your country and the whole world"), APP_VERSION, 75, APP_ROW_LOGOS,
    ICON_MUSIC, {255, 150, 40, 255}, {200, 70, 20, 255}, {255, 255, 255, 255},
    .open = radio_screen, .init = radio_init, .tick = radio_tick, .apply = radio_apply, .forget = radio_forget,
    .row_title = N_("Radio"), .row = radio_row, .row_open = radio_row_open, .draw_shelf = radio_draw_shelf,
    .settings = radio_items, .n_settings = sizeof radio_items / sizeof radio_items[0],
};
