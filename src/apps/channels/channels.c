/* NotaTV app package "channels" - Own TV channels from your own Plex/Jellyfin library (Debat 28).
 * The movies and episodes (LibraryIface of the Plex and Jellyfin apps) become channels that run like real TV: the
 * schedule is fixed by the channel (a seed from its name) and the clock, so a channel is "already running" when you
 * turn it on, and up/down zaps between the channels. Channels are made by themselves:
 *   All movies · one per genre with enough movies · one per decade (80s movies) · one per series with many episodes
 *   (in episode order, "The Simpsons all day")
 * The library is kept in memory only (its addresses include the login), fetched again after 6 hours.
 * Playback: a playlist with one line per channel; each line is an mpv EDL (edl://) with the program running now from
 * the right second, followed by the next few programs. */
#define _GNU_SOURCE
#include "tv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

extern const NotaApp nota_app;

#define MAXCH 40
#define EPOCH 1767225600L /* 2026-01-01 00:00 UTC: every schedule counts from here */
#define ROWH 104
#define AHEAD 4           /* programs in each channel's EDL line */

typedef struct {
    char name[128];
    int *items;           /* indexes into lib, in schedule order */
    int n;
    double total;         /* one round, seconds */
} Channel;

static LibItem *lib;
static int nlib;
static double lib_at = -1e9;
static bool loading;
static int lib_mask = -1;  /* which sources were signed in when the library was fetched */
static Channel ch[MAXCH];
static int nch;

