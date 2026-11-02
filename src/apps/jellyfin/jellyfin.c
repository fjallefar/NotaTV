/* NotaTV app package "jellyfin" - Jellyfin: libraries, continue/next episode, series and playback via mpv */
#include "jellyfin.h"
#include "vendor/cJSON.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <poll.h>
#include <string.h>
#include <time.h>

#define JF_ACCENT RGB(0, 164, 220)
#define TICKS 10000000.0 /* Jellyfin time: 100 ns */

static const char *jf_url(void) { return cfg_get("jf_url", ""); }
/* the box's own name (as Plex, AirPlay and DLNA use) - quotes and control characters would break the header */
static const char *jf_devname(char *buf, size_t n) {
    snprintf(buf, n, "%s", cfg_get("device_name", "NotaTV"));
    for (char *c = buf; *c; c++) if ((unsigned char)*c < 32 || *c == '"' || *c == '\\') *c = ' ';
    if (!buf[0]) snprintf(buf, n, "NotaTV");
    return buf;
}
static const char *jf_token(void) { return cfg_get("jf_token", ""); }
static const char *jf_uid(void) { return cfg_get("jf_uid", ""); }

static const char *device_id(void) {
    const char *d = cfg_get("jf_device", "");
    if (*d) return d;
    char id[40];
    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&id);
    snprintf(id, sizeof id, "notatv-%08x%08x", rand(), rand());
    cfg_set("jf_device", id);
    return cfg_get("jf_device", "");
}

/* ---------- HTTP ---------- */
typedef struct {
    char *path, *body;
    const char *method;
    cJSON *json;
    long status;
    int token;
    char auth[512];
    void (*done)(void *ud, cJSON *json, long status);
    void *ud;
} JReq;
static int jf_gen; /* increased when screens close: replies to closed screens are ignored */

static void jreq_work(void *arg) {
    JReq *r = arg;
    char *url = str_printf("%s%s", jf_url(), r->path);
    const char *hdr[] = {"Accept: application/json", "Content-Type: application/json", r->auth, NULL};
    HttpBuf b;
    if (http_request(r->method, url, hdr, r->body, &b, 20)) {
        r->status = b.status;
        if (b.data) r->json = cJSON_Parse(b.data);
    }
    http_free(&b);
    free(url);
}

static void jreq_done(void *arg) {
    JReq *r = arg;
    if (r->done && (r->token == jf_gen || r->token < 0)) r->done(r->ud, r->json, r->status);
    cJSON_Delete(r->json);
    free(r->path);
    free(r->body);
    free(r);
}

/* keep: the reply is delivered even if the screen has changed (e.g. playback reports) */
static void jf_call_ex(const char *method, const char *path, const char *body,
                       void (*done)(void *, cJSON *, long), void *ud, bool keep) {
    JReq *r = calloc(1, sizeof *r);
    r->method = method;
    r->path = str_dup(path);
    r->body = str_dup(body);
    r->done = done;
    r->ud = ud;
    r->token = keep ? -1 : jf_gen;
    const char *tok = jf_token();
    char dn[128];
    snprintf(r->auth, sizeof r->auth,
             "Authorization: MediaBrowser Client=\"NotaTV\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"%s%s%s",
             jf_devname(dn, sizeof dn), device_id(), TV_VERSION, *tok ? ", Token=\"" : "", tok, *tok ? "\"" : "");
    job_submit(jreq_work, jreq_done, r);
}
static void jf_call(const char *method, const char *path, const char *body, void (*done)(void *, cJSON *, long), void *ud) {
    jf_call_ex(method, path, body, done, ud, false);
}

/* ---------- elementer ---------- */
typedef struct {
    char *id, *type, *name, *series, *series_id, *overview, *year, *backdrop_id, *logo_id, *rating;
    int index, parent_index, child_count;
    int unplayed;
    int tmdb; /* ProviderIds.Tmdb (0 = unknown): finds the right subtitles for a movie */
    int rt_critic, community10; /* Rotten Tomatoes % and community rating x10; -1 = unknown */
    double pos, run; /* seconds */
    bool played;
    char last_played[32]; /* UserData.LastPlayedDate (ISO 8601, compares as text); "" = never */
} JItem;

static void jitem_free(JItem *j) {
    free(j->id); free(j->type); free(j->name); free(j->series); free(j->series_id);
    free(j->overview); free(j->year); free(j->backdrop_id); free(j->logo_id); free(j->rating);
}
static JItem jitem_copy(const JItem *s) {
    JItem d = *s;
    d.id = str_dup(s->id); d.type = str_dup(s->type); d.name = str_dup(s->name); d.series = str_dup(s->series);
    d.series_id = str_dup(s->series_id); d.overview = str_dup(s->overview); d.year = str_dup(s->year);
    d.backdrop_id = str_dup(s->backdrop_id); d.logo_id = str_dup(s->logo_id); d.rating = str_dup(s->rating);
    return d;
}

