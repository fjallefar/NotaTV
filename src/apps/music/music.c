/* NotaTV app package "music" - Music: receives music from the phone (AirPlay via shairport-sync).
 * Metadata (title, artist, cover, progress) is read from shairport-sync's metadata pipe in a background thread.
 * The screen opens by itself when someone starts casting. */
#include "music.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vendor/cJSON.h"

#define META_PIPE "/tmp/shairport-sync-metadata"
#define COVER_DIR "/tmp/notatv-music"
#define MUSIC_ACCENT RGB(250, 45, 85)

typedef struct {
    bool active, playing;
    char source[16]; /* "AirPlay" or "Bluetooth" */
    char title[256], artist[256], album[256], sender[128], cover[256];
    double pos, dur, at; /* progress: pos seconds at app time at */
    int started;         /* counted up on every new playback (opens the screen) */
} MusicState;

static MusicState ms;
static SDL_Mutex *ms_mu;
static int cover_n;

/* ---------- metadata reader ---------- */
static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static size_t b64_decode(const char *in, size_t n, unsigned char *out) {
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64v((unsigned char)in[i]);
        if (v < 0) continue; /* newline and '=' */
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)(acc >> bits); }
    }
    return o;
}
static unsigned hex4(const char *h) { /* "636f7265" -> 'core' */
    unsigned v = 0;
    for (int i = 0; i < 8; i++) {
        char c = h[i];
        v = (v << 4) | (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return v;
}
#define FOURCC(a, b, c, d) ((unsigned)(a) << 24 | (unsigned)(b) << 16 | (unsigned)(c) << 8 | (unsigned)(d))

static void copy_str(char *dst, size_t n, const unsigned char *src, size_t len) {
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static void handle_item(unsigned type, unsigned code, const unsigned char *d, size_t len) {
    SDL_LockMutex(ms_mu);
    if (type == FOURCC('c', 'o', 'r', 'e')) {
        if (code == FOURCC('m', 'i', 'n', 'm')) copy_str(ms.title, sizeof ms.title, d, len);
        else if (code == FOURCC('a', 's', 'a', 'r')) copy_str(ms.artist, sizeof ms.artist, d, len);
        else if (code == FOURCC('a', 's', 'a', 'l')) copy_str(ms.album, sizeof ms.album, d, len);
    } else if (type == FOURCC('s', 's', 'n', 'c')) {
        switch (code) {
        case FOURCC('p', 'b', 'e', 'g'): /* new playback */
            snprintf(ms.source, sizeof ms.source, "AirPlay");
            ms.active = ms.playing = true;
            ms.started++;
            break;
        case FOURCC('p', 'r', 's', 'm'): ms.active = ms.playing = true; ms.at = app_time(); break;
        case FOURCC('p', 'f', 'l', 's'): /* pause/spol */
            if (ms.playing) { ms.pos += app_time() - ms.at; ms.at = app_time(); }
            ms.playing = false;
            break;
        case FOURCC('p', 'e', 'n', 'd'): case FOURCC('a', 'e', 'n', 'd'): ms.active = ms.playing = false; break;
        case FOURCC('s', 'n', 'a', 'm'): copy_str(ms.sender, sizeof ms.sender, d, len); break;
        case FOURCC('p', 'r', 'g', 'r'): { /* "start/now/end" in RTP frames (44100 Hz) */
            char buf[96];
            copy_str(buf, sizeof buf, d, len);
            unsigned long long a, b, c;
            if (sscanf(buf, "%llu/%llu/%llu", &a, &b, &c) == 3 && c > a) {
                ms.pos = (double)(unsigned)(b - a) / 44100.0;
                ms.dur = (double)(unsigned)(c - a) / 44100.0;
                ms.at = app_time();
            }
            break;
        }
        case FOURCC('P', 'I', 'C', 'T'): { /* cover: a new file name each time (the image cache is keyed on the path) */
            if (len < 16) { ms.cover[0] = 0; break; }
            mkdir(COVER_DIR, 0755);
            char path[256];
            snprintf(path, sizeof path, COVER_DIR "/cover-%d", ++cover_n);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(d, 1, len, f);
                fclose(f);
                if (ms.cover[0]) unlink(ms.cover);
                snprintf(ms.cover, sizeof ms.cover, "%s", path);
            }
            break;
        }
        default: break;
        }
    }
    SDL_UnlockMutex(ms_mu);
}

/* the pipe format: <item><type>hex</type><code>hex</code><length>n</length>[<data encoding="base64">...</data>]</item> */
static size_t parse_items(char *buf, size_t len) {
    size_t used = 0;
    for (;;) {
        char *it = strstr(buf + used, "<item>");
        if (!it) return len > 64 ? len - 64 : used; /* keep a tail if a tag is split */
        char *end = strstr(it, "</item>");
        if (!end) return (size_t)(it - buf);
        *end = 0;
        char *ty = strstr(it, "<type>"), *co = strstr(it, "<code>"), *ln = strstr(it, "<length>");
        if (ty && co && ln) {
            unsigned type = hex4(ty + 6), code = hex4(co + 6);
            size_t dl = (size_t)strtoul(ln + 8, NULL, 10);
            char *da = strstr(it, "<data encoding=\"base64\">");
            if (dl > 0 && da) {
                da += 24;
                char *de = strstr(da, "</data>");
                size_t n = de ? (size_t)(de - da) : strlen(da);
                unsigned char *raw = malloc(n);
                size_t rl = raw ? b64_decode(da, n, raw) : 0;
                if (raw) handle_item(type, code, raw, rl);
                free(raw);
            } else handle_item(type, code, (const unsigned char *)"", 0);
        }
        used = (size_t)(end - buf) + 7;
    }
}

static int reader_thread(void *arg) {
    (void)arg;
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
        int fd = open(META_PIPE, O_RDONLY); /* blocks until shairport-sync writes */
        if (fd < 0) { SDL_Delay(2000); continue; }
        ssize_t n;
        while ((n = read(fd, buf + len, cap - len)) > 0) {
            len += (size_t)n;
            buf[len] = 0;
            size_t used = parse_items(buf, len);
            memmove(buf, buf + used, len - used);
            len -= used;
            if (len == cap) { /* very big cover: grow (max 16 MB) */
                if (cap >= (16u << 20)) len = 0;
                else { cap *= 2; buf = realloc(buf, cap + 1); }
            }
        }
        close(fd);
        len = 0;
        SDL_LockMutex(ms_mu); /* the sender closed: no playback */
        ms.active = ms.playing = false;
        SDL_UnlockMutex(ms_mu);
    }
    return 0;
}

void music_init(void) {
    ms_mu = SDL_CreateMutex();
    SDL_Thread *t = SDL_CreateThread(reader_thread, "airplay-meta", NULL);
    if (t) SDL_DetachThread(t);
}

static MusicState snapshot(void) {
    MusicState s;
    SDL_LockMutex(ms_mu);
    s = ms;
    SDL_UnlockMutex(ms_mu);
    return s;
}

/* Bluetooth (bt.c, main thread): connection/playback and track info via AVRCP */
void music_bt_state(bool active, bool playing, const char *sender, bool new_session) {
    if (!ms_mu) return;
    SDL_LockMutex(ms_mu);
    if (new_session || strcmp(ms.source, "Bluetooth")) { /* new source: forget the AirPlay info */
        ms.title[0] = ms.artist[0] = ms.album[0] = ms.cover[0] = 0;
        ms.pos = ms.dur = 0;
    }
    snprintf(ms.source, sizeof ms.source, "Bluetooth");
    if (playing && !ms.playing) ms.at = app_time();
    if (!playing && ms.playing) ms.pos += app_time() - ms.at;
    ms.active = active;
    ms.playing = active && playing;
    if (sender) snprintf(ms.sender, sizeof ms.sender, "%s", sender);
    if (new_session) ms.started++;
    SDL_UnlockMutex(ms_mu);
}
void music_bt_track(const char *title, const char *artist, const char *album, double dur) {
    if (!ms_mu) return;
    SDL_LockMutex(ms_mu);
    if ((artist && strcmp(artist, ms.artist)) || (album && strcmp(album, ms.album)) || (!album && title && strcmp(title, ms.title)))
        ms.cover[0] = 0; /* new album: the cover is looked up again (cover_check) */
    if (title) snprintf(ms.title, sizeof ms.title, "%s", title);
    if (artist) snprintf(ms.artist, sizeof ms.artist, "%s", artist);
    if (album) snprintf(ms.album, sizeof ms.album, "%s", album);
    if (dur >= 0) ms.dur = dur;
    SDL_UnlockMutex(ms_mu);
}
void music_bt_pos(double pos) {
    if (!ms_mu) return;
    SDL_LockMutex(ms_mu);
    ms.pos = pos;
    ms.at = app_time();
    SDL_UnlockMutex(ms_mu);
}

bool music_now(char **title, char **artist, char **cover, bool *playing) {
    if (!ms_mu) return false;
    MusicState s = snapshot();
    if (!s.active) return false;
    *title = str_dup(s.title[0] ? s.title : (s.sender[0] ? s.sender : s.source));
    *artist = str_dup(s.artist);
    *cover = s.cover[0] ? str_dup(s.cover) : NULL;
    *playing = s.playing;
    return true;
}

/* ---------- screen ---------- */
typedef struct {
    BtDevice dev[8];
    int n, sel;          /* sel == n: "+ Pair new phone" */
    double refreshed;
    char connecting[128]; /* path of the phone the box is calling */
    double connect_t;
} MuView;

/* row with paired phones: OK = the box calls the phone, and the music moves to the TV */
static void draw_devices(MuView *v, float y) {
    text_draw(F_SEMI, 26, UI_W / 2, y, C_TEXT2, AL_CENTER, T("Or connect directly from the TV:"));
    float pad = 34, gap = 24, h = 64, total = 0, w[9];
    char label[9][160];
    for (int i = 0; i <= v->n; i++) {
        bool conn = i < v->n && v->connecting[0] && !strcmp(v->connecting, v->dev[i].path);
        if (i == v->n) snprintf(label[i], sizeof label[i], T("+  Pair new phone"));
        else snprintf(label[i], sizeof label[i], "%s%s", v->dev[i].name[0] ? v->dev[i].name : T("Phone"), conn ? T("   -   connecting…") : "");
        w[i] = text_width(F_SEMI, 28, label[i]) + 2 * pad + (conn ? 40 : 0);
        total += w[i] + (i ? gap : 0);
    }
    float x = UI_W / 2 - total / 2, cy = y + 56;
    for (int i = 0; i <= v->n; i++) {
        bool f = i == v->sel, conn = i < v->n && v->connecting[0] && !strcmp(v->connecting, v->dev[i].path);
        fill_rrect(x, cy, w[i], h, h / 2, f ? C_TEXT : RGBA(255, 255, 255, 22));
        float tx = x + pad;
        if (conn) { draw_spinner(tx + 12, cy + h / 2, 12, f ? C_BG : C_TEXT2); tx += 40; }
        text_draw(F_SEMI, 28, tx, cy + 15, f ? C_BG : C_TEXT, AL_LEFT, label[i]);
        x += w[i] + gap;
    }
}

static void mu_update(Screen *sc, float dt) {
    (void)dt;
    MuView *v = sc->st;
    MusicState s = snapshot();
    bt_wake(120); /* Music open: phones must be able to connect (otherwise the radio turns off when idle) */
    if (s.active || app_time() - v->connect_t > 20) v->connecting[0] = 0; /* succeeded or gave up */
    if (!s.active && app_time() - v->refreshed > 4) { /* paired phones (can change via Settings) */
        v->refreshed = app_time();
        BtDevice all[16];
        int na = bt_devices(all, 16);
        v->n = 0;
        for (int i = 0; i < na && v->n < 8; i++)
            if (!all[i].input) v->dev[v->n++] = all[i]; /* phones/computers only - not controllers */
        if (v->sel > v->n) v->sel = v->n;
    }
}

static void mu_draw(Screen *sc) {
    MuView *v = sc->st;
    MusicState s = snapshot();
    draw_background();
    draw_topbar(T("Music"));
    if (!s.active) {
        fill_vgrad(0, 0, UI_W, 760, RGBA(250, 45, 85, 40), RGBA(250, 45, 85, 0));
        draw_icon(ICON_MUSIC, UI_W / 2, 250, 140, MUSIC_ACCENT);
        text_draw(F_BOLD, 52, UI_W / 2, 360, C_TEXT, AL_CENTER, T("Play music from your phone on the TV"));
        char name[160];
        snprintf(name, sizeof name, T("Choose \"%s\""), cfg_get("device_name", "NotaTV"));
        const char *steps[3][2] = {
            {T("Start music"), T("in any app - Spotify too")},
            {T("AirPlay or Bluetooth"), T("the AirPlay icon, or Bluetooth on the phone")},
            {name, T("the music now plays on the TV")},
        };
        float cw = 440, gap = 40, x0 = UI_W / 2 - (3 * cw + 2 * gap) / 2, y = 470;
        for (int i = 0; i < 3; i++) {
            float x = x0 + i * (cw + gap);
            fill_rrect(x, y, cw, 200, 24, RGBA(255, 255, 255, 14));
            fill_circle(x + 56, y + 60, 28, MUSIC_ACCENT);
            char n[4];
            snprintf(n, sizeof n, "%d", i + 1);
            text_draw(F_BOLD, 30, x + 56, y + 42, C_TEXT, AL_CENTER, n);
            text_draw_clip(F_BOLD, 30, x + 32, y + 108, cw - 64, C_TEXT, AL_LEFT, steps[i][0]);
            text_draw_clip(F_REG, 24, x + 32, y + 150, cw - 64, C_TEXT2, AL_LEFT, steps[i][1]);
        }
        text_draw(F_REG, 24, UI_W / 2, 740, C_TEXT3, AL_CENTER,
                  T("AirPlay: iPhone, iPad and Mac on the same network  ·  Bluetooth: all phones"));
        if (cfg_int("bt_enabled", 1)) draw_devices(v, 810);
        return;
    }
    Image *im = s.cover[0] ? img_get(s.cover) : NULL;
    if (im && img_draw_cover(im, 0, 0, UI_W, UI_H, 0, 0.25f)) fill_rect(0, 0, UI_W, UI_H, RGBA(8, 9, 13, 170));
    float cs = 560, cx = 200, cy = UI_H / 2 - cs / 2 + 20;
    draw_shadow(cx, cy, cs, cs, 18, 40, 1);
    fill_rrect(cx, cy, cs, cs, 18, C_SURFACE);
    if (!im || !img_draw_cover(im, cx, cy, cs, cs, 18, 1)) draw_icon(ICON_MUSIC, cx + cs / 2, cy + cs / 2, 160, C_TEXT3);
    float tx = cx + cs + 90, tw = UI_W - tx - 140;
    if (!s.title[0] && !s.playing) { /* connected, but nothing is playing yet */
        text_draw(F_SEMI, 24, tx, cy + 90, MUSIC_ACCENT, AL_LEFT, T("CONNECTED"));
        char t[200];
        snprintf(t, sizeof t, "%s", s.sender[0] ? s.sender : s.source);
        text_draw_wrap(F_BOLD, 60, tx, cy + 130, tw, 2, 72, C_TEXT, t);
        text_draw_clip(F_REG, 32, tx, cy + 290, tw, C_TEXT2, AL_LEFT, T("Start music on the phone - or press OK"));
        draw_keybar(120, UI_H - 62, T("Play"), NULL, NULL, NULL, "Menu");
        return;
    }
    text_draw(F_SEMI, 24, tx, cy + 90, MUSIC_ACCENT, AL_LEFT, s.playing ? T("NOW PLAYING") : T("PAUSED"));
    text_draw_wrap(F_BOLD, 60, tx, cy + 130, tw, 2, 72, C_TEXT, s.title[0] ? s.title : T("Unknown track"));
    text_draw_clip(F_REG, 36, tx, cy + 290, tw, C_TEXT2, AL_LEFT, s.artist);
    text_draw_clip(F_REG, 28, tx, cy + 345, tw, C_TEXT3, AL_LEFT, s.album);
    double pos = s.pos + (s.playing && s.at > 0 ? app_time() - s.at : 0);
    if (s.dur > 0) {
        if (pos > s.dur) pos = s.dur;
        float by = cy + cs - 60;
        fill_rrect(tx, by, tw, 8, 4, RGBA(255, 255, 255, 50));
        fill_rrect(tx, by, tw * (float)(pos / s.dur), 8, 4, C_TEXT);
        char a[32], b[32];
        text_draw(F_REG, 22, tx, by + 22, C_TEXT2, AL_LEFT, fmt_duration(pos, a, sizeof a));
        text_draw(F_REG, 22, tx + tw, by + 22, C_TEXT2, AL_RIGHT, fmt_duration(s.dur, b, sizeof b));
    }
    char from[240];
    bool bt = !strcmp(s.source, "Bluetooth");
    snprintf(from, sizeof from, "%s%s%s%s", s.source, s.sender[0] ? T(" from ") : "", s.sender,
             bt ? T("   ·   ← →: previous/next") : T("   ·   Control playback from the phone"));
    if (bt) draw_keybar(120, UI_H - 62, T("Pause/play"), NULL, NULL, NULL, "Menu");
    text_draw(F_REG, 22, UI_W - 120, UI_H - 58, C_TEXT3, AL_RIGHT, from);
}

static void mu_menu_cb(void *ud, int idx) { /* Red = menu */
    (void)ud;
    if (idx == 0) { bt_disconnect(); app_toast(T("Bluetooth disconnected")); }
}

static bool mu_input(Screen *sc, const InputEvent *ev) {
    MuView *v = sc->st;
    MusicState s = snapshot();
    if (!s.active) { /* choose a paired phone and call it */
        if (!cfg_int("bt_enabled", 1)) return false;
        switch (ev->a) {
        case A_LEFT: if (v->sel > 0) v->sel--; return true;
        case A_RIGHT: if (v->sel < v->n) v->sel++; return true;
        case A_OK:
            if (ev->repeat) return true;
            if (v->sel == v->n) { app_push(bt_pair_screen()); v->refreshed = 0; return true; }
            snprintf(v->connecting, sizeof v->connecting, "%s", v->dev[v->sel].path);
            v->connect_t = app_time();
            bt_connect(v->dev[v->sel].path);
            return true;
        default: return false;
        }
    }
    if (strcmp(s.source, "Bluetooth")) return false;
    switch (ev->a) { /* Bluetooth: control the phone (AVRCP) */
    case A_OK: case A_PLAYPAUSE: if (!ev->repeat) bt_media(s.playing ? "Pause" : "Play"); return true;
    case A_RIGHT: case A_NEXT: bt_media("Next"); return true;
    case A_LEFT: case A_PREV: bt_media("Previous"); return true;
    case A_MENU: {
        static const char *items[] = {N_("Disconnect Bluetooth")};
        choice_open(T("Music"), items, 1, 0, mu_menu_cb, NULL);
        return true;
    }
    default: return false;
    }
}

static void mu_destroy(Screen *sc) {
    free(sc->st);
    free(sc);
}

Screen *music_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "music";
    s->st = calloc(1, sizeof(MuView));
    s->update = mu_update;
    s->draw = mu_draw;
    s->input = mu_input;
    s->destroy = mu_destroy;
    return s;
}