/* ---------- making the channels ---------- */
static unsigned hash(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
static unsigned rnd(unsigned *st) { *st ^= *st << 13; *st ^= *st >> 17; *st ^= *st << 5; return *st; }

static void ch_free(void) {
    for (int i = 0; i < nch; i++) free(ch[i].items);
    nch = 0;
}

static void ch_add(const char *name, bool (*match)(const LibItem *, const void *), const void *ud, bool keep_order) {
    if (nch >= MAXCH) return;
    Channel *c = &ch[nch];
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    for (int i = 0; i < nlib; i++) {
        if (!match(&lib[i], ud)) continue;
        c->items = xrealloc(c->items, sizeof *c->items * (c->n + 1));
        c->items[c->n++] = i;
        c->total += lib[i].dur;
    }
    if (c->n < 2 || c->total < 3600) { free(c->items); return; }
    if (!keep_order) { /* the same shuffle every time: the seed is the name */
        unsigned st = hash(name) | 1;
        for (int i = c->n - 1; i > 0; i--) { int k = (int)(rnd(&st) % (unsigned)(i + 1)); int t = c->items[i]; c->items[i] = c->items[k]; c->items[k] = t; }
    }
    nch++;
}

static bool m_movie(const LibItem *it, const void *ud) { return it->movie; }
static bool m_genre(const LibItem *it, const void *ud) { return it->movie && !strcasecmp(it->genre, ud); }
static bool m_decade(const LibItem *it, const void *ud) { int d = *(const int *)ud; return it->movie && it->year >= d && it->year < d + 10; }
static bool m_series(const LibItem *it, const void *ud) { return !it->movie && !strcmp(it->series, ud); }

static int ep_cmp(const void *a, const void *b) {
    const LibItem *x = &lib[*(const int *)a], *y = &lib[*(const int *)b];
    int c = strcmp(x->series, y->series);
    if (c) return c;
    if (x->season != y->season) return (x->season ? x->season : 999) - (y->season ? y->season : 999); /* specials last */
    return x->episode - y->episode;
}

typedef struct { char name[128]; int n; double secs; } Count;
static int count_cmp(const void *a, const void *b) { return ((const Count *)b)->n - ((const Count *)a)->n; }
static int count_of(Count **c, int *n, const char *name, double secs) {
    for (int i = 0; i < *n; i++) if (!strcasecmp((*c)[i].name, name)) { (*c)[i].n++; (*c)[i].secs += secs; return i; }
    *c = xrealloc(*c, sizeof **c * (*n + 1));
    snprintf((*c)[*n].name, sizeof (*c)[*n].name, "%s", name);
    (*c)[*n].n = 1;
    (*c)[*n].secs = secs;
    return (*n)++;
}

static void build_channels(void) {
    ch_free();
    int movies = 0;
    for (int i = 0; i < nlib; i++) movies += lib[i].movie;
    if (movies >= 5) ch_add(T("All movies"), m_movie, NULL, false);
    /* genres with at least 8 movies, the biggest first (at most 8) */
    Count *g = NULL; int ng = 0;
    for (int i = 0; i < nlib; i++) if (lib[i].movie && lib[i].genre[0]) count_of(&g, &ng, lib[i].genre, lib[i].dur);
    if (ng > 1) qsort(g, ng, sizeof *g, count_cmp);
    for (int i = 0, k = 0; i < ng && k < 8; i++) if (g[i].n >= 8) { ch_add(g[i].name, m_genre, g[i].name, false); k++; }
    free(g);
    /* decades with at least 8 movies */
    for (int d = 1950; d <= 2020; d += 10) {
        int n = 0;
        for (int i = 0; i < nlib; i++) n += lib[i].movie && lib[i].year >= d && lib[i].year < d + 10;
        if (n < 8) continue;
        char name[64];
        snprintf(name, sizeof name, T("%d's movies"), d >= 2000 ? d : d % 100); /* 80s, 2000s */
        ch_add(name, m_decade, &d, false);
    }
    /* series with at least 15 episodes, in episode order */
    Count *s = NULL; int ns = 0;
    for (int i = 0; i < nlib; i++) if (!lib[i].movie && lib[i].series[0]) count_of(&s, &ns, lib[i].series, lib[i].dur);
    if (ns > 1) qsort(s, ns, sizeof *s, count_cmp);
    for (int i = 0; i < ns && nch < MAXCH; i++) {
        if (s[i].n < 15) continue;
        int before = nch;
        ch_add(s[i].name, m_series, s[i].name, true);
        if (nch > before && ch[nch - 1].n > 1) qsort(ch[nch - 1].items, ch[nch - 1].n, sizeof(int), ep_cmp);
    }
    free(s);
}

/* the program on channel c at time t: its index in c->items and how far into it (seconds) */
static int on_air(const Channel *c, time_t t, double *into, time_t *started) {
    double pos = fmod((double)(t - EPOCH), c->total);
    for (int i = 0; i < c->n; i++) {
        double d = lib[c->items[i]].dur;
        if (pos < d) { *into = pos; if (started) *started = t - (time_t)pos; return i; }
        pos -= d;
    }
    *into = 0;
    if (started) *started = t;
    return 0;
}

static bool hidden(const char *name) {
    char k[140];
    snprintf(k, sizeof k, "\x1f%s\x1f", name);
    return strstr(cfg_get("ch_hidden", ""), k) != NULL;
}

/* ---------- loading the library ---------- */
typedef struct { LibItem *it; int n, mask; } LibJob;
static void lib_work(void *arg) {
    LibJob *j = arg;
    static const char *srcs[] = {"plex", "jellyfin"};
    for (int s = 0; s < 2; s++) {
        const LibraryIface *li = app_iface(srcs[s]);
        if (!li || !li->signed_in || !li->signed_in() || !li->library) continue;
        LibItem *got = NULL;
        int n = li->library(&got);
        if (n > 0) {
            j->it = xrealloc(j->it, sizeof *j->it * (j->n + n));
            memcpy(j->it + j->n, got, sizeof *got * n);
            j->n += n;
        }
        if (got) explicit_bzero(got, sizeof *got * (n > 0 ? n : 0));
        free(got);
    }
}
static void lib_done(void *arg) {
    LibJob *j = arg;
    if (lib) explicit_bzero(lib, sizeof *lib * nlib);
    free(lib);
    lib = j->it;
    nlib = j->n;
    lib_at = app_time();
    lib_mask = j->mask;
    loading = false;
    build_channels();
    SDL_Log("channels: %d titler, %d kanaler", nlib, nch);
    free(j);
}
static int source_mask(void) {
    static const char *srcs[] = {"plex", "jellyfin"};
    int m = 0;
    for (int s = 0; s < 2; s++) {
        const LibraryIface *li = app_iface(srcs[s]);
        if (li && li->signed_in && li->signed_in()) m |= 1 << s;
    }
    return m;
}
static bool any_source(void) {
    static const char *srcs[] = {"plex", "jellyfin"};
    for (int s = 0; s < 2; s++) {
        const LibraryIface *li = app_iface(srcs[s]);
        if (li && li->signed_in && li->signed_in()) return true;
    }
    return false;
}
static void load_lib(bool force) {
    if (loading || (!force && app_time() - lib_at < 6 * 3600)) return;
    loading = true;
    LibJob *j = xcalloc(1, sizeof(LibJob));
    j->mask = source_mask();
    job_submit_q(JQ_SLOW, lib_work, lib_done, j);
}

/* ---------- playback ---------- */
typedef struct { int *vis, n, cur; } PlayCtx;

static void edl_add(FILE *f, const char *url, double start) {
    fprintf(f, "%%%zu%%%s", strlen(url), url);
    if (start > 1) fprintf(f, ",start=%.0f", start);
}

static void label_of(const LibItem *it, char *out, size_t n) {
    if (it->movie) snprintf(out, n, "%s", it->title);
    else if (it->season > 0) snprintf(out, n, "%s · S%d E%d · %s", it->series, it->season, it->episode, it->title);
    else snprintf(out, n, "%s · %s", it->series, it->title);
}

static void on_channel(void *ud, int idx) {
    PlayCtx *pc = ud;
    if (idx < 0 || idx >= pc->n) return;
    pc->cur = idx;
    const Channel *c = &ch[pc->vis[idx]];
    double into;
    const LibItem *it = &lib[c->items[on_air(c, time(NULL), &into, NULL)]];
    char lab[400], msg[600];
    label_of(it, lab, sizeof lab);
    snprintf(msg, sizeof msg, "%d  %s  ·  %s", idx + 1, c->name, lab);
    player_osd(msg, 4000);
}

static void play_channels(const int *vis, int n, int start) {
    const char *path = "/tmp/notatv-channels.m3u";
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "#EXTM3U\n");
    time_t now = time(NULL);
    for (int k = 0; k < n; k++) {
        const Channel *c = &ch[vis[k]];
        double into;
        int i = on_air(c, now, &into, NULL);
        fprintf(f, "#EXTINF:-1,%s\nedl://", c->name);
        for (int a = 0; a < AHEAD && a < c->n; a++) {
            if (a) fputc(';', f);
            edl_add(f, lib[c->items[(i + a) % c->n]].url, a == 0 ? into : 0);
        }
        fputc('\n', f);
    }
    fclose(f);
    int *v = xmalloc(sizeof *v * n);
    memcpy(v, vis, sizeof *v * n);
    PlayCtx pc = {v, n, start};
    static const char *extra[] = {"--loop-playlist=inf", "--prefetch-playlist=no", NULL};
    PlayRequest rq = {0};
    rq.url = path;
    rq.subtitle = ch[vis[start]].name;
    rq.is_live = true;
    rq.playlist_start = start;
    rq.playlist_n = n;
    rq.channel_changed = on_channel;
    rq.ud = &pc;
    rq.extra = extra;
    on_channel(&pc, start);
    player_run(&rq);
    free(v);
    unlink(path); /* the addresses in it include the login */
}