static char *js(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? str_dup(v->valuestring) : NULL;
}
static double jnum(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void jitem_parse(JItem *j, cJSON *o) {
    memset(j, 0, sizeof *j);
    j->id = js(o, "Id");
    j->type = js(o, "Type");
    j->name = js(o, "Name");
    j->series = js(o, "SeriesName");
    j->series_id = js(o, "SeriesId");
    j->overview = js(o, "Overview");
    int y = (int)jnum(o, "ProductionYear");
    if (y > 0) j->year = str_printf("%d", y);
    j->index = (int)jnum(o, "IndexNumber");
    j->parent_index = (int)jnum(o, "ParentIndexNumber");
    j->child_count = (int)jnum(o, "ChildCount");
    cJSON *tm = cJSON_GetObjectItem(cJSON_GetObjectItem(o, "ProviderIds"), "Tmdb");
    if (cJSON_IsString(tm)) j->tmdb = atoi(tm->valuestring);
    j->run = jnum(o, "RunTimeTicks") / TICKS;
    cJSON *ud = cJSON_GetObjectItem(o, "UserData");
    j->pos = jnum(ud, "PlaybackPositionTicks") / TICKS;
    j->played = cJSON_IsTrue(cJSON_GetObjectItem(ud, "Played"));
    cJSON *lp = cJSON_GetObjectItem(ud, "LastPlayedDate");
    if (cJSON_IsString(lp)) snprintf(j->last_played, sizeof j->last_played, "%s", lp->valuestring);
    j->unplayed = (int)jnum(ud, "UnplayedItemCount");
    /* backdrop: its own, otherwise the series'/parent's */
    cJSON *bt = cJSON_GetObjectItem(o, "BackdropImageTags");
    if (cJSON_GetArraySize(bt) > 0) j->backdrop_id = str_dup(j->id);
    else j->backdrop_id = js(o, "ParentBackdropItemId");
    /* title logo: its own, otherwise the series' */
    if (cJSON_GetObjectItem(cJSON_GetObjectItem(o, "ImageTags"), "Logo")) j->logo_id = str_dup(j->id);
    else j->logo_id = js(o, "ParentLogoItemId");
    j->rating = js(o, "OfficialRating");
    j->rt_critic = cJSON_GetObjectItem(o, "CriticRating") ? (int)(jnum(o, "CriticRating") + 0.5) : -1;
    j->community10 = cJSON_GetObjectItem(o, "CommunityRating") ? (int)(jnum(o, "CommunityRating") * 10 + 0.5) : -1;
}

static bool is_ep(const JItem *j) { return j->type && !strcmp(j->type, "Episode"); }

/* poster: episodes use the series' */
static char *poster_url(const JItem *j) {
    const char *id = is_ep(j) && j->series_id ? j->series_id : j->id;
    return id ? str_printf("%s/Items/%s/Images/Primary?maxHeight=450&quality=90", jf_url(), id) : NULL;
}
static char *logo_url(const JItem *j) {
    return j->logo_id ? str_printf("%s/Items/%s/Images/Logo?maxWidth=700&quality=90", jf_url(), j->logo_id) : NULL;
}
static char *backdrop_url(const JItem *j, int w) {
    if (j->backdrop_id) return str_printf("%s/Items/%s/Images/Backdrop?maxWidth=%d&quality=85", jf_url(), j->backdrop_id, w);
    return j->id ? str_printf("%s/Items/%s/Images/Primary?maxWidth=%d&quality=85", jf_url(), j->id, w) : NULL;
}

static void item_label(const JItem *j, char *t, size_t tn, char *s, size_t sn) {
    if (is_ep(j)) {
        snprintf(t, tn, "%s", j->series ? j->series : (j->name ? j->name : ""));
        snprintf(s, sn, T("S%d E%d · %s"), j->parent_index, j->index, j->name ? j->name : "");
    } else {
        snprintf(t, tn, "%s", j->name ? j->name : "");
        snprintf(s, sn, "%s", j->year ? j->year : "");
    }
}

/* title logo/title, metadata, rating and overview - shared by hero, focus and the detail page (like Plex). Returns the bottom */
static void meta_add(char *m, size_t n, const char *fmt, ...) {
    size_t l = strlen(m);
    if (l) l += snprintf(m + l, n - l, "  ·  ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m + l, n - l, fmt, ap);
    va_end(ap);
}
static float draw_jinfo(const JItem *h, float hx, float hy, float a, int lines, bool compact) {
    char t[256], sub[256], meta[300] = "";
    item_label(h, t, sizeof t, sub, sizeof sub);
    bool ep = is_ep(h);
    char *lu = logo_url(h);
    float lh = compact ? 110 : 170, my = compact ? 128 : 196;
    if (!lu || !draw_title_logo(lu, hx, hy, compact ? 480 : 640, lh, a))
        text_draw_wrap(F_BOLD, compact ? 56 : 64, hx, hy + (compact ? 36 : 60), 1000, compact ? 1 : 2, 72, color_alpha(C_TEXT, a), t);
    free(lu);
    if (ep) meta_add(meta, sizeof meta, "%s", sub);
    else {
        if (h->year) meta_add(meta, sizeof meta, "%s", h->year);
        if (h->rating) meta_add(meta, sizeof meta, "%s", h->rating);
    }
    int dm = (int)(h->run / 60);
    if (dm >= 60) meta_add(meta, sizeof meta, T("%dh %02dm"), dm / 60, dm % 60);
    else if (dm > 0) meta_add(meta, sizeof meta, "%dm", dm);
    if (h->type && !strcmp(h->type, "Series") && h->child_count > 0) meta_add(meta, sizeof meta, h->child_count == 1 ? T("%d season") : T("%d seasons"), h->child_count);
    if (h->community10 > 0) meta_add(meta, sizeof meta, "%d.%d/10", h->community10 / 10, h->community10 % 10);
    text_draw_clip(F_SEMI, 27, hx, hy + my, 1100, color_alpha(C_TEXT2, a), AL_LEFT, meta);
    float mw = text_width(F_SEMI, 27, meta);
    if (h->rt_critic >= 0 && mw < 1100) draw_ratings(hx + mw + 30, hy + my - 4, h->rt_critic, -1, -1);
    float sy = hy + my + (compact ? 46 : 52), sl = compact ? 37 : 40;
    int n = text_draw_wrap(F_REG, compact ? 27 : 29, hx, sy, 1050, lines, sl, color_alpha(C_TEXT, a * 0.9f), h->overview ? h->overview : "");
    return sy + n * sl;
}

/* ---------- playback (with reporting to the server, so "continue" works everywhere) ---------- */
typedef struct {
    char id[64];
    double last;
    double outro_start; /* from here the item counts as watched (start of the outro/credits), 0 = 90 % */
    JItem *it;
    JItem next;         /* the next episode (fetched shortly after the start) */
    bool has_next;
} PlayCtxJ;

/* raised after every playback: the home rows are fetched again (see jf_update) */
static int jf_play_gen;

static void report(const char *path, const char *id, double pos, bool paused) {
    char *b = str_printf("{\"ItemId\":\"%s\",\"PositionTicks\":%.0f,\"IsPaused\":%s,\"CanSeek\":true,\"PlayMethod\":\"DirectStream\"}",
                         id, pos * TICKS, paused ? "true" : "false");
    jf_call_ex("POST", path, b, NULL, NULL, true);
    free(b);
}

/* a request that waits for the answer (short timeout): used when playback ends, so the lists that are fetched
   straight afterwards already show the new "continue"/"next up" state, and before it starts (stream info) */
static cJSON *jf_get_sync(const char *method, const char *path, const char *body, long timeout) {
    char *url = str_printf("%s%s", jf_url(), path);
    char auth[512], dn[128];
    snprintf(auth, sizeof auth,
             "Authorization: MediaBrowser Client=\"NotaTV\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"",
             jf_devname(dn, sizeof dn), device_id(), TV_VERSION, jf_token());
    const char *hdr[] = {"Accept: application/json", "Content-Type: application/json", auth, NULL};
    HttpBuf b;
    cJSON *j = NULL;
    if (http_request(method, url, hdr, body, &b, timeout) && b.data && b.status >= 200 && b.status < 300) j = cJSON_Parse(b.data);
    http_free(&b);
    free(url);
    return j;
}
static void report_sync(const char *path, const char *id, double pos) {
    char *b = str_printf("{\"ItemId\":\"%s\",\"PositionTicks\":%.0f,\"IsPaused\":false,\"CanSeek\":true,\"PlayMethod\":\"DirectStream\"}",
                         id, pos * TICKS);
    /* waits for the server (a slow one too), so "Continue watching" is up to date when the lists are fetched again */
    cJSON_Delete(jf_get_sync("POST", path, b, 10));
    free(b);
}

static void on_progress(void *ud, double pos, double dur, bool ended, bool paused) {
    PlayCtxJ *c = ud;
    (void)dur;
    if (ended) return;
    report("/Sessions/Playing/Progress", c->id, pos, paused);
    c->last = pos;
}

static void remote_play_tick(void *ud); /* remote control from the Jellyfin app during playback */
static bool jf_next_ep(const JItem *j, JItem *out);

/* ---- skip intro / skip credits: Jellyfin's media segments (10.10+), otherwise the Intro Skipper plugin ---- */
static void add_marker(PlaySkip *sk, PlayCtxJ *c, double a, double e, bool credits) {
    if (e <= a || sk->n >= (int)(sizeof sk->m / sizeof sk->m[0])) return;
    sk->m[sk->n++] = (PlayMarker){a, e, credits};
    if (credits && (c->outro_start <= 0 || a < c->outro_start)) c->outro_start = a;
}
static void jf_segments(PlayCtxJ *c, PlaySkip *sk) {
    char *p = str_printf("/MediaSegments/%s?includeSegmentTypes=Intro&includeSegmentTypes=Recap&includeSegmentTypes=Outro", c->id);
    cJSON *j = jf_get_sync("GET", p, NULL, 5);
    free(p);
    cJSON *seg, *segs = j ? cJSON_GetObjectItem(j, "Items") : NULL;
    cJSON_ArrayForEach(seg, segs) {
        cJSON *ty = cJSON_GetObjectItem(seg, "Type");
        if (!cJSON_IsString(ty)) continue;
        bool credits = !strcmp(ty->valuestring, "Outro");
        if (!credits && strcmp(ty->valuestring, "Intro") && strcmp(ty->valuestring, "Recap")) continue;
        add_marker(sk, c, jnum(seg, "StartTicks") / TICKS, jnum(seg, "EndTicks") / TICKS, credits);
    }
    cJSON_Delete(j);
    if (sk->n) return;
    /* older servers: the Intro Skipper plugin */
    p = str_printf("/Episode/%s/Timestamps", c->id);
    j = jf_get_sync("GET", p, NULL, 4);
    free(p);
    if (j) {
        cJSON *in = cJSON_GetObjectItem(j, "Introduction"), *cr = cJSON_GetObjectItem(j, "Credits");
        if (cJSON_IsTrue(cJSON_GetObjectItem(in, "Valid"))) add_marker(sk, c, jnum(in, "Start"), jnum(in, "End"), false);
        if (cJSON_IsTrue(cJSON_GetObjectItem(cr, "Valid"))) add_marker(sk, c, jnum(cr, "Start"), jnum(cr, "End"), true);
        cJSON_Delete(j);
    }
    if (sk->n) return;
    p = str_printf("/Episode/%s/IntroTimestamps/v1", c->id);
    j = jf_get_sync("GET", p, NULL, 4);
    free(p);
    if (j && cJSON_IsTrue(cJSON_GetObjectItem(j, "Valid"))) add_marker(sk, c, jnum(j, "IntroStart"), jnum(j, "IntroEnd"), false);
    cJSON_Delete(j);
}
static void jf_skip_info(void *ud, PlaySkip *sk) {
    PlayCtxJ *c = ud;
    jf_segments(c, sk);
    if (!c->has_next && c->it && jf_next_ep(c->it, &c->next)) c->has_next = true;
    if (c->has_next) {
        sk->has_next = true;
        snprintf(sk->next_title, sizeof sk->next_title, "S%d E%d%s%s", c->next.parent_index, c->next.index,
                 c->next.name ? " - " : "", c->next.name ? c->next.name : "");
    }
}

/* ---- audio and subtitles the way the user has set them up in Jellyfin (language, "smart"/"forced only" ...):
   the server works out the default streams for the user; here they are turned into mpv's track numbers ---- */
typedef struct { int aid, sid; char **subs; int n_subs; int hdr, w; } JStreams; /* hdr: 0 unknown, 1 SDR, 2 HDR */
static void jstreams_free(JStreams *t) {
    for (int i = 0; i < t->n_subs; i++) free(t->subs[i]);
    free(t->subs);
}
static void jf_streams(const JItem *j, JStreams *t) {
    char *p = str_printf("/Items/%s/PlaybackInfo?userId=%s", j->id, jf_uid());
    cJSON *info = jf_get_sync("GET", p, NULL, 6);
    if (!info) info = jf_get_sync("POST", p, "{}", 6); /* newer servers: only POST */
    free(p);
    cJSON *ms = info ? cJSON_GetArrayItem(cJSON_GetObjectItem(info, "MediaSources"), 0) : NULL;
    if (ms) {
        cJSON *da = cJSON_GetObjectItem(ms, "DefaultAudioStreamIndex"), *ds = cJSON_GetObjectItem(ms, "DefaultSubtitleStreamIndex");
        int def_a = cJSON_IsNumber(da) ? da->valueint : -1, def_s = cJSON_IsNumber(ds) ? ds->valueint : -1;
        char *msid = js(ms, "Id");
        int n_audio = 0, n_emb = 0, n_ext = 0, sel_emb = 0, sel_ext = 0;
        cJSON *st;
        cJSON_ArrayForEach(st, cJSON_GetObjectItem(ms, "MediaStreams")) {
            cJSON *ty = cJSON_GetObjectItem(st, "Type");
            int idx = (int)jnum(st, "Index");
            if (!cJSON_IsString(ty)) continue;
            if (!strcmp(ty->valuestring, "Video") && !t->hdr) { /* HDR and width: the player then skips ffprobe */
                char *trc = js(st, "ColorTransfer"), *vr = js(st, "VideoRange");
                bool hdr = (trc && (!strcmp(trc, "smpte2084") || !strcmp(trc, "arib-std-b67"))) || (vr && !strcmp(vr, "HDR"));
                if (trc || vr) t->hdr = hdr ? 2 : 1;
                t->w = (int)jnum(st, "Width");
                free(trc);
                free(vr);
            }
            if (!strcmp(ty->valuestring, "Audio")) {
                n_audio++;
                if (idx == def_a) t->aid = n_audio;
            } else if (!strcmp(ty->valuestring, "Subtitle")) {
                bool ext = cJSON_IsTrue(cJSON_GetObjectItem(st, "IsExternal"));
                bool text = !cJSON_IsFalse(cJSON_GetObjectItem(st, "IsTextSubtitleStream"));
                if (!ext) { n_emb++; if (idx == def_s) sel_emb = n_emb; }
                else if (text && msid) { /* a file next to the video: mpv gets it as a subtitle file */
                    char *cod = js(st, "Codec");
                    const char *fmt = cod && (!strcmp(cod, "ass") || !strcmp(cod, "ssa")) ? "ass" : cod && !strcmp(cod, "webvtt") ? "vtt" : "srt";
                    t->subs = realloc(t->subs, sizeof(char *) * (t->n_subs + 1));
                    t->subs[t->n_subs++] = str_printf("%s/Videos/%s/%s/Subtitles/%d/0/Stream.%s?ApiKey=%s", jf_url(), j->id, msid, idx, fmt, jf_token());
                    n_ext++;
                    if (idx == def_s) sel_ext = n_ext;
                    free(cod);
                }
            }
        }
        t->sid = sel_emb ? sel_emb : sel_ext ? n_emb + sel_ext : 0;
        free(msid);
    }
    cJSON_Delete(info);
}

/* OpenSubtitles search in the player menu (the shell does the search and the download) */
static int jf_sub_search(void *ud, char ***labels) {
    JItem *j = ((PlayCtxJ *)ud)->it;
    if (is_ep(j) && j->series && j->parent_index > 0) return subs_search(0, j->series, j->parent_index, j->index, labels);
    char q[300];
    snprintf(q, sizeof q, "%s %s", j->name ? j->name : "", j->year ? j->year : "");
    return subs_search(j->tmdb, q, 0, 0, labels);
}
static char *jf_sub_fetch(void *ud, int idx) { (void)ud; return subs_fetch(idx); }

/* true = played to the end (or "Next episode" was pressed). watched (optional): counted as watched. next (optional):
   receives the next episode when it is known, also when playback was stopped early. */
static bool jf_play(JItem *j, bool resume, bool *watched, JItem *next) {
    if (!j->id) return false;
    if (j->type && (!strcmp(j->type, "Series") || !strcmp(j->type, "Season") || !strcmp(j->type, "BoxSet"))) return false;
    /* login both as a header (mpv) and ApiKey in the address (ffprobe for the frame rate/HDR check);
       "-append" so the commas in the header are not split up as a list */
    char *url = str_printf("%s/Videos/%s/stream?static=true&ApiKey=%s", jf_url(), j->id, jf_token());
    char dn[128];
    char *hdr = str_printf("--http-header-fields-append=Authorization: MediaBrowser Client=\"NotaTV\", Device=\"%s\", "
                           "DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"", jf_devname(dn, sizeof dn), device_id(), TV_VERSION, jf_token());
    /* the tracks chosen by Jellyfin; subtitles: cfg jf_subs = jellyfin (default) | player (language setting) | off */
    JStreams ts = {0};
    jf_streams(j, &ts);
    const char *sm = cfg_get("jf_subs", "jellyfin");
    char **extra = calloc(ts.n_subs + 5, sizeof(char *));
    int ne = 0;
    extra[ne++] = hdr;
    if (ts.aid > 0) extra[ne++] = str_printf("--aid=%d", ts.aid);
    if (strcmp(sm, "off")) for (int i = 0; i < ts.n_subs; i++) extra[ne++] = str_printf("--sub-file=%s", ts.subs[i]);
    if (strcmp(sm, "player")) {
        if (!strcmp(sm, "jellyfin") && ts.sid > 0) extra[ne++] = str_printf("--sid=%d", ts.sid);
        else extra[ne++] = str_dup("--sid=no");
    }
    char title[300], sub[300];
    item_label(j, title, sizeof title, sub, sizeof sub);
    char *poster = poster_url(j);
    PlayCtxJ c = {0};
    snprintf(c.id, sizeof c.id, "%s", j->id);
    c.it = j;
    PlayRequest rq = {0};
    rq.url = url;
    rq.title = title;
    rq.subtitle = sub;
    rq.description = j->overview;
    rq.poster = poster;
    rq.start = resume && j->pos > 30 ? j->pos : 0;
    rq.progress = on_progress;
    rq.tick = remote_play_tick;
    rq.skip_info = is_ep(j) || (j->type && !strcmp(j->type, "Movie")) ? jf_skip_info : NULL;
    rq.extra = (const char *const *)extra;
    rq.sub_search = is_ep(j) || (j->type && !strcmp(j->type, "Movie")) ? jf_sub_search : NULL;
    rq.sub_fetch = jf_sub_fetch;
    rq.ud = &c;
    rq.media_hdr = ts.hdr;
    rq.media_w = ts.w;
    report("/Sessions/Playing", c.id, rq.start, false);
    PlayResult res = player_run(&rq);
    /* the video never started (could not be loaded, or Back while loading): report the position it had, never 0 -
       0 would make the server forget where you were, and the item would drop out of "Continue watching" */
    bool started = res.dur > 0;
    double pos = res.pos > 0 ? res.pos : c.last > 0 ? c.last : rq.start;
    if (!started) pos = j->pos;
    report_sync("/Sessions/Playing/Stopped", c.id, pos);
    /* watched to the end, into the credits or past 90 %: mark as watched */
    bool ended = started && res.eof; /* a video that never started has not "ended" (no jump to the next episode) */
    bool done = started && (ended || (c.outro_start > 0 && pos >= c.outro_start) || pos > res.dur * 0.9);
    if (done) {
        char *p = str_printf("/UserPlayedItems/%s?userId=%s", c.id, jf_uid());
        cJSON_Delete(jf_get_sync("POST", p, NULL, 4));
        free(p);
    }
    if (res.dur > 0) { /* keep the caller's item in step (so "Resume"/"Play" and the progress are right) */
        j->played = done;
        j->pos = done ? 0 : pos;
        if (done && watched) *watched = true;
    }
    if (next && c.has_next) { *next = c.next; c.has_next = false; }
    if (c.has_next) jitem_free(&c.next);
    for (int i = 0; i < ne; i++) free(extra[i]);
    free(extra);
    jstreams_free(&ts);
    free(url);
    free(poster);
    jf_play_gen++;
    return ended;
}

/* defined further down (the detail page): plays an episode and offers the next one */
static void jf_play_chain(const JItem *it, bool resume, JItem *back);

/* ---------- rows ---------- */
typedef struct {
    char *title, *path;
    JItem *it;
    int n, cap;
    bool loading, landscape, cont; /* cont: continue rows (not hero candidates) */
} JRow;

static void row_push(JRow *r, JItem *it) {
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 24; r->it = realloc(r->it, sizeof(JItem) * r->cap); }
    r->it[r->n++] = *it;
}
static void row_free(JRow *r) {
    for (int i = 0; i < r->n; i++) jitem_free(&r->it[i]);
    free(r->it); free(r->title); free(r->path);
    memset(r, 0, sizeof *r);
}

static void row_done(void *ud, cJSON *json, long status) {
    JRow *r = ud;
    r->loading = false;
    if (status != 200 || !json) return;
    cJSON *arr = cJSON_IsArray(json) ? json : cJSON_GetObjectItem(json, "Items"), *o;
    cJSON_ArrayForEach(o, arr) {
        JItem it;
        jitem_parse(&it, o);
        if (!it.id) { jitem_free(&it); continue; }
        row_push(r, &it);
    }
}

#define MAX_ROWS 10
#define GCOLS 6
enum { V_HOME, V_GRID };
typedef struct { char *id, *name; } Lib;

typedef struct {
    int state; /* 0 setup, 1 connecting, 2 ready, 3 error */
    char msg[200];
    JRow rows[MAX_ROWS];
    int n_rows, row, col, view;
    float sy, sx[MAX_ROWS];
    SideMenu side;
    Lib libs[16];
    int n_libs;
    char grid_title[200];
    int btn; /* button in focus on the login screen */
    /* the home screen in Plex style (cfg plex_home_style: hero | focus) */
    bool hero;         /* focus is on the hero */
    int hero_idx;
    double hero_t;
    float hero_fade;
    char foc_key[80];
    char *foc_art, *prev_art;
    Layer *bgl;
    char bgl_key[100];
    float bgl_fade;
    int gen;           /* jf_play_gen when the home rows were fetched */
} Jf;

#define HERO_H 600
#define FOCUS_H 345
#define BG_H 560
static bool home_style(const char *w) { return !strcmp(cfg_get("plex_home_style", "hero"), w); }
static bool hero_on(const Jf *v) { return v->view == V_HOME && home_style("hero"); }
static bool focus_on(const Jf *v) { return v->view == V_HOME && home_style("focus"); }

static Jf *live;
static Screen *jf_detail(const JItem *it);

static void free_rows(Jf *v) {
    for (int i = 0; i < v->n_rows; i++) row_free(&v->rows[i]);
    v->n_rows = 0;
    v->row = v->col = 0;
    v->sy = 0;
    jf_gen++;
}

static JRow *add_row(Jf *v, const char *title, const char *path, bool landscape) {
    if (v->n_rows >= MAX_ROWS) return NULL;
    JRow *r = &v->rows[v->n_rows];
    memset(r, 0, sizeof *r);
    v->sx[v->n_rows] = 0;
    v->n_rows++;
    r->title = str_dup(title);
    r->path = str_dup(path);
    r->loading = true;
    r->landscape = landscape;
    jf_call("GET", path, NULL, row_done, r);
    return r;
}

#define FIELDS "Fields=Overview,ProductionYear,ChildCount,ProviderIds,OfficialRating,CommunityRating,CriticRating&EnableImageTypes=Primary,Backdrop,Logo"

static void show_home(Jf *v) {
    free_rows(v);
    v->view = V_HOME;
    v->hero = hero_on(v); /* the home screen starts on the hero */
    char p[600];
    snprintf(p, sizeof p, "/UserItems/Resume?userId=%s&Limit=24&MediaTypes=Video&" FIELDS, jf_uid());
    JRow *cr = add_row(v, T("Continue watching"), p, true);
    if (cr) cr->cont = true;
    snprintf(p, sizeof p, "/Shows/NextUp?userId=%s&Limit=24&" FIELDS, jf_uid());
    cr = add_row(v, T("Next episode"), p, true);
    if (cr) cr->cont = true;
    for (int i = 0; i < v->n_libs && v->n_rows < MAX_ROWS; i++) {
        char t[200];
        snprintf(t, sizeof t, T("Recently added · %s"), v->libs[i].name);
        snprintf(p, sizeof p, "/Items/Latest?userId=%s&ParentId=%s&Limit=24&" FIELDS, jf_uid(), v->libs[i].id);
        add_row(v, t, p, false);
    }
}

static void show_grid(Jf *v, const char *title, const char *path) {
    free_rows(v);
    v->view = V_GRID;
    snprintf(v->grid_title, sizeof v->grid_title, "%s", title);
    add_row(v, title, path, false);
}

static void show_lib(Jf *v, int li) {
    char p[600];
    snprintf(p, sizeof p, "/Items?userId=%s&ParentId=%s&Recursive=true&IncludeItemTypes=Movie,Series,BoxSet,Video"
             "&SortBy=SortName&SortOrder=Ascending&Limit=600&" FIELDS, jf_uid(), v->libs[li].id);
    show_grid(v, v->libs[li].name, p);
}

/* side menu: 0 Home, 1 Search, 2.. libraries, last Log out */
static void build_side(Jf *v) {
    side_clear(&v->side);
    snprintf(v->side.subtitle, sizeof v->side.subtitle, "%s", cfg_get("jf_name", ""));
    side_add(&v->side, T("Home"), ICON_STAR, false);
    side_add(&v->side, T("Search"), ICON_SEARCH, false);
    for (int i = 0; i < v->n_libs; i++) side_add(&v->side, v->libs[i].name, ICON_FOLDER, i == 0);
    if (*jf_token()) { /* only when signed in */
        side_add(&v->side, T("Change server"), ICON_SEARCH, true);
        side_add(&v->side, T("Sign out"), ICON_LOCK, false);
    }
}

static void views_done(void *ud, cJSON *json, long status) {
    Jf *v = ud;
    if (v != live) return;
    if (status == 401) { v->state = 3; snprintf(v->msg, sizeof v->msg, T("Login expired - sign in again")); return; }
    if (status != 200 || !json) {
        v->state = 3;
        if (status) snprintf(v->msg, sizeof v->msg, T("The server responded with error %ld"), status);
        else snprintf(v->msg, sizeof v->msg, T("Cannot reach %s"), jf_url());
        return;
    }
    for (int i = 0; i < v->n_libs; i++) { free(v->libs[i].id); free(v->libs[i].name); }
    v->n_libs = 0;
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(json, "Items")) {
        char *ct = js(o, "CollectionType");
        /* video libraries only */
        bool ok = !ct || !strcmp(ct, "movies") || !strcmp(ct, "tvshows") || !strcmp(ct, "homevideos") || !strcmp(ct, "boxsets");
        free(ct);
        if (!ok || v->n_libs >= 16) continue;
        v->libs[v->n_libs].id = js(o, "Id");
        v->libs[v->n_libs].name = js(o, "Name");
        if (v->libs[v->n_libs].id && v->libs[v->n_libs].name) v->n_libs++;
    }
    v->state = 2;
    build_side(v);
    show_home(v);
}

static void jf_connect(Jf *v) {
    if (!*jf_url() || !*jf_token()) { v->state = 0; return; }
    v->state = 1;
    snprintf(v->msg, sizeof v->msg, T("Connecting to Jellyfin…"));
    char *p = str_printf("/UserViews?userId=%s", jf_uid());
    jf_call("GET", p, NULL, views_done, v);
    free(p);
}

/* ---------- login: address -> username -> password ---------- */
static char login_user[128];
static void auth_done(void *ud, cJSON *json, long status) {
    Jf *v = ud;
    if (v != live) return;
    if (status == 200 && json) {
        char *tok = js(json, "AccessToken");
        cJSON *u = cJSON_GetObjectItem(json, "User");
        char *uid = js(u, "Id"), *name = js(u, "Name");
        if (tok && uid) {
            cfg_set("jf_token", tok);
            cfg_set("jf_uid", uid);
            cfg_set("jf_name", name ? name : login_user);
            app_toast(T("Signed in to Jellyfin"));
            jf_connect(v);
        } else { v->state = 3; snprintf(v->msg, sizeof v->msg, T("Unexpected response from the server")); }
        free(tok); free(uid); free(name);
        return;
    }
    v->state = 3;
    if (status == 401) snprintf(v->msg, sizeof v->msg, T("Wrong username or password"));
    else if (status == 0) snprintf(v->msg, sizeof v->msg, T("Cannot reach %s"), jf_url());
    else snprintf(v->msg, sizeof v->msg, T("Login failed (HTTP %ld)"), status);
}
static void pw_done(void *ud, const char *pw) {
    Jf *v = ud;
    if (!pw || v != live) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "Username", login_user);
    cJSON_AddStringToObject(o, "Pw", pw);
    char *b = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    cfg_set("jf_token", "");
    v->state = 1;
    snprintf(v->msg, sizeof v->msg, T("Signing in…"));
    jf_call("POST", "/Users/AuthenticateByName", b, auth_done, v);
    free(b);
}
static void user_done(void *ud, const char *u) {
    if (!u || !*u) return;
    snprintf(login_user, sizeof login_user, "%s", u);
    keyboard_open(T("Jellyfin: password"), "", true, pw_done, ud);
}
static void url_done(void *ud, const char *text) {
    if (!text || !*text) return;
    char *t = str_dup(text);
    str_trim(t);
    size_t n = strlen(t);
    while (n && t[n - 1] == '/') t[--n] = 0;
    /* without a scheme: IP address/local name -> http with Jellyfin's default port (8096) unless a port is
       given; domain names (e.g. demo.jellyfin.org/stable) -> https */
    char *u;
    if (strstr(t, "://")) u = str_dup(t);
    else {
        size_t hl = strcspn(t, ":/");
        bool ip = true, dot = false;
        for (size_t i = 0; i < hl; i++) { if (t[i] == '.') dot = true; else if (t[i] < '0' || t[i] > '9') ip = false; }
        bool local = ip || !dot;
        if (!local) u = str_printf("https://%s", t);
        else if (t[hl] == ':') u = str_printf("http://%s", t);
        else u = str_printf("http://%.*s:8096%s", (int)hl, t, t + hl);
    }
    cfg_set("jf_url", u);
    free(t);
    free(u);
    keyboard_open(T("Jellyfin: username"), cfg_get("jf_name", ""), false, user_done, ud);
}
static void start_login(Jf *v) {
    keyboard_open(T("Jellyfin address (e.g. 192.168.1.10:8096)"), jf_url(), false, url_done, v);
}