/* ---------- cover for Bluetooth: MusicBrainz + Cover Art Archive (open, non-commercial) ----------
 * Sends the artist and album (or track) to musicbrainz.org. Can be turned off: cfg music_cover_lookup. */
typedef struct { char key[300], artist[256], album[256], title[256], url[200]; bool neterr; } CoverJob;
static _Thread_local bool mb_neterr; /* the last lookup failed on the network/503 (not "does not exist") */
static char cover_key[300];          /* what we last looked up (or are looking up) */
static struct { char key[300], url[200]; } cover_cache[16];
static int cover_cache_n;

static void q_clean(char *s) { for (; *s; s++) if (*s == '"' || *s == '\\') *s = ' '; } /* Lucene quotes */

/* Spotify etc. send "Album (Deluxe Edition)", "Song - Remastered 2011" and "A, B feat. C":
   remove parentheses/brackets and " - …" suffixes, and only use the first artist */
static void name_clean(char *s) {
    char out[256];
    size_t n = 0;
    int depth = 0;
    for (const char *p = s; *p && n < sizeof out - 1; p++) {
        if (*p == '(' || *p == '[') { depth++; continue; }
        if ((*p == ')' || *p == ']') && depth) { depth--; continue; }
        if (!depth) out[n++] = *p;
    }
    out[n] = 0;
    char *dash = strstr(out, " - ");
    if (dash) *dash = 0;
    snprintf(s, 256, "%s", out);
    str_trim(s);
}
static void artist_primary(char *s) {
    static const char *const seps[] = {", ", " & ", " feat. ", " feat ", " ft. ", " ft ", " x ", " og ", " and ", " with "};
    for (size_t i = 0; i < sizeof seps / sizeof seps[0]; i++) {
        char *p = strcasestr(s, seps[i]);
        if (p && p != s) *p = 0;
    }
    str_trim(s);
}