/* ---------- the guide ---------- */
typedef struct { int vis[MAXCH], n, sel; float scroll; bool on_chips; int chip; int seen_nch; } Guide;
static Guide *live;

static void rebuild(Guide *g) {
    g->n = 0;
    for (int i = 0; i < nch; i++) if (!hidden(ch[i].name)) g->vis[g->n++] = i;
    if (g->sel >= g->n) g->sel = g->n ? g->n - 1 : 0;
    g->seen_nch = nch;
}

static void fmt_clock(time_t t, char *out, size_t n) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%H:%M", &tm);
}

static void guide_draw(Screen *s) {
    Guide *g = s->st;
    draw_background();
    draw_topbar(T("Channels"));
    static const char *chips[2];
    chips[0] = T("Update channels");
    chips[1] = T("Show hidden channels");
    draw_chips(96, 128, chips, *cfg_get("ch_hidden", "") ? 2 : 1, g->on_chips ? g->chip : -1);
    if (!any_source()) {
        draw_empty(T("No library yet"), T("Channels are made from your movies and shows in Plex or Jellyfin. Install one of them and sign in."));
        return;
    }
    if (!g->n) {
        if (loading) { draw_spinner(UI_W / 2, UI_H / 2 - 20, 30, C_TEXT); text_draw(F_REG, 24, UI_W / 2, UI_H / 2 + 30, C_TEXT3, AL_CENTER, T("Making your channels…")); }
        else draw_empty(T("Not enough for a channel"), T("A channel needs at least 8 movies of a genre or decade, or a series with 15 episodes."));
        return;
    }
    time_t now = time(NULL);
    float y0 = 220;
    clip_push(0, y0 - 10, UI_W, UI_H - y0 - 60);
    for (int k = 0; k < g->n; k++) {
        float y = y0 + k * ROWH - g->scroll;
        if (y + ROWH < y0 - 10 || y > UI_H) continue;
        const Channel *c = &ch[g->vis[k]];
        bool f = !g->on_chips && k == g->sel;
        double into;
        time_t st;
        int i = on_air(c, now, &into, &st);
        const LibItem *it = &lib[c->items[i]], *nx = &lib[c->items[(i + 1) % c->n]];
        if (f) fill_rrect(80, y - 6, UI_W - 160, ROWH - 12, 18, C_FOCUS);
        Color tc = f ? RGB(10, 10, 10) : C_TEXT, t2 = f ? RGB(60, 60, 60) : C_TEXT3;
        char num[8], lab[400], tm1[8], tm2[8], line[480];
        snprintf(num, sizeof num, "%d", k + 1);
        text_draw(F_BOLD, 28, 140, y + 26, t2, AL_CENTER, num);
        text_draw_clip(F_SEMI, 28, 190, y + 26, 420, tc, AL_LEFT, c->name);
        label_of(it, lab, sizeof lab);
        text_draw_clip(F_SEMI, 26, 650, y + 12, 760, tc, AL_LEFT, lab);
        /* how far the program has come */
        float p = (float)(into / it->dur);
        fill_rrect(650, y + 56, 300, 8, 4, f ? RGBA(0, 0, 0, 40) : C_SURFACE2);
        fill_rrect(650, y + 56, 300 * p, 8, 4, f ? RGB(30, 30, 30) : C_ACCENT);
        fmt_clock(st, tm1, sizeof tm1);
        fmt_clock(st + (time_t)it->dur, tm2, sizeof tm2);
        snprintf(line, sizeof line, "%s - %s", tm1, tm2);
        text_draw(F_REG, 20, 970, y + 48, t2, AL_LEFT, line);
        label_of(nx, lab, sizeof lab);
        snprintf(line, sizeof line, "%s %s  %s", T("Next"), tm2, lab);
        text_draw_clip(F_REG, 22, 1440, y + 30, UI_W - 1440 - 110, t2, AL_LEFT, line);
    }
    clip_pop();
    draw_keybar(96, UI_H - 62, NULL, NULL, NULL, NULL, T("Hide channel"));
}