/* ---------- find servers on the network ---------- */
static DiscServer found[16];
static int n_found;
static void found_pick(void *ud, int idx) {
    Jf *v = ud;
    if (v != live || idx < 0) return;
    if (idx >= n_found) { start_login(v); return; } /* "Enter address" */
    cfg_set("jf_url", found[idx].url);
    keyboard_open(T("Jellyfin: username"), cfg_get("jf_name", ""), false, user_done, v);
}
static void disc_done(void *ud, const DiscServer *srv, int n) {
    Jf *v = ud;
    if (v != live) return;
    v->state = *jf_token() ? 3 : 0;
    snprintf(v->msg, sizeof v->msg, "%s", n ? "" : T("No Jellyfin servers found on the network"));
    if (!n) { app_toast(T("No Jellyfin servers found - enter the address")); return; }
    n_found = n < 16 ? n : 16;
    const char *items[17];
    char lbl[16][300];
    for (int i = 0; i < n_found; i++) {
        found[i] = srv[i];
        snprintf(lbl[i], sizeof lbl[i], "%s   ·   %s", srv[i].name, srv[i].url + 7);
        items[i] = lbl[i];
    }
    items[n_found] = T("Enter address manually");
    choice_open(n_found == 1 ? T("Found 1 Jellyfin server") : T("Jellyfin servers on the network"), items, n_found + 1, 0, found_pick, v);
}
static void find_servers(Jf *v) {
    v->state = 1;
    snprintf(v->msg, sizeof v->msg, T("Searching for Jellyfin servers on the network…"));
    netdisc_start(DISC_JELLYFIN, disc_done, v);
}

/* ---------- log out ---------- */
/* sign out: the token is invalidated on the server, and the server, user and login are forgotten here */
/* ---------- the row on the home screen: what you can continue (like Plex's On Deck) ----------
   Resume (started, newest first) and then Next Up (the next episode of series you watch), one per series, max 12.
   Fetched when the home screen opens and after playback (row_refresh); OK plays from where you stopped. */
