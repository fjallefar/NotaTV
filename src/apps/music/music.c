/* NotaTV app package "music" - Music: receives music from the phone (AirPlay via shairport-sync).
 * Metadata (title, artist, cover, progress) is read from shairport-sync's metadata pipe in a background thread.
 * The screen opens by itself when someone starts casting. */
#include "music.h"
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
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
    char title[256], artist[256], album[256], sender[128], cover[320];
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
        case FOURCC('p', 'a', 'u', 's'): /* pause */
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
    char *buf = xmalloc(cap + 1);
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
                else { cap *= 2; buf = xrealloc(buf, cap + 1); }
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

static void music_overlay(void);
void music_init(void) {
    ms_mu = SDL_CreateMutex();
    if (app_set_overlay) app_set_overlay("music", music_overlay); /* only from NotaTV 1.0.5 */
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

/* ---------- look: a generated cover while the real one loads, burn-in protection ---------- */
#define TAU 6.2831853f
static bool cover_busy; /* a cover lookup is running (main thread) */

static Color hsv(float h, float s, float v) {
    h = fmodf(h, 360);
    if (h < 0) h += 360;
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60, 2) - 1)), m = v - c, r, g, b;
    if (h < 60) r = c, g = x, b = 0;
    else if (h < 120) r = x, g = c, b = 0;
    else if (h < 180) r = 0, g = c, b = x;
    else if (h < 240) r = 0, g = x, b = c;
    else if (h < 300) r = x, g = 0, b = c;
    else r = c, g = 0, b = x;
    return RGB((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}
typedef struct { Color a, b, c; } Pal;
/* colors from the names: the same album always gets the same cover */
static Pal pal_of(const char *artist, const char *album) {
    uint32_t h = 2166136261u;
    for (const char *p = artist; p && *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ '|') * 16777619u;
    for (const char *p = album; p && *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    float hue = (float)(h % 360), spread = 30 + (float)((h >> 9) % 50);
    return (Pal){hsv(hue, 0.62f, 0.66f), hsv(hue + spread, 0.72f, 0.24f), hsv(hue - spread * 0.8f, 0.45f, 0.95f)};
}
/* soft light: rings with low alpha on top of each other (gfx colors, so it is also right when the UI is in HDR) */
static void glow(float cx, float cy, float rad, Color c, float a) {
    for (int i = 0; i < 18; i++) fill_circle(cx, cy, rad * (1 - i / 19.0f), color_alpha(c, a / 18));
}
static void first_letter(const char *s, char *out) {
    while (*s && (unsigned char)*s < 0x80 && !isalnum((unsigned char)*s)) s++;
    unsigned char c = (unsigned char)*s;
    int n = !c ? 0 : c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    memcpy(out, s, n);
    out[n] = 0;
    if (n == 1) out[0] = (char)toupper(c);
}
/* the generated cover: a gradient in the album's colors, two soft lights that drift slowly, the first letter and the
   artist - looks like a designed sleeve, and stays when no cover is found. busy: a sheen while the real one is looked up */
static void draw_gen_cover(float x, float y, float s, float r, const char *artist, const char *album, float a, bool busy) {
    Pal p = pal_of(artist, album);
    float t = (float)app_time();
    fill_rrect_vgrad(x, y, s, s, r, color_alpha(p.a, a), color_alpha(p.b, a));
    /* the lights stay away from the rounded corners (no clipping needed) */
    glow(x + s * (0.66f + 0.03f * sinf(t * 0.21f)), y + s * (0.36f + 0.03f * cosf(t * 0.17f)), s * 0.28f, p.c, 0.45f * a);
    glow(x + s * (0.30f + 0.03f * cosf(t * 0.13f)), y + s * (0.74f + 0.02f * sinf(t * 0.19f)), s * 0.24f, p.a, 0.5f * a);
    char l[8];
    first_letter(album && *album ? album : artist ? artist : "", l);
    if (l[0] && s >= 120) text_draw(F_BOLD, s * 0.42f, x + s * 0.08f, y + s * 0.47f, RGBA(255, 255, 255, (uint8_t)(225 * a)), AL_LEFT, l);
    if (s >= 200 && artist && *artist)
        text_draw_clip(F_SEMI, s * 0.045f, x + s * 0.08f, y + s * 0.08f, s * 0.84f, RGBA(255, 255, 255, (uint8_t)(190 * a)), AL_LEFT, artist);
    if (busy) {
        float w = s * 0.4f, ph = fmodf(t * 0.5f, 1.8f) - 0.4f, bx = x + ph * s;
        clip_push(x, y + r, s, s - 2 * r);
        fill_hgrad(bx, y, w / 2, s, RGBA(255, 255, 255, 0), RGBA(255, 255, 255, (uint8_t)(26 * a)));
        fill_hgrad(bx + w / 2, y, w / 2, s, RGBA(255, 255, 255, (uint8_t)(26 * a)), RGBA(255, 255, 255, 0));
        clip_pop();
    }
}
static const char *cover_name(const MusicState *s) { return s->album[0] ? s->album : s->title; }
static const char *cover_artist(const MusicState *s) { return s->artist[0] ? s->artist : s->sender; }

/* Settings → Apps → Music → "Protect OLED screens from burn-in": nothing on the Music screen stands still */
static bool oled_on(void) {
    static int gen = -1;
    static bool on;
    if (gen != cfg_gen()) { gen = cfg_gen(); on = cfg_int("music_oled", 1) != 0; }
    return on;
}
/* a slow orbit of at most amp x 0.7*amp px, under 0.5 px/s. Drawn with gfx_drift it glides (fractions of a pixel);
   on an older NotaTV without it, it is rounded to whole pixels */
static void oled_offset(float amp, float px, float py, float *ox, float *oy) {
    float t = (float)app_time();
    *ox = amp * sinf(t * TAU / px);
    *oy = amp * 0.7f * sinf(t * TAU / py + 1);
}
/* everything drawn until view_shift(0, 0) is moved (the clip rectangles move along) */
static void view_shift(int ox, int oy) {
    SDL_Rect v = {ox, oy, UI_W, UI_H};
    SDL_SetRenderViewport(gfx_renderer(), ox || oy ? &v : NULL);
}

/* ---------- screen ---------- */
typedef struct {
    BtDevice dev[8];
    int n, sel;          /* sel == n: "+ Pair new phone" */
    double refreshed;
    char connecting[128]; /* path of the phone the box is calling */
    double connect_t;
    char faded[320];      /* the cover that has faded in */
    double ready_t, opened;
    bool on_btn;          /* the focus is on "Disconnect" (Down from the player) */
} MuView;

/* the real cover fades in over the generated one (0..1, eased); one that is already there when the screen opens is shown at once */
static float cover_fade(MuView *v, const MusicState *s, Image **out) {
    double now = app_time();
    if (v->opened == 0) v->opened = now;
    Image *im = s->cover[0] ? img_get(s->cover) : NULL;
    *out = NULL;
    if (!im || img_state(im) != IMG_READY) return 0;
    *out = im;
    if (strcmp(v->faded, s->cover)) {
        snprintf(v->faded, sizeof v->faded, "%s", s->cover);
        v->ready_t = now - v->opened < 0.1 ? -100 : now;
    }
    float x = (float)((now - v->ready_t) / 0.5);
    return x >= 1 ? 1 : 1 - (1 - x) * (1 - x) * (1 - x);
}
static bool cover_loading(const MusicState *s) {
    if (cover_busy) return true;
    Image *im = s->cover[0] ? img_get(s->cover) : NULL;
    return im && img_state(im) == IMG_LOADING;
}

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

/* "Disconnect": the same pill as the phones' row; Down moves the focus to it, Up back to the player */
static void draw_disconnect(MuView *v, float x, float y) {
    const char *l = T("Disconnect");
    float pad = 34, h = 64, w = text_width(F_SEMI, 28, l) + 2 * pad;
    fill_rrect(x, y, w, h, h / 2, v->on_btn ? C_TEXT : RGBA(255, 255, 255, 22));
    text_draw(F_SEMI, 28, x + pad, y + 15, v->on_btn ? C_BG : C_TEXT, AL_LEFT, l);
}
/* the phone stops sending: Bluetooth disconnects the phone, AirPlay restarts the receiver (that ends the session) */
static void mu_disconnect(MuView *v) {
    MusicState s = snapshot();
    v->on_btn = false;
    if (!strcmp(s.source, "Bluetooth")) { bt_disconnect(); app_toast(T("Bluetooth disconnected")); return; }
    run_cmd("%s/music disconnect >/dev/null 2>&1 &", helper_path());
    SDL_LockMutex(ms_mu);
    ms.active = ms.playing = false;
    SDL_UnlockMutex(ms_mu);
    app_toast(T("AirPlay disconnected"));
}

static void mu_update(Screen *sc, float dt) {
    (void)dt;
    MuView *v = sc->st;
    MusicState s = snapshot();
    if (!s.active) v->on_btn = false;
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

/* the background is drawn without the burn-in shift (it fills the screen); it drifts by itself instead */
static void mu_draw_bg(const MusicState *s, Image *im, float fa, bool oled) {
    if (!s->active) {
        draw_background();
        fill_vgrad(0, 0, UI_W, 760, RGBA(250, 45, 85, 40), RGBA(250, 45, 85, 0));
        return;
    }
    Pal p = pal_of(cover_artist(s), cover_name(s)); /* the screen takes the album's colors at once */
    fill_vgrad_dither(0, 0, UI_W, UI_H, color_mix(RGB(16, 19, 27), p.a, 0.22f), color_mix(RGB(7, 8, 12), p.b, 0.35f));
    if (!im || fa <= 0) return;
    float k = 1, dx = 0, dy = 0;
    if (oled) { /* very slow zoom and drift */
        float t = (float)app_time();
        k = 1.07f + 0.03f * sinf(t * TAU / 97);
        dx = 30 * sinf(t * TAU / 151);
        dy = 20 * sinf(t * TAU / 113 + 2);
    }
    float w = UI_W * k, h = UI_H * k;
    if (img_draw_cover(im, (UI_W - w) / 2 + dx, (UI_H - h) / 2 + dy, w, h, 0, 0.25f * fa))
        fill_rect(0, 0, UI_W, UI_H, RGBA(8, 9, 13, (uint8_t)(170 * fa)));
}

static void mu_draw_fg(MuView *v, MusicState s, Image *im, float fa, bool oled);
static void mu_draw(Screen *sc) {
    MuView *v = sc->st;
    MusicState s = snapshot();
    bool oled = oled_on();
    Image *im;
    float fa = cover_fade(v, &s, &im);
    mu_draw_bg(&s, im, fa, oled);
    float ox = 0, oy = 0;
    if (oled) oled_offset(14, 173, 127, &ox, &oy);
    bool drift = oled && gfx_drift_begin && gfx_drift_begin(0, 0, UI_W, UI_H);
    if (oled && !drift) view_shift((int)lroundf(ox), (int)lroundf(oy));
    mu_draw_fg(v, s, im, fa, oled);
    if (drift) gfx_drift_end(ox, oy);
    else if (oled) view_shift(0, 0);
}

static void mu_draw_fg(MuView *v, MusicState s, Image *im, float fa, bool oled) {
    /* burn-in: the key hints (white, always in the same place) go away 30 s after the last key press / opening the screen */
    bool hints = !oled || app_time() - fmax(input_last_activity(), v->opened) < 30;
    draw_topbar(T("Music"));
    if (!s.active) {
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
            /* two lines: the guidance was cut ("Bluetooth on the pho...") exactly where it explains */
            text_draw_wrap(F_REG, 24, x + 32, y + 138, cw - 64, 2, 28, C_TEXT2, steps[i][1]);
        }
        text_draw(F_REG, 24, UI_W / 2, 740, C_TEXT3, AL_CENTER,
                  T("AirPlay: iPhone, iPad and Mac on the same network  ·  Bluetooth: all phones"));
        if (cfg_int("bt_enabled", 1)) draw_devices(v, 810);
        return;
    }
    float cs = 560, cx = 200, cy = UI_H / 2 - cs / 2 + 20;
    draw_shadow(cx, cy, cs, cs, 18, 40, 1);
    if (fa < 1) draw_gen_cover(cx, cy, cs, 18, cover_artist(&s), cover_name(&s), 1, cover_loading(&s));
    if (im && fa > 0) { /* fades in with a small zoom (98 % -> 100 %) */
        float d = (1 - fa) * cs * 0.01f;
        img_draw_cover(im, cx + d, cy + d, cs - 2 * d, cs - 2 * d, 18, fa);
    }
    float tx = cx + cs + 90, tw = UI_W - tx - 140;
    if (!s.title[0] && !s.playing) { /* connected, but nothing is playing yet */
        text_draw(F_SEMI, 24, tx, cy + 90, MUSIC_ACCENT, AL_LEFT, T("CONNECTED"));
        char t[200];
        snprintf(t, sizeof t, "%s", s.sender[0] ? s.sender : s.source);
        text_draw_wrap(F_BOLD, 60, tx, cy + 130, tw, 2, 72, C_TEXT, t);
        text_draw_clip(F_REG, 32, tx, cy + 290, tw, C_TEXT2, AL_LEFT, T("Start music on the phone - or press OK"));
        draw_disconnect(v, tx, cy + cs + 24);
        if (hints) draw_keybar(120, UI_H - 62, v->on_btn ? T("Disconnect") : T("Play"), NULL, NULL, NULL, "Menu");
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
    draw_disconnect(v, tx, cy + cs + 24);
    if (hints && (bt || v->on_btn)) draw_keybar(120, UI_H - 62, v->on_btn ? T("Disconnect") : T("Pause/play"), NULL, NULL, NULL, bt ? "Menu" : NULL);
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
    switch (ev->a) { /* "Disconnect" below the player (AirPlay and Bluetooth) */
    case A_DOWN: if (v->on_btn) return false; v->on_btn = true; return true;
    case A_UP: if (!v->on_btn) return false; v->on_btn = false; return true;
    case A_OK: if (v->on_btn) { if (!ev->repeat) mu_disconnect(v); return true; } break;
    case A_LEFT: case A_RIGHT: if (v->on_btn) return true; break;
    default: break;
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
    s->st = xcalloc(1, sizeof(MuView));
    s->update = mu_update;
    s->draw = mu_draw;
    s->input = mu_input;
    s->destroy = mu_destroy;
    return s;
}

/* ---------- cover for Bluetooth: Apple's iTunes search first (one call, usually well under a second), then
 * MusicBrainz + Cover Art Archive (open, but several calls at most one per second).
 * Sends the artist and album (or track) to itunes.apple.com and musicbrainz.org. Can be turned off: cfg music_cover_lookup.
 * Found covers are remembered in <data>/music-covers.tsv, so an album heard before has its cover at once. */
typedef struct { char key[300], artist[256], album[256], title[256], url[300]; bool neterr; int src; } CoverJob;
/* Settings → Apps → Music → "Cover art from": Apple then MusicBrainz ("both", the default), MusicBrainz then Apple,
   Apple only, MusicBrainz only */
enum { SRC_BOTH, SRC_MB_APPLE, SRC_APPLE, SRC_MB };
static int cover_src(void) {
    const char *v = cfg_get("music_cover_src", "both");
    return !strcmp(v, "apple") ? SRC_APPLE : !strcmp(v, "mb") ? SRC_MB : !strcmp(v, "mb_apple") ? SRC_MB_APPLE : SRC_BOTH;
}
/* a remembered cover is only used if it comes from a source that is chosen now (Apple's images are on mzstatic.com) */
static bool src_allows(int src, const char *url) {
    bool apple = strstr(url, ".mzstatic.com/") != NULL;
    return src == SRC_BOTH || src == SRC_MB_APPLE || (src == SRC_APPLE) == apple;
}
static _Thread_local bool mb_neterr; /* the last lookup failed on the network/503 (not "does not exist") */
static char cover_key[300];          /* what we last looked up (or are looking up) */
#define COVER_CACHE_N 256
static struct { char key[300], url[300]; } cover_cache[COVER_CACHE_N]; /* url "" = nothing found (not saved) */
static int cover_cache_n;            /* how many were put in (the oldest are overwritten) */
static bool cover_cache_loaded;

static void cover_cache_path(char *p, size_t n) { snprintf(p, n, "%s/music-covers.tsv", data_dir()); }
static void cover_cache_put(const char *key, const char *url) {
    int i = cover_cache_n++ % COVER_CACHE_N;
    snprintf(cover_cache[i].key, sizeof cover_cache[i].key, "%s", key);
    snprintf(cover_cache[i].url, sizeof cover_cache[i].url, "%s", url);
}
static void cover_cache_load(void) {
    if (cover_cache_loaded) return;
    cover_cache_loaded = true;
    char p[512], line[700];
    cover_cache_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        tab[1 + strcspn(tab + 1, "\r\n")] = 0;
        if (line[0] && !strncmp(tab + 1, "https://", 8)) cover_cache_put(line, tab + 1);
    }
    fclose(f);
}
static void cover_cache_save(void) { /* small file (max 256 lines), oldest first */
    char p[512], tmp[520];
    cover_cache_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    int n = cover_cache_n < COVER_CACHE_N ? cover_cache_n : COVER_CACHE_N;
    for (int k = 0; k < n; k++) {
        int i = (cover_cache_n - n + k) % COVER_CACHE_N;
        if (cover_cache[i].url[0] && !strpbrk(cover_cache[i].key, "\t\n\r")) fprintf(f, "%s\t%s\n", cover_cache[i].key, cover_cache[i].url);
    }
    fclose(f);
    rename(tmp, p);
}
/* uninstall: forget the list of albums (it says what has been played) */
static void music_forget(void) {
    char p[512];
    cover_cache_path(p, sizeof p);
    unlink(p);
    memset(cover_cache, 0, sizeof cover_cache);
    cover_cache_n = 0;
    cover_key[0] = 0;
}

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

/* names match when one contains the other (after cleaning: "Album (Deluxe)" = "Album"), ignoring case */
static bool name_match(const char *want, const char *got) {
    char g[256];
    snprintf(g, sizeof g, "%s", got ? got : "");
    name_clean(g);
    return want[0] && g[0] && (strcasestr(g, want) || strcasestr(want, g));
}
/* entity "album" (name = album) or "song" (name = track); the artist must match too. The artwork URL can be asked for
   in any size: 100x100 -> 600x600 */
static bool itunes_find(const char *entity, const char *artist, const char *prim, const char *name, char *url, size_t n) {
    char *term = str_printf("%s %s", artist, name);
    char *e = url_encode(term);
    char *u = str_printf("https://itunes.apple.com/search?media=music&entity=%s&limit=10&term=%s", entity, e);
    HttpBuf b = {0};
    bool found = false;
    if (http_request("GET", u, NULL, NULL, &b, 6) && b.status == 200 && b.data) {
        cJSON *root = cJSON_Parse(b.data), *it;
        const char *nk = !strcmp(entity, "album") ? "collectionName" : "trackName";
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "results")) {
            cJSON *ar = cJSON_GetObjectItem(it, "artistName"), *nm = cJSON_GetObjectItem(it, nk), *art = cJSON_GetObjectItem(it, "artworkUrl100");
            if (!cJSON_IsString(ar) || !cJSON_IsString(nm) || !cJSON_IsString(art) || strncmp(art->valuestring, "https://", 8)) continue;
            if (!name_match(name, nm->valuestring) || !(strcasestr(ar->valuestring, prim) || strcasestr(artist, ar->valuestring))) continue;
            const char *a = art->valuestring, *p = strstr(a, "100x100");
            if (p) snprintf(url, n, "%.*s600x600%s", (int)(p - a), a, p + 7);
            else snprintf(url, n, "%s", a);
            found = true;
            break;
        }
        cJSON_Delete(root);
    }
    http_free(&b);
    free(u); free(e); free(term);
    return found;
}

static bool itunes_try(CoverJob *j, const char *full, const char *prim, const char *album, const char *title) {
    if ((album[0] && itunes_find("album", full, prim, album, j->url, sizeof j->url)) ||
        (title[0] && itunes_find("song", full, prim, title, j->url, sizeof j->url))) {
        SDL_Log("cover: %s / %s -> iTunes", full, album[0] ? album : title);
        return true;
    }
    j->url[0] = 0;
    return false;
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
    if ((j->src == SRC_BOTH || j->src == SRC_APPLE) && itunes_try(j, full, prim, album, title)) return;
    if (j->src == SRC_APPLE) { SDL_Log("cover: %s / %s -> intet (iTunes)", full, album[0] ? album : title); return; }
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
    else if (j->src == SRC_MB_APPLE && itunes_try(j, full, prim, album, title)) { mb_neterr = false; return; }
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
    cover_busy = false;
    if (j->neterr) { cover_key[0] = 0; cover_retry_at = app_time() + 30; free(j); return; }
    cover_cache_put(j->key, j->url); /* also remember "nothing found" (in memory), so we do not ask again */
    if (j->url[0]) { cover_set(j->key, j->url); cover_cache_save(); }
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
    j.src = cover_src();
    if (!want || !strcmp(j.key, cover_key) || app_time() < cover_retry_at) return;
    snprintf(cover_key, sizeof cover_key, "%s", j.key); /* one lookup per album (MusicBrainz: max 1/s) */
    cover_cache_load();
    for (int i = 0; i < COVER_CACHE_N; i++)
        if (!strcmp(cover_cache[i].key, j.key) && (!cover_cache[i].url[0] || src_allows(j.src, cover_cache[i].url))) {
            if (cover_cache[i].url[0]) cover_set(j.key, cover_cache[i].url);
            return;
        }
    CoverJob *cj = malloc(sizeof *cj);
    if (!cj) return;
    *cj = j;
    cover_busy = true;
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
        Image *im = cov ? img_get(cov) : NULL;
        if (!im || img_state(im) != IMG_READY) {
            MusicState s = snapshot();
            draw_gen_cover(x, y, 230, 16, cover_artist(&s), cover_name(&s), 1, cover_loading(&s));
        } else img_draw_cover(im, x, y, 230, 230, 16, 1);
        text_draw(F_SEMI, 24, x + 270, y + 40, C_ACCENT, AL_LEFT, playing ? T("NOW PLAYING") : T("PAUSED"));
        text_draw_clip(F_BOLD, 52, x + 270, y + 80, 1200, C_TEXT, AL_LEFT, t);
        text_draw_clip(F_REG, 32, x + 270, y + 150, 1200, C_TEXT2, AL_LEFT, ar ? ar : "");
        shown = true;
    }
    free(t); free(ar); free(cov);
    return shown;
}
/* ---------- "Now playing" box on top of other screens (app_set_overlay) ----------
 * In the top bar next to the clock (further left on the home screen and in Live TV). Comes in when you leave Music while something plays,
 * is fully visible for 8 s after a new track, then steps back (72 %); goes away 60 s after the music stopped. */