static void guide_update(Screen *s, float dt) {
    Guide *g = s->st;
    if (g->seen_nch != nch || (!g->n && nch)) rebuild(g);
    if (any_source()) load_lib(false);
    float target = g->sel > 6 ? (g->sel - 6) * ROWH : 0;
    g->scroll = anim_to(g->scroll, target, dt, 12);
}

static void hide_cb(void *ud, bool yes) {
    Guide *g = live;
    if (!yes || !g || g->sel >= g->n) return;
    char *v = str_printf("%s\x1f%s\x1f", cfg_get("ch_hidden", ""), ch[g->vis[g->sel]].name);
    cfg_set("ch_hidden", v);
    free(v);
    rebuild(g);
}

static bool guide_input(Screen *s, const InputEvent *ev) {
    Guide *g = s->st;
    int nchips = *cfg_get("ch_hidden", "") ? 2 : 1;
    if (g->on_chips) {
        int c = g->chip;
        if (buttons_nav(&c, nchips, ev)) { g->chip = c; return true; }
        if (ev->a == A_DOWN) { if (g->n) g->on_chips = false; return true; }
        if (ev->a == A_OK) {
            if (g->chip == 0) { load_lib(true); app_toast(T("Updating the channels from your library")); }
            else { cfg_set("ch_hidden", ""); rebuild(g); g->chip = 0; }
            return true;
        }
        if (ev->a == A_BACK || ev->a == A_LEFT) { app_pop(); return true; }
        return true;
    }
    switch (ev->a) {
    case A_UP: case A_CHUP: if (g->sel) g->sel--; else g->on_chips = true; return true;
    case A_DOWN: case A_CHDOWN: if (g->sel + 1 < g->n) { g->sel++; return true; } return false;
    case A_OK: case A_PLAY: case A_PLAYPAUSE: if (g->n) play_channels(g->vis, g->n, g->sel); return true;
    case A_MENU:
        if (g->n) {
            char t[200];
            snprintf(t, sizeof t, T("Hide %s?"), ch[g->vis[g->sel]].name);
            confirm_open(t, T("It can be shown again at the top of Channels."), T("Hide"), T("Cancel"), hide_cb, NULL);
        }
        return true;
    case A_BACK: case A_LEFT: app_pop(); return true;
    default: return false;
    }
}