#define HOME_MAX 12
static JItem home_it[HOME_MAX];
static HomeItem home_rows[HOME_MAX];
static int n_home, home_gen, home_pending;
static JItem *home_got[2]; /* the replies: 0 = Resume, 1 = Next Up */
static int n_home_got[2];

static void home_got_free(void) {
    for (int w = 0; w < 2; w++) {
        for (int i = 0; i < n_home_got[w]; i++) jitem_free(&home_got[w][i]);
        free(home_got[w]);
        home_got[w] = NULL;
        n_home_got[w] = 0;
    }
}

static void home_clear(void) {
    for (int i = 0; i < n_home; i++) {
        jitem_free(&home_it[i]);
        free(home_rows[i].title); free(home_rows[i].subtitle); free(home_rows[i].image); free(home_rows[i].key); free(home_rows[i].logo);
    }
    memset(home_rows, 0, sizeof home_rows);
    n_home = 0;
}

static bool home_has_series(const JItem *list, int n, const JItem *j) {
    for (int i = 0; i < n; i++) {
        if (list[i].id && j->id && !strcmp(list[i].id, j->id)) return true;
        if (list[i].series_id && j->series_id && !strcmp(list[i].series_id, j->series_id)) return true;
    }
    return false;
}

static void home_build(void) {
    home_clear();
    for (int w = 0; w < 2; w++)
        for (int i = 0; i < n_home_got[w]; i++) {
            JItem *j = &home_got[w][i];
            if (n_home >= HOME_MAX || !j->id || home_has_series(home_it, n_home, j)) continue;
            home_it[n_home] = *j;
            memset(j, 0, sizeof *j); /* moved */
            const JItem *e = &home_it[n_home];
            HomeItem *h = &home_rows[n_home];
            char t[300], st[300];
            item_label(e, t, sizeof t, st, sizeof st);
            h->title = str_dup(t);
            h->subtitle = str_dup(st);
            h->image = backdrop_url(e, 1280);
            h->logo = logo_url(e);
            h->key = str_printf("jf:%s", e->id);
            h->progress = e->run > 0 ? e->pos / e->run : 0;
            n_home++;
        }
    home_got_free();
}

/* ud: (gen << 1) | which (0 = Resume, 1 = Next Up) */
static void home_cb(void *ud, cJSON *json, long status) {
    intptr_t u = (intptr_t)ud;
    int w = (int)(u & 1);
    home_pending--;
    if ((u >> 1) != home_gen) return; /* signed out meanwhile */
    if (status == 200 && json) {
        cJSON *arr = cJSON_IsArray(json) ? json : cJSON_GetObjectItem(json, "Items"), *o;
        home_got[w] = calloc(cJSON_GetArraySize(arr) + 1, sizeof(JItem));
        cJSON_ArrayForEach(o, arr) jitem_parse(&home_got[w][n_home_got[w]++], o);
    }
    if (home_pending == 0) home_build();
}

static void jellyfin_row_refresh(void) {
    if (!*jf_token() || !*jf_uid() || home_pending > 0) return;
    home_gen++;
    home_got_free();
    char p[600];
    snprintf(p, sizeof p, "/UserItems/Resume?userId=%s&Limit=12&MediaTypes=Video&" FIELDS, jf_uid());
    home_pending++;
    jf_call_ex("GET", p, NULL, home_cb, (void *)(intptr_t)(home_gen << 1), true);
    snprintf(p, sizeof p, "/Shows/NextUp?userId=%s&Limit=12&" FIELDS, jf_uid());
    home_pending++;
    jf_call_ex("GET", p, NULL, home_cb, (void *)(intptr_t)((home_gen << 1) | 1), true);
}

static int jellyfin_row(HomeItem **items) {
    *items = home_rows;
    return *jf_token() ? n_home : 0;
}

static void jellyfin_row_open(const HomeItem *it, int idx) {
    (void)idx;
    for (int i = 0; it->key && i < n_home; i++)
        if (!strcmp(it->key + 3, home_it[i].id)) { jf_play_chain(&home_it[i], true, NULL); return; }
}

void jellyfin_forget(void) {
    if (*jf_token()) cJSON_Delete(jf_get_sync("POST", "/Sessions/Logout", "", 4));
    cfg_set("jf_token", "");
    cfg_set("jf_uid", "");
    cfg_set("jf_url", "");
    cfg_set("jf_name", "");
    home_gen++; /* replies on the way are dropped */
    home_got_free();
    home_clear();
    wipe_session_traces();
}
static void logout_cb(void *ud, bool yes) {
    Jf *v = ud;
    if (!yes || v != live) return;
    jellyfin_forget();
    free_rows(v);
    for (int i = 0; i < v->n_libs; i++) { free(v->libs[i].id); free(v->libs[i].name); }
    v->n_libs = 0;
    v->state = 0;
    v->btn = 0;
    v->msg[0] = 0;
    v->side.active = 0;
    build_side(v);
    app_toast(T("Signed out of Jellyfin"));
}

/* buttons on the login screen (depend on the state) */
static int login_buttons(Jf *v, const char **b) {
    if (v->state == 3 && *jf_token()) { b[0] = T("Try again"); b[1] = T("Sign in again"); b[2] = T("Find server"); return 3; }
    b[0] = T("Find server on the network"); b[1] = T("Enter address"); return 2;
}
static void login_press(Jf *v) {
    const char *b[4];
    login_buttons(v, b);
    const char *l = b[v->btn];
    if (!strcmp(l, T("Try again"))) jf_connect(v);
    else if (!strncmp(l, T("Find server"), strlen(T("Find server")))) find_servers(v);
    else start_login(v);
}

/* ---------- main screen ---------- */
static void search_done(void *ud, const char *q) {
    Jf *v = ud;
    if (!q || !*q || v != live) return;
    char *e = url_encode(q);
    char *p = str_printf("/Items?userId=%s&SearchTerm=%s&Recursive=true&IncludeItemTypes=Movie,Series,Episode&Limit=120&" FIELDS,
                         jf_uid(), e);
    char *t = str_printf(T("Results for \"%s\""), q);
    show_grid(v, t, p);
    free(e); free(p); free(t);
}

static void side_activate(Jf *v, int idx) {
    v->side.active = idx;
    if (idx == 0) show_home(v);
    else if (idx == 1) keyboard_open(T("Search Jellyfin"), "", false, search_done, v);
    else if (idx - 2 < v->n_libs) show_lib(v, idx - 2);
    else if (idx - 2 == v->n_libs) { /* Skift server */
        free_rows(v);
        v->state = 0;
        v->btn = 0;
        v->msg[0] = 0;
    } else confirm_open(T("Sign out of Jellyfin?"), T("You will need to sign in again to watch your movies and shows."), T("Sign out"), T("Cancel"), logout_cb, v);
}

static void jf_enter(Screen *s) {
    Jf *v = s->st;
    live = v;
    v->gen = jf_play_gen;
    side_init(&v->side, "Jellyfin", ICON_FILM, JF_ACCENT);
    build_side(v);
    jf_connect(v);
}

static void jf_resume(Screen *s) {
    Jf *v = s->st;
    live = v;
    v->gen = jf_play_gen; /* fetched here: jf_update must not fetch everything once more */
    if (v->state == 2 && v->view == V_HOME) show_home(v); /* refresh "continue" after playback */
}

static void jf_destroy(Screen *s) {
    Jf *v = s->st;
    free_rows(v);
    free(v->foc_art);
    free(v->prev_art);
    layer_free(v->bgl);
    for (int i = 0; i < v->n_libs; i++) { free(v->libs[i].id); free(v->libs[i].name); }
    if (live == v) live = NULL;
    free(v);
    free(s);
}

static float row_iw(const JRow *r) { return r->landscape ? 440 : 250; }
static float row_ih(const JRow *r) { return r->landscape ? 248 : 375; }
static bool row_hidden(const JRow *r) { return !r->loading && !r->n; } /* empty rows are hidden (e.g. no "continue") */

/* hero candidates: the first movies/series with a backdrop from the rows (not "continue") */
static int hero_items(Jf *v, JItem **out, int max) {
    int n = 0;
    for (int pass = 0; pass < 3 && n < max; pass++)
        for (int r = 0; r < v->n_rows && n < max; r++) {
            JRow *row = &v->rows[r];
            if (row->cont || pass >= row->n) continue;
            JItem *it = &row->it[pass];
            if (!it->backdrop_id || !it->type || (strcmp(it->type, "Movie") && strcmp(it->type, "Series"))) continue;
            bool dup = false;
            for (int k = 0; k < n && !dup; k++) dup = !strcmp(out[k]->id, it->id);
            if (!dup) out[n++] = it;
        }
    return n;
}
/* fetch backdrop and logo in advance, so they are ready when focus moves there */
static void prefetch_bg(const JItem *h) {
    char *u = backdrop_url(h, 1280);
    if (u) img_get(u);
    free(u);
    u = logo_url(h);
    if (u) img_get(u);
    free(u);
}

static void jf_update(Screen *s, float dt) {
    Jf *v = s->st;
    if (v->gen != jf_play_gen) { /* after a playback: "Continue" and "Next episode" are fetched again */
        v->gen = jf_play_gen;
        if (v->state == 2 && v->view == V_HOME) show_home(v);
    }
    side_update(&v->side, dt);
    float avail = UI_W - (SIDE_W + 60) - 120;
    if (v->view == V_GRID) {
        float t = (v->row - 1) * 480.0f;
        v->sy = anim_to(v->sy, t > 0 ? t : 0, dt, 12);
        return;
    }
    float ty = 0;
    for (int r = 0; r < v->row && r < v->n_rows; r++)
        if (!row_hidden(&v->rows[r])) ty += row_ih(&v->rows[r]) + 155;
    if (!focus_on(v)) ty -= 120;
    if (ty < 0) ty = 0;
    if (hero_on(v)) {
        ty = v->hero ? 0 : HERO_H + ty;
        if (v->hero && !v->side.open && app_time() - v->hero_t > 12) { v->hero_idx++; v->hero_t = app_time(); v->hero_fade = 0; }
        v->hero_fade = anim_to(v->hero_fade, 1, dt, 6);
    }
    if (focus_on(v)) v->hero_fade = anim_to(v->hero_fade, 1, dt, 7);
    v->sy = anim_to(v->sy, ty, dt, 12);
    for (int r = 0; r < v->n_rows; r++) {
        float iw = row_iw(&v->rows[r]), target = 0;
        if (r == v->row && !v->hero) {
            float sel = v->col * (iw + 30);
            if (sel > avail - iw) target = sel - (avail - iw);
        }
        v->sx[r] = anim_to(v->sx[r], target, dt, 12);
    }
}

/* badges on the tile: progress, new (triangle) or number of unwatched episodes - like in Plex */
static void draw_badges(const JItem *it, float xx, float yy, float ww, float hh) {
    bool video = it->type && (!strcmp(it->type, "Movie") || !strcmp(it->type, "Episode") || !strcmp(it->type, "Video"));
    if (it->run > 0 && it->pos > 0) {
        fill_rrect(xx + 12, yy + hh - 14, ww - 24, 6, 3, RGBA(0, 0, 0, 150));
        fill_rrect(xx + 12, yy + hh - 14, (ww - 24) * (float)(it->pos / it->run), 6, 3, JF_ACCENT);
    } else if (video && !it->played) {
        fill_tri(xx + ww - 34, yy, xx + ww, yy, xx + ww, yy + 34, JF_ACCENT);
    } else if (!video && it->unplayed > 0) {
        char n[16];
        snprintf(n, sizeof n, "%d", it->unplayed);
        float bw = text_width(F_BOLD, 20, n) + 18;
        fill_rrect(xx + ww - bw - 8, yy + 8, bw, 30, 15, JF_ACCENT);
        text_draw(F_BOLD, 20, xx + ww - bw / 2 - 8, yy + 12, RGB(20, 20, 20), AL_CENTER, n);
    }
}

static void draw_tile(const JItem *it, float x, float y, float w, float h, bool landscape, bool f) {
    float fa = ui_focus(it, f), sc = 1 + 0.07f * fa, ww = w * sc, hh = h * sc, xx = x - (ww - w) / 2, yy = y - (hh - h) / 2;
    ui_focus_under(xx, yy, ww, hh, 14, fa);
    fill_rrect(xx, yy, ww, hh, 14, C_SURFACE);
    char *u = landscape ? backdrop_url(it, 640) : poster_url(it);
    char t[256], sub[256];
    item_label(it, t, sizeof t, sub, sizeof sub);
    if (!img_draw_cover(img_get(u), xx, yy, ww, hh, 14, 1))
        text_draw_wrap(F_SEMI, 24, xx + 16, yy + 20, ww - 32, 4, 30, C_TEXT2, t);
    free(u);
    draw_badges(it, xx, yy, ww, hh);
    ui_focus_over(xx, yy, ww, hh, 14, fa);
    text_draw_clip(F_SEMI, 24, x, y + h + 14, w, f ? C_TEXT : C_TEXT2, AL_LEFT, t);
    if (sub[0]) text_draw_clip(F_REG, 22, x, y + h + 44, w, C_TEXT3, AL_LEFT, sub);
}