#define OV_LEFT 700 /* the box does not go further left than this (the greeting / the screen's title is there) */
static void music_overlay(void) {
    static float vis, bright = 1;
    static double last_play, last_change;
    static char last_key[520];
    if (!ms_mu) return;
    MusicState s = snapshot();
    Screen *t = app_top();
    double now = app_time();
    bool here = t && t->name && (!strcmp(t->name, "music") || !strcmp(t->name, "bt-pair"));
    if (s.playing) last_play = now;
    bool want = s.active && !here && (s.playing || now - last_play < 60) && (s.title[0] || s.sender[0]);
    char key[520];
    snprintf(key, sizeof key, "%s|%s", s.title, s.artist);
    if (strcmp(key, last_key) || (want && vis < 0.01f)) { snprintf(last_key, sizeof last_key, "%s", key); last_change = now; }
    bool oled = oled_on();
    float target = now - last_change < 8 ? 1 : oled ? 0.55f : 0.72f;
    if (!s.playing) target *= 0.8f;
    vis += ((want ? 1 : 0) - vis) * 0.12f;
    bright += (target - bright) * 0.05f;
    if (vis < 0.01f) return;
    float a = vis * bright;

    const char *title = s.title[0] ? s.title : s.sender, *sub = s.title[0] ? s.artist : s.source;
    float h = 64, cov = 48, tw = text_width(F_SEMI, 22, title), sw = text_width(F_REG, 18, sub);
    float textw = tw > sw ? tw : sw;
    if (textw > 300) textw = 300;
    float w = 8 + cov + 14 + textw + 18 + 20 + 18;
    /* in the top bar, left of the clock; the home screen (network icon, key hint) and Live TV (status text) have
       things there, so it goes further left */
    bool busy_bar = t && t->name && (!strcmp(t->name, "home") || !strcmp(t->name, "iptv"));
    float right = busy_bar ? UI_W - 560 : UI_W - 96 - text_width(F_SEMI, 34, "00:00") - 32;
    /* other overlays (Tesla's cars on the home screen) may already be in the bar: then it goes left of them. Without room
       (it would reach the greeting/title) it shrinks to the cover and the bars, and as a last resort it goes below the bar */
    float avail = app_overlay_place ? app_overlay_place(right, 0) : right, row_y = 49;
    if (avail - w < OV_LEFT) { textw = -18; w = 8 + cov + 14 + 20 + 18; } /* bars right after the cover */
    bool row2 = avail - w < OV_LEFT;
    if (row2) { avail = right; row_y = 49 + h + 16; }
    float x = row2 || !app_overlay_place ? avail - w : app_overlay_place(avail, w * vis) - w * (1 - vis), y = row_y - (1 - vis) * 14;
    float ox = 0, oy = 0;
    bool drift = false;
    if (oled) { /* the box moves a little too (with its shadow) */
        oled_offset(6, 149, 109, &ox, &oy);
        drift = gfx_drift_begin && gfx_drift_begin(x - 24, y - 20, w + 48, h + 48);
        if (!drift) { x += lroundf(ox); y += lroundf(oy); }
    }
    draw_shadow(x, y, w, h, h / 2, 18, 0.6f * a);
    fill_rrect(x, y, w, h, h / 2, RGBA(18, 20, 28, (uint8_t)(215 * a)));
    stroke_rrect(x, y, w, h, h / 2, 1, RGBA(255, 255, 255, (uint8_t)(22 * a)));
    float cx = x + 8, cy = y + 8;
    Image *im = s.cover[0] ? img_get(s.cover) : NULL;
    if (im && img_state(im) == IMG_READY) img_draw_cover(im, cx, cy, cov, cov, cov / 2, a);
    else draw_gen_cover(cx, cy, cov, cov / 2, cover_artist(&s), cover_name(&s), a, false);
    float tx = cx + cov + 14;
    if (textw > 0) {
        text_draw_clip(F_SEMI, 22, tx, y + 9, textw, color_alpha(C_TEXT, a), AL_LEFT, title);
        text_draw_clip(F_REG, 18, tx, y + 36, textw, color_alpha(C_TEXT2, a), AL_LEFT, sub);
    }
    float bx = tx + textw + 18, base = y + h / 2 + 10; /* three small bars: dance while it plays */
    for (int i = 0; i < 3; i++) {
        float bh = s.playing ? 6 + 14 * (0.5f + 0.5f * sinf((float)now * (5.0f + i * 1.3f) + i * 1.7f)) : 5;
        fill_rrect(bx + i * 8, base - bh, 4, bh, 2, color_alpha(MUSIC_ACCENT, a));
    }
    if (drift) gfx_drift_end(ox, oy);
}