/* MusicBrainz requires a real User-Agent and at most 1 call per second; 503 = too fast -> wait and try again */
static char *mb_get(const char *path_query) {
    static double last;
    char *url = str_printf("https://musicbrainz.org/ws/2/%s&fmt=json", path_query);
    const char *hdr[] = {"Accept: application/json", "User-Agent: NotaTV/" TV_VERSION " ( open source TV box; cover art for Bluetooth music )", NULL};
    char *out = NULL;
    for (int attempt = 0; attempt < 3 && !out; attempt++) {
        double wait = last + 1.1 + attempt * 1.5 - app_time();
        if (wait > 0) SDL_Delay((Uint32)(wait * 1000));
        last = app_time();
        HttpBuf b = {0};
        bool ok = http_request("GET", url, hdr, NULL, &b, 8);
        if (ok && b.status == 200 && b.data) { out = b.data; b.data = NULL; }
        long st = b.status;
        http_free(&b);
        if (ok && st != 503) break; /* 404/400 etc.: no point in trying again */
    }
    if (!out) mb_neterr = true;
    free(url);
    return out;
}

/* is there a front image in the Cover Art Archive for this release group? (small image = cheap check) */
static bool caa_has(const char *gid) {
    char *u = str_printf("https://coverartarchive.org/release-group/%s/front-250", gid);
    HttpBuf b = {0};
    bool ok = http_request("GET", u, NULL, NULL, &b, 8) && b.status == 200 && b.len > 100;
    http_free(&b);
    free(u);
    return ok;
}