static void draw_hero(Jf *v, float x0, bool rail) {
    JItem *hs[8];
    int nh = hero_items(v, hs, 8);
    if (!nh || v->sy >= HERO_H + 100) return;
    JItem *h = hs[((v->hero_idx % nh) + nh) % nh];
    float off = v->sy, a = 1 - fminf(1, off / HERO_H);
    for (int i = 0; i < nh; i++) prefetch_bg(hs[i]);
    char *u = backdrop_url(h, 1280);
    Image *him = img_get(u);
    free(u);
    if (img_state(him) != IMG_READY) v->hero_fade = 0; /* the fade only starts when the image is ready */
    if (img_draw_cover(him, 0, -off * 0.5f, UI_W, 820, 0, a * v->hero_fade)) {
        fill_hgrad(0, 0, UI_W * 0.75f, 820, RGBA(8, 9, 13, (uint8_t)(235 * a)), RGBA(8, 9, 13, 0));
        fill_vgrad(0, 420 - off * 0.5f, UI_W, 400, RGBA(8, 9, 13, 0), RGBA(8, 9, 13, 255));
    }
    float hx = x0, hy = 200 - off;
    draw_jinfo(h, hx, hy, a, 3, false);
    bool f = v->hero && !rail;
    const char *lbl = h->pos > 30 ? T("Resume") : T("Play");
    float bw = text_width(F_SEMI, 28, lbl) + 110, by = hy + 380;
    fill_rrect(hx, by, bw, 72, 36, f ? C_FOCUS : RGBA(255, 255, 255, (uint8_t)(50 * a)));
    draw_icon(ICON_PLAY, hx + 42, by + 36, 26, f ? RGB(10, 10, 10) : color_alpha(C_TEXT, a));
    text_draw(F_SEMI, 28, hx + 70, by + 18, f ? RGB(10, 10, 10) : color_alpha(C_TEXT, a), AL_LEFT, lbl);
    if (f) text_draw(F_REG, 22, hx + bw + 30, by + 24, C_TEXT3, AL_LEFT, T("OK: info   ·   Play button: play   ·   Left/Right: more"));
    for (int i = 0; i < nh; i++) {
        bool cur = i == ((v->hero_idx % nh) + nh) % nh;
        fill_rrect(hx + i * 28, by + 100, cur ? 20 : 10, 10, 5, color_alpha(cur ? C_TEXT : C_TEXT3, a));
    }
}

/* "focus" style: the backdrop and info at the top follow the title you are on */
static void draw_focus(Jf *v, float x0, bool rail) {
    JItem *h = &v->rows[v->row].it[v->col];
    char key[80];
    snprintf(key, sizeof key, "%s", h->id);
    if (strcmp(key, v->foc_key)) { /* new title in focus: the old backdrop fades out, the new one in */
        snprintf(v->foc_key, sizeof v->foc_key, "%s", key);
        free(v->prev_art);
        v->prev_art = v->foc_art;
        v->foc_art = backdrop_url(h, 1280);
        v->hero_fade = 0;
    }
    JRow *cr = &v->rows[v->row];
    for (int d = -2; d <= 3; d++)
        if (d && v->col + d >= 0 && v->col + d < cr->n) prefetch_bg(&cr->it[v->col + d]);
    for (int d = -1; d <= 1; d += 2) {
        int r = v->row + d;
        if (r >= 0 && r < v->n_rows && v->rows[r].n) prefetch_bg(&v->rows[r].it[0]);
    }
    Image *ni = v->foc_art ? img_get(v->foc_art) : NULL, *pi = v->prev_art ? img_get(v->prev_art) : NULL;
    bool ready = ni && img_state(ni) == IMG_READY;
    if (!ready) v->hero_fade = 0;
    float fa = v->hero_fade;
    /* the backdrop is drawn into a layer and only redrawn when the title or the fade changes */
    if (!v->bgl) { v->bgl = layer_new(UI_W, UI_H); layer_set_opaque(v->bgl, true); }
    char lkey[100];
    snprintf(lkey, sizeof lkey, "%s|%d", v->foc_key, ready);
    bool settled = fa > 0.995f && v->bgl_fade > 0.995f;
    if ((!layer_valid(v->bgl) || strcmp(lkey, v->bgl_key) || !settled) && layer_begin(v->bgl)) {
        draw_background();
        if (pi && fa < 1) img_draw_cover(pi, 0, 0, UI_W, BG_H, 0, 1 - fa);
        if (ni) img_draw_cover(ni, 0, 0, UI_W, BG_H, 0, fa);
        fill_hgrad(0, 0, UI_W * 0.75f, BG_H, RGBA(8, 9, 13, 235), RGBA(8, 9, 13, 0));
        fill_vgrad(0, BG_H - 330, UI_W, 330, RGBA(12, 14, 20, 0), RGBA(12, 14, 20, 255));
        layer_end(v->bgl);
        snprintf(v->bgl_key, sizeof v->bgl_key, "%s", lkey);
        v->bgl_fade = fa;
    }
    layer_draw(v->bgl, 0, 0, 1);
    if (!rail) draw_jinfo(h, x0, 130, 1, 2, true);
}

static void jf_draw(Screen *s) {
    Jf *v = s->st;
    bool fcs = focus_on(v) && v->state == 2 && v->n_rows && v->row < v->n_rows && v->col < v->rows[v->row].n;
    if (!fcs) draw_background(); /* focus style: the backdrop is in the layer */
    draw_topbar(NULL);
    float x0 = SIDE_W + 60, cx = x0 + (UI_W - x0) / 2;
    if (v->state != 2) {
        draw_icon(ICON_FILM, cx, 300, 120, JF_ACCENT);
        if (v->state == 0) {
            text_draw(F_BOLD, 42, cx, 430, C_TEXT, AL_CENTER, T("Connect to Jellyfin"));
            text_draw_wrap(F_REG, 28, cx - 520, 500, 1040, 6, 40, C_TEXT2,
                           v->msg[0] ? v->msg
                                     : T("Watch movies and shows from your Jellyfin server.\n\nFind the server on the network, or enter the address yourself (e.g. 192.168.1.10:8096). Then your username and password - you can also type from your phone."));
        } else if (v->state == 1) {
            draw_spinner(cx, 520, 30, C_TEXT);
            text_draw(F_REG, 26, cx, 580, C_TEXT2, AL_CENTER, v->msg);
        } else {
            text_draw(F_BOLD, 42, cx, 430, C_TEXT, AL_CENTER, T("Could not connect"));
            text_draw(F_REG, 28, cx, 500, C_TEXT2, AL_CENTER, v->msg);
        }
        if (v->state != 1) {
            const char *b[4];
            int nb = login_buttons(v, b);
            draw_buttons(cx, 780, b, nb, v->side.open ? -1 : v->btn);
            text_draw(F_REG, 22, cx, UI_H - 70, C_TEXT3, AL_CENTER, T("Left (from the first button): menu"));
        }
        side_draw(&v->side);
        return;
    }
    bool rail = v->side.open, cf = !rail;
    if (v->view == V_GRID && v->n_rows) {
        clip_push(SIDE_W, 120, UI_W, UI_H - 120);
        JRow *r = &v->rows[0];
        float top = 140 - v->sy;
        text_draw(F_BOLD, 44, x0, top - 70, C_TEXT, AL_LEFT, v->grid_title);
        if (r->loading && !r->n) draw_spinner(cx, UI_H / 2, 30, C_TEXT);
        else if (!r->n) text_draw(F_REG, 26, x0, top + 60, C_TEXT3, AL_LEFT, T("Nothing found"));
        for (int i = 0; i < r->n; i++) {
            float x = x0 + (i % GCOLS) * 280, y = top + (i / GCOLS) * 480;
            if (y > UI_H || y + 480 < 0) continue;
            draw_tile(&r->it[i], x, y, 250, 375, false, cf && v->row == i / GCOLS && v->col == i % GCOLS);
        }
        clip_pop();
        side_draw(&v->side);
        return;
    }
    float y = 140 - v->sy;
    if (hero_on(v)) { draw_hero(v, x0, rail); y += HERO_H; }
    if (fcs) { draw_focus(v, x0, rail); y += FOCUS_H; }
    /* focus style: the rows must not scroll up over the info text */
    float clip_y = focus_on(v) ? 150 + FOCUS_H - 20 : 120;
    clip_push(SIDE_W, clip_y, UI_W, UI_H - clip_y);
    for (int r = 0; r < v->n_rows; r++) {
        JRow *row = &v->rows[r];
        float iw = row_iw(row), ih = row_ih(row);
        if (row_hidden(row)) continue;
        text_draw(F_SEMI, 30, x0, y, r == v->row && cf && !v->hero ? C_TEXT : C_TEXT2, AL_LEFT, row->title);
        float iy = y + 56;
        if (row->loading && !row->n) draw_spinner(x0 + 100, iy + ih / 2, 24, C_TEXT2);
        if (iy > UI_H + 50) break;
        for (int i = 0; i < row->n; i++) {
            float x = x0 + i * (iw + 30) - v->sx[r];
            if (x > UI_W || x + iw < SIDE_W) continue;
            draw_tile(&row->it[i], x, iy, iw, ih, row->landscape, cf && !v->hero && r == v->row && v->col == i);
        }
        y += ih + 155;
    }
    clip_pop();
    side_draw(&v->side);
}

/* rows without content are skipped on up/down */
static int next_row(Jf *v, int from, int dir) {
    for (int r = from + dir; r >= 0 && r < v->n_rows; r += dir)
        if (v->rows[r].n) return r;
    return from;
}

static void open_item(Jf *v, JItem *it, bool play) {
    if (it->type && (!strcmp(it->type, "BoxSet") || !strcmp(it->type, "Folder") || !strcmp(it->type, "CollectionFolder"))) {
        char *p = str_printf("/Items?userId=%s&ParentId=%s&SortBy=ProductionYear,SortName&" FIELDS, jf_uid(), it->id);
        char *t = str_dup(it->name ? it->name : T("Collection"));
        show_grid(v, t, p);
        free(p); free(t);
        return;
    }
    bool container = it->type && !strcmp(it->type, "Series");
    if (play && !container) jf_play_chain(it, true, NULL); /* the Play key: start directly */
    else app_push(jf_detail(it));
}

static bool jf_input(Screen *s, const InputEvent *ev) {
    Jf *v = s->st;
    int act;
    if (side_input(&v->side, ev, &act)) {
        if (act >= 0) side_activate(v, act);
        return true;
    }
    if (v->state != 2) {
        if (v->state == 1) return ev->a != A_BACK && ev->a != A_HOME;
        const char *b[4];
        int nb = login_buttons(v, b);
        if (buttons_nav(&v->btn, nb, ev)) return true;
        if (ev->a == A_OK) { if (!ev->repeat) login_press(v); return true; }
        if (ev->a == A_LEFT || ev->a == A_MENU) { side_open(&v->side); return true; }
        return false;
    }
    JRow *gr = v->view == V_GRID && v->n_rows ? &v->rows[0] : NULL;
    if (v->view == V_HOME && v->n_rows && !v->rows[v->row].n) { int r0 = next_row(v, -1, 1); if (r0 >= 0) v->row = r0; }
    if (v->hero && hero_on(v)) { /* hero i fokus */
        JItem *hs[8];
        int nh = hero_items(v, hs, 8);
        JItem *h = nh ? hs[((v->hero_idx % nh) + nh) % nh] : NULL;
        switch (ev->a) {
        case A_DOWN: { int r0 = next_row(v, -1, 1); if (r0 >= 0) { v->hero = false; v->row = r0; v->col = 0; } return true; }
        case A_UP: return true;
        case A_RIGHT: v->hero_idx++; v->hero_t = app_time(); v->hero_fade = 0; return true;
        case A_LEFT:
            if (v->hero_idx <= 0 || !nh) side_open(&v->side);
            else { v->hero_idx--; v->hero_t = app_time(); v->hero_fade = 0; }
            return true;
        case A_OK: if (h && !ev->repeat) open_item(v, h, false); return true;
        case A_PLAYPAUSE: case A_PLAY: if (h && !ev->repeat) open_item(v, h, true); return true;
        default: break;
        }
    }
    switch (ev->a) {
    case A_UP:
        if (gr) { if (v->row > 0) v->row--; }
        else {
            int r = next_row(v, v->row, -1);
            if (r != v->row) { v->row = r; v->col = 0; }
            else if (hero_on(v)) { v->hero = true; v->hero_t = app_time(); }
        }
        return true;
    case A_BACK:
        if (v->view != V_HOME) return false;
        if (hero_on(v) && !v->hero) { v->hero = true; v->row = v->col = 0; return true; }
        if (v->row > 0 || v->col > 0) { v->row = v->col = 0; return true; }
        return false;
    case A_DOWN:
        if (gr) {
            if ((v->row + 1) * GCOLS < gr->n) v->row++;
            if (v->row * GCOLS + v->col >= gr->n) v->col = (gr->n - 1) % GCOLS;
        } else { int r = next_row(v, v->row, 1); if (r != v->row) { v->row = r; v->col = 0; } }
        return true;
    case A_LEFT:
        if (v->col > 0) v->col--;
        else side_open(&v->side);
        return true;
    case A_RIGHT:
        if (gr) { if (v->col < GCOLS - 1 && v->row * GCOLS + v->col + 1 < gr->n) v->col++; }
        else if (v->n_rows && v->col < v->rows[v->row].n - 1) v->col++;
        return true;
    case A_MENU: side_open(&v->side); return true;
    case A_SEARCH: keyboard_open(T("Search Jellyfin"), "", false, search_done, v); return true;
    case A_OK: case A_PLAYPAUSE: case A_PLAY: {
        if (ev->repeat || !v->n_rows) return true;
        JRow *r = gr ? gr : &v->rows[v->row];
        int i = gr ? v->row * GCOLS + v->col : v->col;
        if (i >= r->n) return true;
        open_item(v, &r->it[i], ev->a != A_OK);
        return true;
    }
    default: return false;
    }
}