static void cover_src_changed(void) {
    for (int i = 0; i < COVER_CACHE_N; i++)
        if (!cover_cache[i].url[0]) cover_cache[i].key[0] = 0; /* "nothing found" was with the old choice */
    cover_key[0] = 0;
    SDL_LockMutex(ms_mu);
    if (!strcmp(ms.source, "Bluetooth")) ms.cover[0] = 0; /* the current cover may be from a source that is no longer chosen */
    SDL_UnlockMutex(ms_mu);
}
static const char *const src_o[] = {"both", "mb_apple", "apple", "mb", NULL},
                  *const src_l[] = {N_("Apple, then MusicBrainz"), N_("MusicBrainz, then Apple"), N_("Apple (iTunes) only"),
                                    N_("MusicBrainz only"), NULL};

static const Item music_items[] = {
    {N_("Cover art from"), IT_CYCLE, "music_cover_src", "both", src_o, src_l, .changed = cover_src_changed,
     .help = N_("For Bluetooth music. Apple is fastest; MusicBrainz is an open music database. Only the chosen ones get the artist and album")},
    {N_("Protect OLED screens from burn-in"), IT_TOGGLE, "music_oled", "1", set_onoff,
     .help = N_("Everything on the Music screen and in the Now playing box moves very slowly, so nothing stands still for hours. You will not notice it")},
};

static const MusicIface music_iface = {music_bt_state, music_bt_track, music_bt_pos};
const NotaApp nota_app = {
    NOTA_API, "music", N_("Music"), N_("Play music from your phone via AirPlay"), APP_VERSION, 70, 0,
    ICON_MUSIC, {250, 45, 85, 255}, {150, 20, 60, 255}, {255, 255, 255, 255},
    .open = music_screen, .init = music_init, .tick = music_tick, .draw_shelf = music_draw_shelf, .forget = music_forget,
    .settings = music_items, .n_settings = sizeof music_items / sizeof music_items[0],
    .iface = &music_iface,
};