/* search and take the first candidate (score >= 80) that has a cover */
static char *mb_find(const char *kind, const char *query) {
    char *e = url_encode(query);
    char *pq = str_printf("%s/?query=%s&limit=5", kind, e);
    char *js = mb_get(pq);
    char *found = NULL;
    cJSON *root = cJSON_Parse(js);
    cJSON *arr = cJSON_GetObjectItem(root, !strcmp(kind, "release-group") ? "release-groups" : "recordings"), *it;
    int tried = 0;
    cJSON_ArrayForEach(it, arr) {
        cJSON *sc = cJSON_GetObjectItem(it, "score");
        if (cJSON_IsNumber(sc) && sc->valueint < 80) break; /* sorted by score */
        const char *gid = NULL;
        if (!strcmp(kind, "release-group")) {
            cJSON *id = cJSON_GetObjectItem(it, "id");
            gid = cJSON_IsString(id) ? id->valuestring : NULL;
        } else { /* track: the release group of its releases */
            cJSON *rel = cJSON_GetArrayItem(cJSON_GetObjectItem(it, "releases"), 0);
            cJSON *id = cJSON_GetObjectItem(cJSON_GetObjectItem(rel, "release-group"), "id");
            gid = cJSON_IsString(id) ? id->valuestring : NULL;
        }
        if (!gid || ++tried > 3) continue;
        if (caa_has(gid)) { found = str_dup(gid); break; }
    }
    cJSON_Delete(root);
    free(js); free(pq); free(e);
    return found;
}