Screen *jellyfin_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "jellyfin";
    s->st = calloc(1, sizeof(Jf));
    s->enter = jf_enter;
    s->resume = jf_resume;
    s->update = jf_update;
    s->draw = jf_draw;
    s->input = jf_input;
    s->destroy = jf_destroy;
    return s;
}

/* ---------- detail page: movies/episodes (play) and series (seasons + episodes) ---------- */

/* the next episode of the show (all seasons, sorted). The result is a full, playable copy. */
static bool jf_next_ep(const JItem *j, JItem *out) {
    if (!is_ep(j) || !j->series_id || !*jf_url() || !*jf_token() || !*jf_uid()) return false;
    char *p = str_printf("/Shows/%s/Episodes?userId=%s&SortBy=ParentIndexNumber,IndexNumber&SortOrder=Ascending&" FIELDS,
                         j->series_id, jf_uid());
    char *url = str_printf("%s%s", jf_url(), p);
    char auth[512], dn[128];
    snprintf(auth, sizeof auth,
             "Authorization: MediaBrowser Client=\"NotaTV\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"",
             jf_devname(dn, sizeof dn), device_id(), TV_VERSION, jf_token());
    const char *hdr[] = {"Accept: application/json", auth, NULL};
    HttpBuf b;
    bool ok = false;
    if (http_request("GET", url, hdr, NULL, &b, 8) && b.data) {
        cJSON *root = cJSON_Parse(b.data);
        cJSON *items = root ? cJSON_GetObjectItem(root, "Items") : NULL;
        int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n; i++) {
            char *id = js(cJSON_GetArrayItem(items, i), "Id");
            bool cur = id && j->id && !strcmp(id, j->id);
            free(id);
            if (cur) {
                for (int k = i + 1; k < n; k++) {
                    JItem nx;
                    jitem_parse(&nx, cJSON_GetArrayItem(items, k));
                    if (is_ep(&nx)) { *out = nx; ok = true; break; }
                    jitem_free(&nx);
                }
                break;
            }
        }
        cJSON_Delete(root);
    }
    http_free(&b);
    free(url);
    free(p);
    return ok;
}

/* ---------- play chain: an episode, then the next one straight away ---------- */
typedef struct { JItem it; JItem *back; } JfChain;

static void jf_chain_free(JfChain *c) { jitem_free(&c->it); free(c); }
static void jf_chain_sync(JfChain *c) {
    if (c->back) {
        jitem_free(c->back);
        *c->back = c->it;
        memset(&c->it, 0, sizeof c->it);
    }
    jf_chain_free(c);
    app_request_redraw();
}

/* plays the episode; when it ends (or "Next episode" is pressed in the player) the next one follows without a pause.
   Stopping early (Back) leaves the menu as it is, but an episode watched into the credits moves "Play" on to the next. */
static void jf_chain_run(JfChain *c, bool resume) {
    for (;;) {
        JItem next;
        memset(&next, 0, sizeof next);
        bool watched = false;
        bool ended = jf_play(&c->it, resume, &watched, &next);
        if (!next.id) break;
        if (ended || watched) {
            jitem_free(&c->it);
            c->it = next;
            if (!ended) break; /* stopped in the credits: "Play" continues with the next episode */
            resume = false;
            continue;
        }
        jitem_free(&next);
        break;
    }
    jf_chain_sync(c);
}

/* Play an episode and go on to the next one when it ends. back (optional) is where the item ends up, so the
   caller's "Play" continues with the next episode if you stop in the credits. */
static void jf_play_chain(const JItem *it, bool resume, JItem *back) {
    JfChain *c = calloc(1, sizeof *c);
    c->it = jitem_copy(it);
    c->back = back;
    jf_chain_run(c, resume);
}

/* ---- detail page: a movie (buttons), or a series: the series at the top, Play/Continue, the seasons and the
   episodes of the chosen season. A series page is also opened from an episode (e.g. "Continue watching" or
   "Latest") or a season; all episodes are fetched at once and grouped by season (like in Plex). ---- */
#define JD_MAX_SEASONS 128
typedef struct {
    JItem it;
    bool series, have_info, placed;
    JRow eps;                               /* series: every episode, in season/episode order */
    int seasons[JD_MAX_SEASONS], n_seasons; /* season numbers, specials (0) last */
    int *sidx, n_sidx;                      /* episodes of the chosen season */
    int next;                               /* the episode Play/Continue plays (-1 = none) */
    char focus_id[64];                      /* opened from this episode */
    int want_season;                        /* opened from this season (-1 = the next episode's) */
    int focus;                              /* 0 buttons, 1 seasons, 2 episodes */
    int btn, scol, ecol;
    float ssx, esx;
    int gen; /* jf_play_gen when the episodes were fetched */
} JDetail;
static JDetail *live_d;

/* which episode Play means (same rules as Plex): the one in progress if it was watched last, otherwise the first
   unwatched after the one watched most recently, otherwise the first unwatched - specials only when nothing else */
static int jf_pick_next(const JItem *e, int n) {
    int prog = -1, last = -1;
    for (int i = 0; i < n; i++) {
        if (e[i].pos > 30 && !e[i].played && (prog < 0 || strcmp(e[i].last_played, e[prog].last_played) > 0)) prog = i;
        if (e[i].played && (last < 0 || strcmp(e[i].last_played, e[last].last_played) >= 0)) last = i;
    }
    if (prog >= 0 && (last < 0 || strcmp(e[prog].last_played, e[last].last_played) >= 0)) return prog;
    for (int pass = 0; pass < 2; pass++) {
        if (last >= 0)
            for (int i = last + 1; i < n; i++) if (!e[i].played && (pass || e[i].parent_index > 0)) return i;
        for (int i = 0; i < n; i++) if (!e[i].played && (pass || e[i].parent_index > 0)) return i;
    }
    for (int i = 0; i < n; i++) if (e[i].parent_index > 0) return i;
    return n ? 0 : -1;
}

static void jd_pick_season(JDetail *d, int si) {
    d->scol = si < 0 ? 0 : si >= d->n_seasons ? d->n_seasons - 1 : si;
    d->n_sidx = 0;
    if (!d->n_seasons) return;
    for (int i = 0; i < d->eps.n; i++) if (d->eps.it[i].parent_index == d->seasons[d->scol]) d->sidx[d->n_sidx++] = i;
}
static int jd_season_of(JDetail *d, int s) {
    for (int i = 0; i < d->n_seasons; i++) if (d->seasons[i] == s) return i;
    return -1;
}
static int jd_ep_pos(JDetail *d, const char *id) {
    for (int i = 0; id && *id && i < d->n_sidx; i++) if (d->eps.it[d->sidx[i]].id && !strcmp(d->eps.it[d->sidx[i]].id, id)) return i;
    return -1;
}

static void info_done(void *ud, cJSON *json, long status) {
    JDetail *d = ud;
    if (d != live_d || status != 200 || !json) return;
    cJSON *o = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "Items"), 0);
    if (!o) return;
    JItem it;
    jitem_parse(&it, o);
    if (!it.id) { jitem_free(&it); return; }
    jitem_free(&d->it);
    d->it = it;
    d->have_info = true;
}

static void eps_done(void *ud, cJSON *json, long status) {
    JDetail *d = ud;
    if (d != live_d) return;
    int keep_season = d->n_seasons ? d->seasons[d->scol] : -1;
    char keep_id[64] = "";
    if (d->ecol < d->n_sidx && d->eps.it[d->sidx[d->ecol]].id) snprintf(keep_id, sizeof keep_id, "%s", d->eps.it[d->sidx[d->ecol]].id);
    if (status != 200 || !json) { d->eps.loading = false; return; }
    row_free(&d->eps);
    row_done(&d->eps, json, status);
    int w = 0; /* only episodes, with the series filled in (for playback and "next episode") */
    for (int i = 0; i < d->eps.n; i++) {
        JItem *e = &d->eps.it[i];
        if (!is_ep(e)) { jitem_free(e); continue; }
        if (!e->series) e->series = str_dup(d->it.name);
        if (!e->series_id) e->series_id = str_dup(d->it.id);
        d->eps.it[w++] = *e;
    }
    d->eps.n = w;
    free(d->sidx);
    d->sidx = calloc(d->eps.n + 1, sizeof(int));
    d->n_seasons = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < d->eps.n; i++) {
            int s = d->eps.it[i].parent_index;
            if ((s == 0) != (pass == 1) || jd_season_of(d, s) >= 0 || d->n_seasons >= JD_MAX_SEASONS) continue;
            d->seasons[d->n_seasons++] = s;
        }
    d->next = jf_pick_next(d->eps.it, d->eps.n);
    if (!d->placed) {
        d->placed = true;
        int season = d->want_season >= 0 ? d->want_season : d->next >= 0 ? d->eps.it[d->next].parent_index : -1;
        int si = jd_season_of(d, season);
        jd_pick_season(d, si < 0 ? 0 : si);
        int pos = jd_ep_pos(d, d->focus_id);
        if (pos < 0 && d->next >= 0) pos = jd_ep_pos(d, d->eps.it[d->next].id);
        d->ecol = pos < 0 ? 0 : pos;
    } else {
        int si = jd_season_of(d, keep_season);
        jd_pick_season(d, si < 0 ? 0 : si);
        int pos = jd_ep_pos(d, keep_id);
        d->ecol = pos < 0 ? 0 : pos;
    }
    if (d->next < 0 && d->focus == 0) d->focus = 1;
}

static void jd_load(JDetail *d) {
    if (!d->series) return;
    d->gen = jf_play_gen;
    d->eps.loading = true;
    char *p = str_printf("/Shows/%s/Episodes?userId=%s&SortBy=ParentIndexNumber,IndexNumber&SortOrder=Ascending&" FIELDS, d->it.id, jf_uid());
    jf_call("GET", p, NULL, eps_done, d);
    free(p);
}

static void jd_enter(Screen *s) {
    JDetail *d = s->st;
    live_d = d;
    if (d->series && !d->have_info) { /* opened from an episode or a season: fetch the series itself */
        char *p = str_printf("/Items?userId=%s&Ids=%s&" FIELDS, jf_uid(), d->it.id);
        jf_call("GET", p, NULL, info_done, d);
        free(p);
    }
    jd_load(d);
}
static void jd_resume(Screen *s) {
    JDetail *d = s->st;
    live_d = d;
    if (d->gen != jf_play_gen) jd_load(d); /* progress, watched marks and the next episode after playback */
}

static void jd_destroy(Screen *s) {
    JDetail *d = s->st;
    if (live_d == d) live_d = NULL;
    jf_gen++; /* pending replies must not write into the freed page */
    jitem_free(&d->it);
    row_free(&d->eps);
    free(d->sidx);
    free(d);
    free(s);
}

static void season_label(int s, char *b, size_t n) {
    if (s == 0) snprintf(b, n, "%s", T("Specials"));
    else snprintf(b, n, T("Season %d"), s);
}
static float pill_w(int s) {
    char l[64];
    season_label(s, l, sizeof l);
    return text_width(F_SEMI, 26, l) + 60;
}
#define JD_PILL_Y 610.0f
#define JD_EP_Y 712.0f
#define JD_EP_W 384.0f
#define JD_EP_H 216.0f
#define JD_EP_GAP 28.0f