static void guide_enter(Screen *s) { live = s->st; rebuild(live); }
static void guide_destroy(Screen *s) { if (live == s->st) live = NULL; free(s->st); free(s); }

static Screen *guide_screen(void) {
    if (!draw_chips) { app_toast(T("Update NotaTV to use this app")); return NULL; } /* an older NotaTV */
    Screen *s = xcalloc(1, sizeof *s);
    s->name = "channels";
    s->st = xcalloc(1, sizeof(Guide));
    s->enter = guide_enter;
    s->update = guide_update;
    s->draw = guide_draw;
    s->input = guide_input;
    s->destroy = guide_destroy;
    return s;
}

static void ch_forget(void) {
    cfg_set("ch_hidden", "");
    ch_free();
    if (lib) explicit_bzero(lib, sizeof *lib * nlib);
    free(lib);
    lib = NULL;
    nlib = 0;
    lib_at = -1e9;
}

/* signed out of Plex or Jellyfin: its titles (and their logins) leave memory at once */
static void ch_tick(void) {
    static double next;
    if (!lib || app_time() < next) return;
    next = app_time() + 2;
    if (source_mask() != lib_mask) {
        SDL_Log("channels: et bibliotek er logget ud - kanalerne laves igen");
        ch_free();
        explicit_bzero(lib, sizeof *lib * nlib);
        free(lib);
        lib = NULL;
        nlib = 0;
        lib_at = -1e9;
    }
}

const NotaApp nota_app = {
    NOTA_API, "channels", N_("Channels"), N_("Your own TV channels from your movies and shows"), APP_VERSION, 15, 0,
    ICON_TV, {230, 90, 60, 255}, {150, 40, 30, 255}, {255, 255, 255, 255},
    .open = guide_screen, .tick = ch_tick, .forget = ch_forget,
};