static void cover_work(void *arg) {
    CoverJob *j = arg;
    mb_neterr = false;
    char full[256], prim[256], album[256], title[256];
    snprintf(full, sizeof full, "%s", j->artist);
    snprintf(album, sizeof album, "%s", j->album);
    snprintf(title, sizeof title, "%s", j->title);
    q_clean(full); q_clean(album); q_clean(title);
    snprintf(prim, sizeof prim, "%s", full);
    artist_primary(prim); /* "A, B feat. C" -> "A" (tried after the full name: "Mumford & Sons" must stay whole) */
    name_clean(album);
    name_clean(title);
    const char *artists[2] = {full, strcmp(full, prim) ? prim : NULL};
    char *id = NULL;
    for (int k = 0; k < 2 && !id; k++) {
        const char *ar = artists[k];
        if (!ar) continue;
        if (album[0]) { /* album -> release-group */
            char *q = str_printf("releasegroup:\"%s\" AND artist:\"%s\"", album, ar);
            id = mb_find("release-group", q);
            free(q);
        }
        if (!id && title[0]) { /* the track -> its release (singles, or an album with a different name) */
            char *q = str_printf("recording:\"%s\" AND artist:\"%s\"", title, ar);
            id = mb_find("recording", q);
            free(q);
        }
    }
    if (!id && album[0]) { /* looser: without quotes (spelling, punctuation) */
        char *q = str_printf("releasegroup:(%s) AND artist:(%s)", album, prim);
        id = mb_find("release-group", q);
        free(q);
    }
    if (id) snprintf(j->url, sizeof j->url, "https://coverartarchive.org/release-group/%s/front-500", id);
    j->neterr = !id && mb_neterr;
    SDL_Log("cover: %s / %s -> %s", full, album[0] ? album : title, id ? "fundet" : j->neterr ? "netværksfejl (prøver igen)" : "intet");
    free(id);
}