static void jd_update(Screen *s, float dt) {
    JDetail *d = s->st;
    if (d->series && d->gen != jf_play_gen) jd_load(d); /* after a playback */
    float avail = UI_W - 240, px = 0, t = 0;
    for (int i = 0; i < d->n_seasons; i++) {
        float w = pill_w(d->seasons[i]);
        if (i == d->scol && px + w > avail) t = px + w - avail;
        px += w + 16;
    }
    d->ssx = anim_to(d->ssx, t, dt, 12);
    float sel = d->ecol * (JD_EP_W + JD_EP_GAP);
    d->esx = anim_to(d->esx, sel > avail - JD_EP_W ? sel - (avail - JD_EP_W) : 0, dt, 12);
}

/* buttons: [Resume] [Start over | Play] [Trailer]; for a series they are about the next episode */
typedef enum { JB_RESUME, JB_PLAY, JB_START, JB_TRAILER } JBtn;
static const JItem *jd_target(const JDetail *d) {
    if (!d->series) return &d->it;
    return d->next >= 0 && d->next < d->eps.n ? &d->eps.it[d->next] : NULL;
}
static int jd_buttons(const JDetail *d, JBtn *k) {
    const JItem *t = jd_target(d);
    if (!t) return 0;
    int n = 0;
    if (t->pos > 30 && t->run > 0) { k[n++] = JB_RESUME; k[n++] = JB_START; }
    else k[n++] = JB_PLAY;
    if (d->it.type && !strcmp(d->it.type, "Movie")) k[n++] = JB_TRAILER;
    return n;
}

static void youtube_trailer(const JItem *it) {
    char q[400];
    snprintf(q, sizeof q, "ytdl://ytsearch1:%s %s trailer", it->name ? it->name : "", it->year ? it->year : "");
    char *t = str_printf(T("%s - trailer"), it->name ? it->name : "");
    PlayRequest rq = {0};
    rq.url = q;
    rq.title = t;
    static const char *extra[] = {"--ytdl-format=bv*[height<=1080][vcodec!^=av01]+ba/b", NULL};
    rq.extra = extra;
    app_toast(T("Finding trailer…"));
    player_run(&rq);
    free(t);
}

static void jd_draw(Screen *s) {
    JDetail *d = s->st;
    JItem *it = &d->it;
    draw_background();
    char *bu = backdrop_url(it, 1920);
    if (img_draw_cover(img_get(bu), 0, 0, UI_W, UI_H, 0, 0.5f)) {
        fill_hgrad(0, 0, UI_W * 0.8f, UI_H, RGBA(8, 9, 13, 250), RGBA(8, 9, 13, 0));
        fill_vgrad(0, UI_H * 0.4f, UI_W, UI_H * 0.6f, RGBA(8, 9, 13, 0), RGBA(8, 9, 13, 245));
    }
    free(bu);
    float x = 120;
    const JItem *fe = d->series && d->focus == 2 && d->ecol < d->n_sidx ? &d->eps.it[d->sidx[d->ecol]] : NULL;
    if (fe) { /* the episode in focus, with the series' logo */
        JItem tmp = *fe;
        if (!tmp.logo_id) tmp.logo_id = it->logo_id;
        draw_jinfo(&tmp, x, d->series ? 90 : 150, 1, 3, false);
    } else if (d->series && d->n_seasons) { /* the number of seasons from the episodes (not counting specials) */
        int ns = 0;
        for (int i = 0; i < d->n_seasons; i++) if (d->seasons[i] > 0) ns++;
        int cc = it->child_count;
        it->child_count = ns;
        draw_jinfo(it, x, 90, 1, 3, false);
        it->child_count = cc;
    } else draw_jinfo(it, x, d->series ? 90 : 150, 1, d->series ? 3 : 5, false);
    if (d->series && !d->n_seasons) {
        if (d->eps.loading) draw_spinner(UI_W / 2, JD_EP_Y + 60, 30, C_TEXT2);
        return;
    }
    /* buttons */
    JBtn k[4];
    int nb = jd_buttons(d, k);
    if (d->btn >= nb) d->btn = nb ? nb - 1 : 0;
    const JItem *t = jd_target(d);
    float bx = x, by = d->series ? 500 : 780;
    for (int b = 0; b < nb; b++) {
        char lb[96], dur[32];
        switch (k[b]) {
        case JB_RESUME:
            if (d->series) snprintf(lb, sizeof lb, T("Continue S%d E%d (%s)"), t->parent_index, t->index, fmt_duration(t->pos, dur, sizeof dur));
            else snprintf(lb, sizeof lb, T("Continue (%s)"), fmt_duration(t->pos, dur, sizeof dur));
            break;
        case JB_PLAY:
            if (d->series) snprintf(lb, sizeof lb, T("Play S%d E%d"), t->parent_index, t->index);
            else snprintf(lb, sizeof lb, T("Play"));
            break;
        case JB_START: snprintf(lb, sizeof lb, T("Start over")); break;
        default: snprintf(lb, sizeof lb, T("Trailer (YouTube)")); break;
        }
        bool f = d->focus == 0 && d->btn == b;
        float w = text_width(F_SEMI, 28, lb) + 110;
        fill_rrect(bx, by, w, 76, 38, f ? C_FOCUS : RGBA(255, 255, 255, 40));
        draw_icon(k[b] == JB_TRAILER ? ICON_FILM : ICON_PLAY, bx + 42, by + 38, 26, f ? RGB(10, 10, 10) : C_TEXT);
        text_draw(F_SEMI, 28, bx + 70, by + 20, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, lb);
        bx += w + 24;
    }
    if (!d->series) return;
    /* seasons */
    clip_push(0, JD_PILL_Y - 10, UI_W, 80);
    float px = x - d->ssx;
    for (int i = 0; i < d->n_seasons; i++) {
        char l[64];
        season_label(d->seasons[i], l, sizeof l);
        float w = pill_w(d->seasons[i]);
        bool chosen = i == d->scol, f = d->focus == 1 && chosen;
        if (px + w > 0 && px < UI_W) {
            fill_rrect(px, JD_PILL_Y, w, 58, 29, f ? C_FOCUS : chosen ? RGBA(255, 255, 255, 70) : RGBA(255, 255, 255, 22));
            text_draw(F_SEMI, 26, px + w / 2, JD_PILL_Y + 14, f ? RGB(10, 10, 10) : chosen ? C_TEXT : C_TEXT2, AL_CENTER, l);
        }
        px += w + 16;
    }
    clip_pop();
    /* episodes of the chosen season */
    clip_push(0, JD_EP_Y - 30, UI_W, UI_H - JD_EP_Y + 30);
    for (int i = 0; i < d->n_sidx; i++) {
        JItem *e = &d->eps.it[d->sidx[i]];
        float ex = x + i * (JD_EP_W + JD_EP_GAP) - d->esx, ey = JD_EP_Y;
        if (ex > UI_W || ex + JD_EP_W < 0) continue;
        bool f = d->focus == 2 && d->ecol == i;
        float fa = ui_focus(e, f), sc = 1 + 0.06f * fa, w = JD_EP_W * sc, h = JD_EP_H * sc, xx = ex - (w - JD_EP_W) / 2, yy = ey - (h - JD_EP_H) / 2;
        ui_focus_under(xx, yy, w, h, 14, fa);
        fill_rrect(xx, yy, w, h, 14, C_SURFACE);
        char *u = str_printf("%s/Items/%s/Images/Primary?maxWidth=640&quality=85", jf_url(), e->id);
        if (!img_draw_cover(img_get(u), xx, yy, w, h, 14, 1)) draw_icon(ICON_TV, xx + w / 2, yy + h / 2, 60, C_TEXT3);
        free(u);
        draw_badges(e, xx, yy, w, h);
        ui_focus_over(xx, yy, w, h, 14, fa);
        char et[300];
        snprintf(et, sizeof et, "%d. %s", e->index, e->name ? e->name : "");
        text_draw_clip(F_SEMI, 24, ex, ey + JD_EP_H + 14, JD_EP_W, f ? C_TEXT : C_TEXT2, AL_LEFT, et);
        if (e->run > 0) {
            char dm[32];
            snprintf(dm, sizeof dm, T("%d min"), (int)(e->run / 60 + 0.5));
            text_draw(F_REG, 22, ex, ey + JD_EP_H + 46, C_TEXT3, AL_LEFT, dm);
        }
    }
    clip_pop();
}

static bool jd_input(Screen *s, const InputEvent *ev) {
    JDetail *d = s->st;
    JItem *it = &d->it;
    JBtn k[4];
    int nb = jd_buttons(d, k);
    if (!d->series) {
        if (ev->a == A_LEFT) { if (d->btn > 0) d->btn--; return true; }
        if (ev->a == A_RIGHT) { if (d->btn < nb - 1) d->btn++; return true; }
        if ((ev->a == A_OK || ev->a == A_PLAYPAUSE || ev->a == A_PLAY) && !ev->repeat) {
            switch (k[d->btn < nb ? d->btn : 0]) {
            case JB_RESUME: jf_play_chain(it, true, it); break;
            case JB_PLAY: case JB_START: jf_play_chain(it, false, it); break;
            case JB_TRAILER: youtube_trailer(it); break;
            }
            d->btn = 0;
            return true;
        }
        return false;
    }
    if (!d->n_seasons) return false;
    switch (ev->a) {
    case A_UP:
        if (d->focus > 0) d->focus--;
        if (d->focus == 0 && !nb) d->focus = 1;
        return true;
    case A_DOWN:
        if (d->focus == 0) d->focus = 1;
        else if (d->focus == 1 && d->n_sidx) d->focus = 2;
        return true;
    case A_LEFT:
        if (d->focus == 0 && d->btn > 0) d->btn--;
        else if (d->focus == 1 && d->scol > 0) { jd_pick_season(d, d->scol - 1); d->ecol = 0; d->esx = 0; }
        else if (d->focus == 2 && d->ecol > 0) d->ecol--;
        return true;
    case A_RIGHT:
        if (d->focus == 0 && d->btn < nb - 1) d->btn++;
        else if (d->focus == 1 && d->scol < d->n_seasons - 1) { jd_pick_season(d, d->scol + 1); d->ecol = 0; d->esx = 0; }
        else if (d->focus == 2 && d->ecol < d->n_sidx - 1) d->ecol++;
        return true;
    case A_OK: case A_PLAYPAUSE: case A_PLAY:
        if (ev->repeat) return true;
        if (d->focus == 0 && nb) jf_play_chain(jd_target(d), k[d->btn] == JB_RESUME, NULL);
        else if (d->focus == 1 && d->n_sidx) d->focus = 2;
        else if (d->focus == 2 && d->ecol < d->n_sidx) jf_play_chain(&d->eps.it[d->sidx[d->ecol]], true, NULL);
        return true;
    default: return false;
    }
}

/* a movie/video, or a series page: from a series, a season (that season) or an episode (that episode in focus) */
static Screen *jf_detail(const JItem *src) {
    Screen *s = calloc(1, sizeof *s);
    JDetail *d = calloc(1, sizeof *d);
    d->want_season = -1;
    d->next = -1;
    bool ep = is_ep(src) && src->series_id, season = src->type && !strcmp(src->type, "Season") && src->series_id;
    if (ep || season) {
        memset(&d->it, 0, sizeof d->it);
        d->it.id = str_dup(src->series_id);
        d->it.type = str_dup("Series");
        d->it.name = str_dup(src->series);
        d->it.backdrop_id = str_dup(src->series_id);
        d->it.rt_critic = d->it.community10 = -1;
        if (ep) { snprintf(d->focus_id, sizeof d->focus_id, "%s", src->id ? src->id : ""); d->want_season = src->parent_index; }
        else d->want_season = src->index;
    } else {
        d->it = jitem_copy(src);
        d->have_info = true;
    }
    d->series = d->it.type && !strcmp(d->it.type, "Series");
    s->name = "jellyfin-detail";
    s->st = d;
    s->enter = jd_enter;
    s->resume = jd_resume;
    s->update = jd_update;
    s->draw = jd_draw;
    s->input = jd_input;
    s->destroy = jd_destroy;
    return s;
}

/* ================= Remote control: "Play on → NotaTV" from the Jellyfin app =================
 * The box registers as a controllable session and keeps a WebSocket open to the server (background thread).
 * Commands (Play, Playstate, GeneralCommand) are put in a queue, which the main thread handles. */
typedef struct { char *type; cJSON *data; } RMsg;
static SDL_Mutex *rmu;
static RMsg rq_msgs[16];
static int rq_n;
static struct { char url[512], token[256], auth[700], dev[64]; int gen; } rconf; /* snapshot for the thread */
static SDL_AtomicInt rgen;
static bool rthread;