static void cover_set(const char *key, const char *url) {
    SDL_LockMutex(ms_mu);
    char now[300];
    snprintf(now, sizeof now, "%s|%s", ms.artist, ms.album[0] ? ms.album : ms.title);
    if (!strcmp(now, key) && !strcmp(ms.source, "Bluetooth")) snprintf(ms.cover, sizeof ms.cover, "%s", url);
    SDL_UnlockMutex(ms_mu);
}

static double cover_retry_at; /* network error: the same album is looked up again after this */
static void cover_done(void *arg) {
    CoverJob *j = arg;
    if (j->neterr) { cover_key[0] = 0; cover_retry_at = app_time() + 30; free(j); return; }
    int i = cover_cache_n++ % 16; /* also remember "nothing found", so we do not ask again */
    snprintf(cover_cache[i].key, sizeof cover_cache[i].key, "%s", j->key);
    snprintf(cover_cache[i].url, sizeof cover_cache[i].url, "%s", j->url);
    if (j->url[0]) cover_set(j->key, j->url);
    free(j);
}

static void cover_check(void) {
    if (!cfg_int("music_cover_lookup", 1)) return;
    SDL_LockMutex(ms_mu);
    bool want = ms.active && !strcmp(ms.source, "Bluetooth") && ms.artist[0] && (ms.album[0] || ms.title[0]);
    CoverJob j = {0};
    if (want) {
        snprintf(j.key, sizeof j.key, "%s|%s", ms.artist, ms.album[0] ? ms.album : ms.title);
        snprintf(j.artist, sizeof j.artist, "%s", ms.artist);
        snprintf(j.album, sizeof j.album, "%s", ms.album);
        snprintf(j.title, sizeof j.title, "%s", ms.title);
    }
    SDL_UnlockMutex(ms_mu);
    if (!want || !strcmp(j.key, cover_key) || app_time() < cover_retry_at) return;
    snprintf(cover_key, sizeof cover_key, "%s", j.key); /* one lookup per album (MusicBrainz: max 1/s) */
    for (int i = 0; i < 16; i++)
        if (!strcmp(cover_cache[i].key, j.key)) { if (cover_cache[i].url[0]) cover_set(j.key, cover_cache[i].url); return; }
    CoverJob *cj = malloc(sizeof *cj);
    if (!cj) return;
    *cj = j;
    job_submit_q(JQ_SLOW, cover_work, cover_done, cj);
}

/* called every frame: new playback -> show "Now playing" (unless it is already on top) */
void music_tick(void) {
    static int seen = -1;
    if (!ms_mu) return;
    cover_check();
    SDL_LockMutex(ms_mu);
    int st = ms.started;
    SDL_UnlockMutex(ms_mu);
    if (seen < 0) { seen = st; return; }
    if (st == seen) return;
    seen = st;
    Screen *t = app_top();
    if (!t || !t->name || strcmp(t->name, "music")) app_push(music_screen());
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
/* the big top on the home screen: something is playing - cover and title instead of the icon */
static bool music_draw_shelf(float x, float y) {
    char *t = NULL, *ar = NULL, *cov = NULL;
    bool playing = false, shown = false;
    if (music_now(&t, &ar, &cov, &playing) && t) {
        fill_rrect(x, y, 300, 300, 16, C_SURFACE);
        if (cov) img_draw_cover(img_get(cov), x, y, 300, 300, 16, 1);
        text_draw(F_SEMI, 24, x + 340, y + 70, C_ACCENT, AL_LEFT, playing ? T("NOW PLAYING") : T("PAUSED"));
        text_draw_clip(F_BOLD, 52, x + 340, y + 110, 1200, C_TEXT, AL_LEFT, t);
        text_draw_clip(F_REG, 32, x + 340, y + 180, 1200, C_TEXT2, AL_LEFT, ar ? ar : "");
        shown = true;
    }
    free(t); free(ar); free(cov);
    return shown;
}
static const MusicIface music_iface = {music_bt_state, music_bt_track, music_bt_pos};
const NotaApp nota_app = {
    NOTA_API, "music", N_("Music"), N_("Play music from your phone via AirPlay"), "1.0.0", 70, 0,
    ICON_MUSIC, {250, 45, 85, 255}, {150, 20, 60, 255}, {255, 255, 255, 255},
    .open = music_screen, .init = music_init, .tick = music_tick, .draw_shelf = music_draw_shelf,
    .iface = &music_iface,
};