static void rpush(const char *type, cJSON *data) {
    SDL_LockMutex(rmu);
    if (rq_n < 16) rq_msgs[rq_n++] = (RMsg){str_dup(type), data ? cJSON_Duplicate(data, 1) : NULL};
    SDL_UnlockMutex(rmu);
}
static bool rpop(RMsg *m, bool (*want)(const char *)) {
    SDL_LockMutex(rmu);
    for (int i = 0; i < rq_n; i++)
        if (!want || want(rq_msgs[i].type)) {
            *m = rq_msgs[i];
            memmove(&rq_msgs[i], &rq_msgs[i + 1], sizeof(RMsg) * (rq_n - i - 1));
            rq_n--;
            SDL_UnlockMutex(rmu);
            return true;
        }
    SDL_UnlockMutex(rmu);
    return false;
}
static void rfree(RMsg *m) { free(m->type); cJSON_Delete(m->data); }

static void ws_handle(const char *txt) {
    cJSON *o = cJSON_Parse(txt);
    cJSON *t = cJSON_GetObjectItem(o, "MessageType");
    if (cJSON_IsString(t) && (!strcmp(t->valuestring, "Play") || !strcmp(t->valuestring, "Playstate") ||
                              !strcmp(t->valuestring, "GeneralCommand")))
        rpush(t->valuestring, cJSON_GetObjectItem(o, "Data"));
    cJSON_Delete(o);
}

static void ws_send(CURL *c, const char *txt) {
    size_t sent;
    curl_ws_send(c, txt, strlen(txt), &sent, 0, CURLWS_TEXT);
}

static int ws_thread(void *arg) {
    (void)arg;
    size_t cap = 256 * 1024;
    char *buf = malloc(cap + 1);
    for (;;) {
        SDL_LockMutex(rmu);
        int gen = rconf.gen;
        char url[512], token[256], auth[700], dev[64];
        snprintf(dev, sizeof dev, "%s", rconf.dev);
        snprintf(url, sizeof url, "%s", rconf.url);
        snprintf(token, sizeof token, "%s", rconf.token);
        snprintf(auth, sizeof auth, "%s", rconf.auth);
        SDL_UnlockMutex(rmu);
        if (!url[0] || !token[0]) { SDL_Delay(3000); continue; }
        /* 1) register the box as controllable: then it shows up under "Play on" in the Jellyfin app */
        char *cu = str_printf("%s/Sessions/Capabilities/Full", url);
        const char *hdr[] = {"Content-Type: application/json", auth, NULL};
        HttpBuf b = {0};
        http_request("POST", cu, hdr,
                     "{\"PlayableMediaTypes\":[\"Video\",\"Audio\"],\"SupportsMediaControl\":true,\"SupportsPersistentIdentifier\":true,"
                     "\"SupportedCommands\":[\"SetVolume\",\"VolumeUp\",\"VolumeDown\",\"Mute\",\"Unmute\",\"ToggleMute\",\"DisplayMessage\"]}",
                     &b, 10);
        http_free(&b);
        free(cu);
        /* 2) WebSocket: ws(s)://server/socket?api_key=…&deviceId=… */
        char *wsu = str_printf("ws%s/socket?api_key=%s&deviceId=%s", url + 4, token, dev);
        CURL *c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, wsu);
        curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        CURLcode rc = curl_easy_perform(c);
        free(wsu);
        if (rc != CURLE_OK) { curl_easy_cleanup(c); SDL_Delay(10000); continue; }
        curl_socket_t sock;
        curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock);
        double last_ka = 0;
        size_t len = 0;
        ws_send(c, "{\"MessageType\":\"KeepAlive\"}");
        while (gen == SDL_GetAtomicInt(&rgen)) {
            size_t rl = 0;
            const struct curl_ws_frame *m = NULL;
            rc = curl_ws_recv(c, buf + len, cap - len, &rl, &m);
            if (rc == CURLE_AGAIN) {
                struct pollfd p = {sock, POLLIN, 0};
                poll(&p, 1, 1000);
                if (app_time() - last_ka > 30) { last_ka = app_time(); ws_send(c, "{\"MessageType\":\"KeepAlive\"}"); }
                continue;
            }
            if (rc != CURLE_OK || !m || (m->flags & CURLWS_CLOSE)) break;
            len += rl;
            if (m->bytesleft == 0 && !(m->flags & CURLWS_CONT)) { buf[len] = 0; ws_handle(buf); len = 0; }
            if (len >= cap) len = 0; /* unreasonably big message */
        }
        curl_easy_cleanup(c);
        SDL_Delay(2000); /* the connection dropped (or a new login): reconnect */
    }
    return 0;
}

static double num(cJSON *o, const char *k) { cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsNumber(v) ? v->valuedouble : 0; }
static const char *str(cJSON *o, const char *k) { cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }

static bool is_control(const char *t) { return strcmp(t, "Play") != 0; }

/* during playback: pause/seek/stop/volume; a new "Play" stops the current one (started afterwards) */
static void remote_play_tick(void *ud) {
    (void)ud;
    RMsg m;
    while (rpop(&m, is_control)) {
        char a[32];
        if (!strcmp(m.type, "Playstate")) {
            const char *cmd = str(m.data, "Command");
            if (!strcmp(cmd, "Pause")) player_cmd((const char *[]){"set", "pause", "yes", NULL});
            else if (!strcmp(cmd, "Unpause")) player_cmd((const char *[]){"set", "pause", "no", NULL});
            else if (!strcmp(cmd, "PlayPause")) player_cmd((const char *[]){"cycle", "pause", NULL});
            else if (!strcmp(cmd, "Stop")) player_cmd((const char *[]){"quit", NULL});
            else if (!strcmp(cmd, "Seek")) {
                snprintf(a, sizeof a, "%.1f", num(m.data, "SeekPositionTicks") / TICKS);
                player_cmd((const char *[]){"seek", a, "absolute", NULL});
            }
            if (strcmp(cmd, "Stop")) player_cmd((const char *[]){"script-message", "notatv-show", NULL});
        } else if (!strcmp(m.type, "GeneralCommand")) {
            const char *name = str(m.data, "Name");
            cJSON *args = cJSON_GetObjectItem(m.data, "Arguments");
            if (!strcmp(name, "SetVolume")) {
                snprintf(a, sizeof a, "%s", str(args, "Volume"));
                player_cmd((const char *[]){"set", "volume", a, NULL});
            } else if (!strcmp(name, "VolumeUp")) player_cmd((const char *[]){"add", "volume", "5", NULL});
            else if (!strcmp(name, "VolumeDown")) player_cmd((const char *[]){"add", "volume", "-5", NULL});
            else if (!strcmp(name, "Mute") || !strcmp(name, "Unmute") || !strcmp(name, "ToggleMute"))
                player_cmd((const char *[]){!strcmp(name, "ToggleMute") ? "cycle" : "set", "mute",
                                            !strcmp(name, "ToggleMute") ? NULL : !strcmp(name, "Mute") ? "yes" : "no", NULL});
            else if (!strcmp(name, "DisplayMessage")) player_osd(str(args, "Text"), 4000);
        }
        rfree(&m);
    }
    SDL_LockMutex(rmu);
    bool new_play = false;
    for (int i = 0; i < rq_n; i++) if (!strcmp(rq_msgs[i].type, "Play")) new_play = true;
    SDL_UnlockMutex(rmu);
    if (new_play) player_cmd((const char *[]){"quit", NULL});
}

typedef struct { double start; } RemoteStart;
static void remote_item(void *ud, cJSON *json, long status) {
    RemoteStart *rs = ud;
    if (status == 200 && json) {
        JItem it;
        jitem_parse(&it, json);
        it.pos = rs->start;
        jf_play(&it, rs->start > 0, NULL, NULL);
        jitem_free(&it);
    } else app_toast(T("Jellyfin: could not load the title (%ld)"), status);
    free(rs);
}

/* called every frame from the main loop */
static double next_conf;
void jellyfin_remote_tick(void) {
    if (!rmu) rmu = SDL_CreateMutex();
    if (app_time() >= next_conf) { /* login changed / turned on/off: give the thread a new snapshot */
        next_conf = app_time() + 3;
        bool on = cfg_int("jf_cast", 1) && app_on("jellyfin") && *jf_url() && *jf_token();
        char auth[700] = "", dn[128];
        if (on) snprintf(auth, sizeof auth, "Authorization: MediaBrowser Client=\"NotaTV\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"",
                         jf_devname(dn, sizeof dn), device_id(), TV_VERSION, jf_token());
        SDL_LockMutex(rmu);
        bool changed = strcmp(rconf.url, on ? jf_url() : "") || strcmp(rconf.auth, auth);
        if (changed) {
            snprintf(rconf.url, sizeof rconf.url, "%s", on ? jf_url() : "");
            snprintf(rconf.token, sizeof rconf.token, "%s", on ? jf_token() : "");
            snprintf(rconf.auth, sizeof rconf.auth, "%s", auth);
            snprintf(rconf.dev, sizeof rconf.dev, "%s", on ? device_id() : "");
            rconf.gen++;
            SDL_SetAtomicInt(&rgen, rconf.gen);
        }
        SDL_UnlockMutex(rmu);
        if (on && !rthread) {
            SDL_Thread *t = SDL_CreateThread(ws_thread, "jellyfin-ws", NULL);
            if (t) { SDL_DetachThread(t); rthread = true; }
        }
    }
    RMsg m;
    while (rpop(&m, NULL)) {
        if (!strcmp(m.type, "Play")) {
            cJSON *ids = cJSON_GetObjectItem(m.data, "ItemIds"), *id0 = cJSON_GetArrayItem(ids, 0);
            if (cJSON_IsString(id0)) {
                RemoteStart *rs = calloc(1, sizeof *rs);
                rs->start = num(m.data, "StartPositionTicks") / TICKS;
                char *p = str_printf("/Items/%s?userId=%s&" FIELDS, id0->valuestring, jf_uid());
                jf_call_ex("GET", p, NULL, remote_item, rs, true);
                free(p);
            }
        } else if (!strcmp(m.type, "GeneralCommand") && !strcmp(str(m.data, "Name"), "DisplayMessage")) {
            app_toast("%s", str(cJSON_GetObjectItem(m.data, "Arguments"), "Text"));
        } /* Playstate without playback: nothing to control */
        rfree(&m);
    }
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
static const char *const plexst_o[] = {"hero", "focus", NULL}, *const plexst_l[] = {"Hero", N_("Focus"), NULL};
static const char *const jfsub_o[] = {"jellyfin", "player", "off", NULL}, *const jfsub_l[] = {N_("Follow Jellyfin"), N_("Language setting"), N_("Off"), NULL};
static void act_jf_logout(void) { app_ask_signout("Jellyfin", jellyfin_forget); }
static const char *jf_signed_out(void) { return *jf_token() ? NULL : ITEM_HIDDEN; }
static const Item jf_items[] = {
    {N_("Jellyfin subtitles"), IT_CYCLE, "jf_subs", "jellyfin", jfsub_o, jfsub_l,
     .help = N_("Follow Jellyfin: the audio and subtitles your Jellyfin user settings pick (language, smart, forced only). Language setting: the player picks by Subtitle language under Picture and sound. Off: never")},
    {N_("Jellyfin: play from phone"), IT_TOGGLE, "jf_cast", "1", set_onoff, .help = N_("NotaTV appears under \"Play On\" in the Jellyfin app when you are signed in to Jellyfin here")},
    {N_("Home screen (Plex and Jellyfin)"), IT_CYCLE, "plex_home_style", "hero", plexst_o, plexst_l, .help = N_("Hero: a featured title at the top. Focus: background and info follow the selected title")},
    {N_("Sign out of Jellyfin"), IT_ACTION, .action = act_jf_logout, .help = N_("Invalidates the login at the server and deletes it from this box"),
     .locked = jf_signed_out},
};

/* installed/uninstalled: give the "Play on" thread a new snapshot now (it stops when the app is gone) */
static void jellyfin_app_apply(void) { next_conf = 0; jellyfin_remote_tick(); }
const NotaApp nota_app = {
    NOTA_API, "jellyfin", "Jellyfin", N_("Movies and shows from your Jellyfin server"), APP_VERSION, 30, APP_DEFAULT_OFF,
    ICON_FILM, {0, 164, 220, 255}, {120, 70, 190, 255}, {255, 255, 255, 255},
    .row_title = N_("Continue watching"), .shelf_label = N_("CONTINUE IN JELLYFIN"),
    .row = jellyfin_row, .row_refresh = jellyfin_row_refresh, .row_open = jellyfin_row_open,
    .open = jellyfin_screen, .tick = jellyfin_remote_tick, .apply = jellyfin_app_apply, .forget = jellyfin_forget,
    .settings = jf_items, .n_settings = sizeof jf_items / sizeof jf_items[0],
};
