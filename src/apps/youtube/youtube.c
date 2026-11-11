/* NotaTV app package "youtube" - YouTube: side menu, search and personal feeds via yt-dlp, playback via mpv */
#include "youtube.h"
#include "vendor/cJSON.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

typedef struct { char *id, *title, *channel, *channel_id, *thumb; double duration, views; bool is_live, is_channel; long published; } Video;
typedef struct { char *title; char *query; Video *v; int n; bool loading, failed; int seq; } YRow;

enum { PAGE_HOME, PAGE_GRID };
/* side menu items (the index in the menu is computed when building it) */
enum { M_HOME, M_SEARCH, M_SUBS, M_CHANNELS, M_REC, M_LATER, M_HIST, M_ACCOUNT,
       M_CH_VIDEOS, M_CH_LIVE, M_CH_SHORTS, M_CH_SUB, M_SETTINGS, M_COUNT };

typedef struct {
    YRow rows[8];
    int n_rows;
    int page;
    int row, col;
    float sy, sx[8];
    char query[256];
    char **hist;
    int n_hist;
    SideMenu side;
    int side_map[16]; /* menupunkt -> M_* */
    int cfg_seen;
    char prof[128]; /* active account when the page was shown */
    char chan_id[64], chan_name[128]; /* channel mode (empty = regular YouTube) */
    YRow chans;   /* channel suggestions at the top of search results */
    bool has_chans;
    int ccol;     /* focus in the channel row; row == -1 means the main row */
    float csx;
    char wait_id[16];                 /* play when the prefetch for this video is done */
    double wait_t;
} Yt;

static Screen *channel_screen(const char *id, const char *name);
static Screen *info_screen(Video *v);
static void info_set_resume(Screen *s, double t);
static bool yt_play_at(Video *v, double start);
#define yt_play(v) yt_play_at((v), 0)

static int yt_token; /* increased when the screen closes, so late replies are ignored */
static int yt_gen;   /* increased on sign-out, so replies from before are ignored */

/* YouTube accounts: cookies.txt per profile in <data>/youtube/<name>.cookies (uploaded via the phone page) */
static bool cookie_file(char *buf, size_t n) {
    const char *p = cfg_get("yt_profile", "");
    if (!*p) return false;
    snprintf(buf, n, "%s/youtube/%s.cookies", data_dir(), p);
    return access(buf, R_OK) == 0;
}
static bool logged_in(void) {
    char ck[512];
    return cookie_file(ck, sizeof ck);
}

typedef struct { char *query; Video *v; int n; int token, seq; YRow *row; char cookies[512]; int count; bool channels; int gen; } SearchJob;

static void search_work(void *arg) {
    SearchJob *j = arg;
    /* ":ytrec", ":ytwatchlater" and URLs (feeds) are used directly, everything else is a search */
    bool direct = j->query[0] == ':' || !strncmp(j->query, "http", 4);
    char *spec = direct ? str_dup(j->query) : str_printf("ytsearch%d:%s", j->count, j->query);
    char *q = shell_quote(spec);
    char *ck = j->cookies[0] ? shell_quote(j->cookies) : NULL;
    char *cmd = str_printf("timeout 50 yt-dlp --flat-playlist -J --no-warnings --playlist-end %d %s%s %s 2>/tmp/yt-dlp-search.log",
                           j->count, ck ? "--cookies " : "", ck ? ck : "", q);
    char *out = run_capture(cmd);
    free(spec); free(q); free(ck); free(cmd);
    cJSON *root = cJSON_Parse(out);
    free(out);
    cJSON *entries = cJSON_GetObjectItem(root, "entries");
    int n = cJSON_GetArraySize(entries);
    j->v = xcalloc(n + 1, sizeof(Video));
    cJSON *e;
    cJSON_ArrayForEach(e, entries) {
        cJSON *id = cJSON_GetObjectItem(e, "id"), *t = cJSON_GetObjectItem(e, "title");
        if (!cJSON_IsString(id) || !cJSON_IsString(t)) continue;
        cJSON *ie = cJSON_GetObjectItem(e, "ie_key"), *eu = cJSON_GetObjectItem(e, "url");
        if (j->channels) { /* channel search: channels only */
            if (!cJSON_IsString(ie) || strcmp(ie->valuestring, "YoutubeTab")) continue;
            cJSON *cid = cJSON_GetObjectItem(e, "channel_id");
            const char *chid = cJSON_IsString(cid) ? cid->valuestring : id->valuestring;
            if (strncmp(chid, "UC", 2)) continue;
            Video *v = &j->v[j->n++];
            v->is_channel = true;
            v->id = str_dup(chid);
            v->channel_id = str_dup(chid);
            v->title = str_dup(t->valuestring);
            v->channel = str_dup(t->valuestring);
            cJSON *th = cJSON_GetObjectItem(e, "thumbnails"), *last = th ? cJSON_GetArrayItem(th, cJSON_GetArraySize(th) - 1) : NULL;
            cJSON *tu = cJSON_GetObjectItem(last, "url");
            if (cJSON_IsString(tu)) v->thumb = !strncmp(tu->valuestring, "//", 2) ? str_printf("https:%s", tu->valuestring) : str_dup(tu->valuestring);
            cJSON *fc = cJSON_GetObjectItem(e, "channel_follower_count");
            v->views = cJSON_IsNumber(fc) ? fc->valuedouble : 0;
            continue;
        }
        if (cJSON_IsString(ie) && strcmp(ie->valuestring, "Youtube")) continue; /* skip channels and playlists */
        if (cJSON_IsString(eu) && strstr(eu->valuestring, "/shorts/")) continue;   /* no Shorts */
        Video *v = &j->v[j->n++];
        v->id = str_dup(id->valuestring);
        v->title = str_dup(t->valuestring);
        cJSON *ch = cJSON_GetObjectItem(e, "channel");
        if (!cJSON_IsString(ch)) ch = cJSON_GetObjectItem(e, "uploader");
        v->channel = cJSON_IsString(ch) ? str_dup(ch->valuestring) : NULL;
        cJSON *cid = cJSON_GetObjectItem(e, "channel_id");
        if (!cJSON_IsString(cid)) cid = cJSON_GetObjectItem(root, "channel_id"); /* channel tab: shared by all */
        v->channel_id = cJSON_IsString(cid) ? str_dup(cid->valuestring) : NULL;
        if (!v->channel) { cJSON *rc = cJSON_GetObjectItem(root, "channel"); if (cJSON_IsString(rc)) v->channel = str_dup(rc->valuestring); }
        cJSON *d = cJSON_GetObjectItem(e, "duration"), *vc = cJSON_GetObjectItem(e, "view_count");
        v->duration = cJSON_IsNumber(d) ? d->valuedouble : 0;
        v->views = cJSON_IsNumber(vc) ? vc->valuedouble : 0;
        cJSON *ls = cJSON_GetObjectItem(e, "live_status");
        v->is_live = cJSON_IsString(ls) && !strcmp(ls->valuestring, "is_live");
    }
    cJSON_Delete(root);
}

/* ---------- prefetch: direct stream URLs for the focused video and its neighbors ---------- */
typedef struct { char id[16]; char *video, *audio; double t; int q; } Prefetch; /* video == NULL: the lookup failed; q: the quality it was looked up for */
static Prefetch pf[24];
static int pf_next;
static char pf_busy_id[16]; /* video being looked up right now ("" = none) */
static char pf_focus[16];
static double pf_focus_t;

/* YouTube has 1440p and 4K only as VP9/AV1. The Pi 4 cannot decode those in hardware (H.264 goes up to 1080p),
   and the Pi 5 has no hardware decoder for them at all (only HEVC): it decodes in software and mpv draws such
   frames in 1080p anyway. On both, 1080p is the most, and 1440p/2160p are shown greyed out. Other devices get 2160p. */
static int yt_max(void) { return !board_pi || board_pi() ? 1080 : 2160; } /* older NotaTV: Pi only */
static int yt_quality(void) {
    int q = cfg_int("yt_quality", 1080);
    return q <= 0 ? 1080 : q > yt_max() ? yt_max() : q;
}

static char *yt_format_for(int h) {
    if (h > 1080) /* only VP9/AV1 above 1080p: VP9 before AV1 */
        return str_printf("bv*[height<=%d][vcodec!^=av01]+ba/bv*[height<=%d]+ba/b[height<=%d]/b", h, h, h);
    /* H.264 first (hardware); https over HLS; otherwise software fallback */
    return str_printf("bv*[height<=%d][vcodec^=avc1][protocol^=https]+ba[protocol^=https]/bv*[height<=%d][vcodec^=avc1]+ba/"
                      "bv*[height<=%d][vcodec!^=av01]+ba/b[height<=%d]/b", h, h, h, h);
}

static char *yt_format(void) { return yt_format_for(yt_quality()); }

static Prefetch *pf_entry(const char *id) { /* also failed (tried again after a minute); only for the current quality */
    for (int i = 0; i < 24; i++)
        if (pf[i].id[0] && !strcmp(pf[i].id, id) && pf[i].q == yt_quality() && app_time() - pf[i].t < (pf[i].video ? 3 * 3600 : 60)) return &pf[i];
    return NULL;
}
static Prefetch *pf_find(const char *id) {
    Prefetch *p = pf_entry(id);
    return p && p->video ? p : NULL;
}

typedef struct { char id[16]; char *video, *audio; char cookies[512]; int q, gen; } PfJob;
static void pf_work(void *arg) {
    PfJob *j = arg;
    char *fmt = yt_format_for(j->q);
    char *url = str_printf("https://www.youtube.com/watch?v=%s", j->id);
    const char *argv[12];
    int n = 0;
    argv[n++] = "timeout"; argv[n++] = "25"; argv[n++] = "yt-dlp"; argv[n++] = "-g"; argv[n++] = "--no-warnings";
    argv[n++] = "-f"; argv[n++] = fmt;
    if (j->cookies[0]) { argv[n++] = "--cookies"; argv[n++] = j->cookies; }
    argv[n++] = "--"; argv[n++] = url; argv[n] = NULL;
    char *out = run_capture_argv(argv);
    free(fmt); free(url);
    char *sv_l1 = NULL;
    char *l1 = strtok_r(out, "\n", &sv_l1), *l2 = strtok_r(NULL, "\n", &sv_l1);
    if (l1 && !strncmp(l1, "http", 4)) j->video = str_dup(l1);
    if (l2 && !strncmp(l2, "http", 4)) j->audio = str_dup(l2);
    free(out);
}
static void pf_done(void *arg) {
    PfJob *j = arg;
    pf_busy_id[0] = 0;
    if (j->gen != yt_gen) { free(j->video); free(j->audio); free(j); return; } /* signed out: the URLs are not kept */
    Prefetch *p = NULL; /* the slot for this video (any quality: the old one is replaced) */
    for (int i = 0; i < 24 && !p; i++) if (pf[i].id[0] && !strcmp(pf[i].id, j->id)) p = &pf[i];
    if (!p) { p = &pf[pf_next]; pf_next = (pf_next + 1) % 24; }
    free(p->video); free(p->audio);
    snprintf(p->id, sizeof p->id, "%s", j->id);
    p->q = j->q;
    p->video = j->video;
    p->audio = j->audio;
    p->t = app_time();
    free(j);
}
/* ids[0] = the focused video, the rest neighbors in priority order (NULL = skip).
 * Only once focus has been still for 0.6 s; one lookup at a time (low priority). */
static void pf_consider(const char *const *ids, int n) {
    if (!ids[0]) { pf_focus[0] = 0; return; }
    if (strcmp(pf_focus, ids[0])) { snprintf(pf_focus, sizeof pf_focus, "%s", ids[0]); pf_focus_t = app_time(); return; }
    if (pf_busy_id[0] || app_time() - pf_focus_t < 0.6) return;
    for (int i = 0; i < n; i++) {
        if (!ids[i] || pf_entry(ids[i])) continue;
        PfJob *j = xcalloc(1, sizeof *j);
        snprintf(j->id, sizeof j->id, "%s", ids[i]);
        j->q = yt_quality();
        j->gen = yt_gen;
        if (!cookie_file(j->cookies, sizeof j->cookies)) j->cookies[0] = 0;
        snprintf(pf_busy_id, sizeof pf_busy_id, "%s", ids[i]);
        job_submit_q(JQ_SLOW, pf_work, pf_done, j);
        return;
    }
}

static void free_videos(Video *v, int n) {
    for (int i = 0; i < n; i++) { free(v[i].id); free(v[i].title); free(v[i].channel); free(v[i].channel_id); free(v[i].thumb); }
    free(v);
}

/* ---------- local subscriptions (RSS, no Google account) ---------- */
typedef struct { char id[64], name[128]; } Sub;
static Sub *subs;
static int n_subs;

static void subs_path(char *p, size_t n) { snprintf(p, n, "%s/yt-subs.txt", data_dir()); }

static void subs_save(void) {
    char p[512];
    subs_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < n_subs; i++) fprintf(f, "%s\t%s\n", subs[i].id, subs[i].name);
    fclose(f);
}

static bool subs_has(const char *id) {
    for (int i = 0; id && i < n_subs; i++) if (!strcmp(subs[i].id, id)) return true;
    return false;
}

static void subs_add(const char *id, const char *name) {
    if (!id || !*id || subs_has(id)) return;
    subs = xrealloc(subs, sizeof(Sub) * (n_subs + 1));
    snprintf(subs[n_subs].id, sizeof subs[0].id, "%s", id);
    snprintf(subs[n_subs].name, sizeof subs[0].name, "%s", name && *name ? name : id);
    n_subs++;
}

static void subs_remove(const char *id) {
    for (int i = 0; i < n_subs; i++)
        if (!strcmp(subs[i].id, id)) { memmove(subs + i, subs + i + 1, sizeof(Sub) * (n_subs - i - 1)); n_subs--; return; }
}

static int sub_cmp(const void *a, const void *b) { return strcasecmp(((const Sub *)a)->name, ((const Sub *)b)->name); }

static void subs_load(void) {
    n_subs = 0;
    char p[512], line[512];
    subs_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        str_trim(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        subs_add(line, tab + 1);
    }
    if (f) fclose(f);
    /* import from Google Takeout (subscriptions.csv uploaded via the phone page) */
    snprintf(p, sizeof p, "%s/youtube/subscriptions.csv", data_dir());
    f = fopen(p, "r");
    if (f) {
        int added = 0, before = n_subs;
        while (fgets(line, sizeof line, f)) {
            str_trim(line);
            char *c1 = strchr(line, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
            if (!c1 || !c2 || strncmp(line, "UC", 2)) continue; /* skip the heading */
            *c1 = 0;
            subs_add(line, c2 + 1);
        }
        fclose(f);
        unlink(p);
        added = n_subs - before;
        if (added) app_toast(T("%d channels imported"), added);
    }
    qsort(subs, n_subs, sizeof(Sub), sub_cmp);
    subs_save();
}

/* one feed per channel is fetched in parallel in the image queue and merged by publish time */
typedef struct { YRow *row; int seq, token, pending; Video *v; int n, cap; int gen; } RssAgg;
typedef struct { RssAgg *agg; char id[64], name[128]; Video *v; int n; } RssJob;

static char *xml_between(const char *s, const char *end, const char *a, const char *b) {
    const char *p = strstr(s, a);
    if (!p || p > end) return NULL;
    p += strlen(a);
    const char *q = strstr(p, b);
    if (!q || q > end) return NULL;
    char *r = xstrndup(p, q - p);
    /* simple XML entities */
    char *w = r;
    for (char *t = r; *t;) {
        if (!strncmp(t, "&amp;", 5)) { *w++ = '&'; t += 5; }
        else if (!strncmp(t, "&quot;", 6)) { *w++ = '"'; t += 6; }
        else if (!strncmp(t, "&#39;", 5) || !strncmp(t, "&apos;", 6)) { *w++ = '\''; t += t[1] == '#' ? 5 : 6; }
        else if (!strncmp(t, "&lt;", 4)) { *w++ = '<'; t += 4; }
        else if (!strncmp(t, "&gt;", 4)) { *w++ = '>'; t += 4; }
        else *w++ = *t++;
    }
    *w = 0;
    return r;
}

static void rss_work(void *arg) {
    RssJob *j = arg;
    char url[160];
    snprintf(url, sizeof url, "https://www.youtube.com/feeds/videos.xml?channel_id=%s", j->id);
    HttpBuf b;
    if (!http_request("GET", url, NULL, NULL, &b, 10) || b.status != 200 || !b.data) { http_free(&b); return; }
    j->v = xcalloc(16, sizeof(Video));
    for (char *e = strstr(b.data, "<entry>"); e && j->n < 15; e = strstr(e + 7, "<entry>")) {
        char *ee = strstr(e, "</entry>");
        if (!ee) break;
        char *id = xml_between(e, ee, "<yt:videoId>", "</yt:videoId>");
        char *title = xml_between(e, ee, "<title>", "</title>");
        char *pub = xml_between(e, ee, "<published>", "</published>");
        char *views = xml_between(e, ee, "views=\"", "\"");
        char *link = xml_between(e, ee, "<link rel=\"alternate\" href=\"", "\"");
        bool is_short = link && strstr(link, "/shorts/");
        free(link);
        if (id && title && !is_short) {
            Video *v = &j->v[j->n++];
            v->id = id; v->title = title; id = title = NULL;
            v->channel = str_dup(j->name);
            v->channel_id = str_dup(j->id);
            v->views = views ? atof(views) : 0;
            struct tm tm = {0};
            if (pub && sscanf(pub, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
                tm.tm_year -= 1900; tm.tm_mon -= 1;
                v->published = (long)timegm(&tm);
            }
        }
        free(id); free(title); free(pub); free(views);
    }
    http_free(&b);
}

static int by_published(const void *a, const void *b) {
    long x = ((const Video *)a)->published, y = ((const Video *)b)->published;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void rss_done(void *arg) {
    RssJob *j = arg;
    RssAgg *g = j->agg;
    if (g->n + j->n > g->cap) { g->cap = (g->n + j->n) * 2; g->v = xrealloc(g->v, sizeof(Video) * g->cap); }
    memcpy(g->v + g->n, j->v, sizeof(Video) * j->n);
    g->n += j->n;
    free(j->v);
    free(j);
    if (--g->pending > 0) return;
    if (g->gen != yt_gen) { free_videos(g->v, g->n); free(g); return; } /* signed out: the list is not kept */
    qsort(g->v, g->n, sizeof(Video), by_published);
    int keep = g->n > 60 ? 60 : g->n;
    if (g->token == yt_token && g->seq == g->row->seq) {
        free_videos(g->row->v, g->row->n);
        g->row->v = g->v;
        g->row->n = keep;
        g->row->loading = false;
        g->row->failed = keep == 0;
        for (int i = keep; i < g->n; i++) { free(g->v[i].id); free(g->v[i].title); free(g->v[i].channel); free(g->v[i].channel_id); }
    } else free_videos(g->v, g->n);
    free(g);
}

static void rss_load(YRow *row) {
    row->loading = true;
    row->failed = false;
    if (n_subs == 0) { row->loading = false; row->failed = true; return; }
    RssAgg *g = xcalloc(1, sizeof *g);
    g->row = row;
    g->seq = ++row->seq;
    g->token = yt_token;
    g->gen = yt_gen;
    g->pending = n_subs;
    for (int i = 0; i < n_subs; i++) {
        RssJob *j = xcalloc(1, sizeof *j);
        j->agg = g;
        snprintf(j->id, sizeof j->id, "%s", subs[i].id);
        snprintf(j->name, sizeof j->name, "%s", subs[i].name);
        job_submit_q(JQ_IMAGE, rss_work, rss_done, j);
    }
}

static void search_done(void *arg) {
    SearchJob *j = arg;
    if (j->gen != yt_gen) { /* signed out: the list is not kept */
        free_videos(j->v, j->n);
        free(j->query);
        free(j);
        return;
    }
    if (j->token == yt_token && j->seq == j->row->seq) {
        free_videos(j->row->v, j->row->n);
        j->row->v = j->v;
        j->row->n = j->n;
        j->row->loading = false;
        j->row->failed = j->n == 0;
    } else free_videos(j->v, j->n);
    free(j->query);
    free(j);
}

static void row_load(YRow *row, int count) {
    if (!strcmp(row->query, "@subs")) { rss_load(row); return; }
    row->loading = true;
    row->failed = false;
    SearchJob *j = xcalloc(1, sizeof *j);
    j->query = str_dup(row->query);
    j->token = yt_token;
    j->seq = ++row->seq;
    j->row = row;
    j->count = count;
    j->gen = yt_gen;
    j->channels = strstr(row->query, "&sp=EgIQAg") != NULL;
    if (!cookie_file(j->cookies, sizeof j->cookies)) j->cookies[0] = 0;
    job_submit_q(JQ_SLOW, search_work, search_done, j);
}

static void hist_load(Yt *y) {
    for (int i = 0; i < y->n_hist; i++) free(y->hist[i]);
    free(y->hist);
    y->hist = NULL;
    y->n_hist = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/yt-history.txt", data_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && y->n_hist < 8) {
        str_trim(line);
        if (!line[0]) continue;
        y->hist = xrealloc(y->hist, sizeof(char *) * (y->n_hist + 1));
        y->hist[y->n_hist++] = xstrdup(line);
    }
    fclose(f);
}

static void hist_add(Yt *y, const char *q) {
    char path[512];
    snprintf(path, sizeof path, "%s/yt-history.txt", data_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", q);
    for (int i = 0; i < y->n_hist; i++)
        if (strcmp(y->hist[i], q)) fprintf(f, "%s\n", y->hist[i]);
    fclose(f);
    hist_load(y);
}

static void clear_rows(Yt *y) {
    for (int i = 0; i < 8; i++) {
        free_videos(y->rows[i].v, y->rows[i].n);
        free(y->rows[i].title);
        free(y->rows[i].query);
        int seq = y->rows[i].seq + 1; /* invalidate pending searches */
        memset(&y->rows[i], 0, sizeof y->rows[i]);
        y->rows[i].seq = seq;
    }
    y->n_rows = 0;
}

static void add_row(Yt *y, const char *title, const char *query, int count) {
    if (y->n_rows >= 8) return;
    YRow *r = &y->rows[y->n_rows];
    r->title = str_dup(title);
    r->query = str_dup(query);
    y->sx[y->n_rows] = 0;
    y->n_rows++;
    row_load(r, count);
}

static void show_channel(Yt *y) {
    clear_rows(y);
    y->page = PAGE_HOME;
    char u[256];
    snprintf(u, sizeof u, "https://www.youtube.com/channel/%s/videos", y->chan_id);
    add_row(y, T("Videos"), u, 30);
    snprintf(u, sizeof u, "https://www.youtube.com/channel/%s/streams", y->chan_id);
    add_row(y, T("Live and past streams"), u, 20);
    y->row = y->col = 0;
}

static void clear_chans(Yt *y);

static void show_home(Yt *y) {
    if (y->chan_id[0]) { show_channel(y); return; }
    clear_rows(y);
    clear_chans(y);
    y->page = PAGE_HOME;
    y->query[0] = 0;
    if (n_subs) add_row(y, T("Latest from your subscriptions"), "@subs", 0);
    if (logged_in()) add_row(y, T("Recommended for you"), ":ytrec", 20);
    if (y->n_hist > 0) add_row(y, y->hist[0], y->hist[0], 20);
    /* the default searches on the home page follow the language (can be changed with yt_cat1-3) */
    bool da = !strcmp(i18n_lang(), "da");
    add_row(y, T("Music"), cfg_get("yt_cat1", da ? "musik hits 2026" : "top music hits"), 20);
    add_row(y, T("News"), cfg_get("yt_cat2", da ? "dr nyheder" : "news today"), 20);
    add_row(y, T("Documentaries"), cfg_get("yt_cat3", da ? "dokumentar dansk" : "documentary"), 20);
    y->row = y->col = 0;
}

static void clear_chans(Yt *y) {
    free_videos(y->chans.v, y->chans.n);
    free(y->chans.title);
    free(y->chans.query);
    int seq = y->chans.seq + 1;
    memset(&y->chans, 0, sizeof y->chans);
    y->chans.seq = seq;
    y->has_chans = false;
    y->ccol = 0;
}

static void show_grid(Yt *y, const char *title, const char *query) {
    clear_rows(y);
    clear_chans(y);
    y->page = PAGE_GRID;
    snprintf(y->query, sizeof y->query, "%s", title);
    add_row(y, title, query, 40);
    y->row = y->col = 0;
}

/* ---------- sidemenu ---------- */
static void build_side(Yt *y) {
    SideMenu *m = &y->side;
    int keep_active = m->n ? y->side_map[m->active] : M_HOME;
    side_clear(m);
    const char *prof = cfg_get("yt_profile", "");
    bool li = logged_in();
    snprintf(m->subtitle, sizeof m->subtitle, "%s", li ? prof : T("Not signed in"));
    int k = 0;
#define ADD(lbl, ic, gap, id) do { y->side_map[side_add(m, lbl, ic, gap)] = id; k++; } while (0)
    if (y->chan_id[0]) { /* kanalside */
        snprintf(m->subtitle, sizeof m->subtitle, "%s", y->chan_name);
        ADD(T("Videos"), ICON_PLAY, false, M_CH_VIDEOS);
        ADD("Live", ICON_TV, false, M_CH_LIVE);
        ADD(T("Search YouTube"), ICON_SEARCH, true, M_SEARCH);
        ADD(subs_has(y->chan_id) ? T("Unsubscribe") : T("Subscribe"), ICON_STAR, true, M_CH_SUB);
        m->active = 0;
        return;
    }
    ADD(T("Home"), ICON_TV, false, M_HOME);
    ADD(T("Search"), ICON_SEARCH, false, M_SEARCH);
    ADD(T("Subscriptions"), ICON_PLAY, true, M_SUBS);
    ADD(T("Channels"), ICON_STAR, false, M_CHANNELS);
    if (li) {
        ADD(T("Recommended"), ICON_STAR, true, M_REC);
        ADD(T("Watch later"), ICON_FILM, false, M_LATER);
        ADD(T("History"), ICON_BACK, false, M_HIST);
    }
    char lbl[160];
    snprintf(lbl, sizeof lbl, li ? T("Account: %s") : T("Sign in"), prof);
    ADD(lbl, ICON_LOCK, true, M_ACCOUNT);
    if (app_settings_open) ADD(T("Settings"), ICON_GEAR, false, M_SETTINGS); /* the app's settings, here where they are used */
#undef ADD
    (void)k;
    m->active = 0;
    for (int i = 0; i < m->n; i++) if (y->side_map[i] == keep_active) m->active = i;
}

static Screen *login_screen(void);

static void query_done(void *ud, const char *text) {
    Yt *y = ud;
    if (!text || !*text) return;
    hist_add(y, text);
    char title[300];
    snprintf(title, sizeof title, T("Results for \"%s\""), text);
    show_grid(y, title, text);
    /* channel suggestions at the top */
    char *e = url_encode(text);
    y->chans.title = str_dup(T("Channels"));
    y->chans.query = str_printf("https://www.youtube.com/results?search_query=%s&sp=EgIQAg%%3D%%3D", e);
    free(e);
    y->has_chans = true;
    row_load(&y->chans, 12);
    for (int i = 0; i < y->side.n; i++) if (y->side_map[i] == M_SEARCH) y->side.active = i;
}

static void chan_pick(void *ud, int idx) {
    (void)ud;
    if (idx >= 0 && idx < n_subs) app_push(channel_screen(subs[idx].id, subs[idx].name));
}

static void side_activate(Yt *y, int idx) {
    int id = y->side_map[idx];
    char u[256];
    switch (id) {
    case M_CH_VIDEOS: case M_CH_LIVE: {
        const char *tab = id == M_CH_VIDEOS ? "videos" : "streams";
        snprintf(u, sizeof u, "https://www.youtube.com/channel/%s/%s", y->chan_id, tab);
        y->side.active = idx;
        show_grid(y, y->side.items[idx].label, u);
        return;
    }
    case M_CH_SUB:
        if (subs_has(y->chan_id)) { subs_remove(y->chan_id); app_toast(T("Unsubscribed from %s"), y->chan_name); }
        else { subs_add(y->chan_id, y->chan_name); app_toast(T("You are now subscribed to %s"), y->chan_name); }
        qsort(subs, n_subs, sizeof(Sub), sub_cmp);
        subs_save();
        build_side(y);
        return;
    case M_CHANNELS: {
        if (n_subs == 0) { app_toast(T("No subscriptions yet - open a channel and choose Subscribe")); return; }
        char **names = xcalloc(n_subs, sizeof(char *));
        for (int i = 0; i < n_subs; i++) names[i] = subs[i].name;
        choice_open(T("Your channels"), (const char *const *)names, n_subs, 0, chan_pick, NULL);
        free(names);
        return;
    }
    default: break;
    }
    switch (id) {
    case M_HOME: y->side.active = idx; show_home(y); break;
    case M_SEARCH: keyboard_open(T("Search YouTube"), "", false, query_done, y); break;
    case M_SUBS:
        y->side.active = idx;
        if (n_subs == 0) app_toast(T("No subscriptions yet - press OK on a video, choose Info and comments, then Subscribe"));
        show_grid(y, T("Subscriptions"), "@subs");
        break;
    case M_REC: y->side.active = idx; show_grid(y, T("Recommended for you"), ":ytrec"); break;
    case M_LATER: y->side.active = idx; show_grid(y, T("Watch later"), ":ytwatchlater"); break;
    case M_HIST: y->side.active = idx; show_grid(y, T("History"), ":ythistory"); break;
    case M_ACCOUNT: app_push(login_screen()); break;
    case M_SETTINGS: app_settings_open("youtube"); break;
    }
}

static void yt_enter(Screen *s) {
    Yt *y = s->st;
    yt_token++;
    hist_load(y);
    if (!y->chan_id[0]) subs_load();
    side_init(&y->side, y->chan_id[0] ? y->chan_name : "YouTube", ICON_YOUTUBE, RGB(255, 255, 255));
    build_side(y);
    y->cfg_seen = app_cfg_gen();
    snprintf(y->prof, sizeof y->prof, "%s", cfg_get("yt_profile", ""));
    show_home(y);
}

static void yt_resume(Screen *s) {
    Yt *y = s->st;
    /* the account may have been switched on the login page */
    const char *p = cfg_get("yt_profile", "");
    if (strcmp(y->prof, p)) {
        snprintf(y->prof, sizeof y->prof, "%s", p);
        build_side(y);
        show_home(y);
    }
}

static Yt *sheet_yt; /* the screen whose video sheet is open */
static void yt_destroy(Screen *s) {
    if (sheet_yt == s->st) sheet_yt = NULL;
    Yt *y = s->st;
    yt_token++;
    clear_rows(y);
    clear_chans(y);
    for (int i = 0; i < y->n_hist; i++) free(y->hist[i]);
    free(y->hist);
    free(y);
    free(s);
}

/* gitter: 4 kolonner */
#define GCOLS 4
static int grid_rows(YRow *r) { return (r->n + GCOLS - 1) / GCOLS; }

static bool header_row(Yt *y) { /* is there a row above the content? */
    if (y->chan_id[0] && y->page == PAGE_HOME) return true;
    return y->page == PAGE_GRID && y->has_chans && y->chans.n > 0;
}

static Video *focused_video(Yt *y) {
    if (y->side.open || !y->n_rows || y->row < 0) return NULL;
    if (y->page == PAGE_GRID) {
        int i = y->row * GCOLS + y->col;
        return i < y->rows[0].n ? &y->rows[0].v[i] : NULL;
    }
    return y->row < y->n_rows && y->col < y->rows[y->row].n ? &y->rows[y->row].v[y->col] : NULL;
}

static Video *video_at(Yt *y, int row, int col) {
    if (y->page == PAGE_GRID) {
        int i = row * GCOLS + col;
        return col >= 0 && col < GCOLS && i >= 0 && i < y->rows[0].n ? &y->rows[0].v[i] : NULL;
    }
    return row >= 0 && row < y->n_rows && col >= 0 && col < y->rows[row].n ? &y->rows[row].v[col] : NULL;
}
static const char *pf_id(Video *v) { return v && !v->is_live && !v->is_channel ? v->id : NULL; }

/* play, but wait for an ongoing prefetch of the same video instead of starting over */
static void play_or_wait(Yt *y, Video *v) {
    if (!v->is_live && !pf_find(v->id) && !strcmp(pf_busy_id, v->id)) {
        snprintf(y->wait_id, sizeof y->wait_id, "%s", v->id);
        y->wait_t = app_time();
        return;
    }
    yt_play(v);
}

/* OK on a video: its sheet, Play first (OK, OK = play) - the same rule as everywhere (STATUS 104) */
static void video_sheet_done(void *ud, int idx) {
    Yt *y = ud;
    if (y != sheet_yt || idx < 0) return;
    Video *fv = focused_video(y);
    if (!fv) return;
    if (idx == 0) play_or_wait(y, fv);
    else if (idx == 1) app_push(info_screen(fv));
    else if (idx == 2 && fv->channel_id) app_push(channel_screen(fv->channel_id, fv->channel ? fv->channel : ""));
}
static void open_video_sheet(Yt *y, Video *fv) {
    const char *items[3] = {T("Play"), T("Info and comments"), T("Go to channel")};
    sheet_yt = y;
    choice_open(fv->title ? fv->title : "YouTube", items, fv->channel_id ? 3 : 2, 0, video_sheet_done, y);
}

static void yt_update(Screen *s, float dt) {
    Yt *y = s->st;
    side_update(&y->side, dt);
    Video *fv = focused_video(y);
    const char *ids[] = {pf_id(fv), fv ? pf_id(video_at(y, y->row, y->col + 1)) : NULL,
                         fv ? pf_id(video_at(y, y->row, y->col - 1)) : NULL, fv ? pf_id(video_at(y, y->row + 1, y->col)) : NULL};
    pf_consider(ids, 4);
    if (y->wait_id[0]) {
        if (!fv || strcmp(fv->id, y->wait_id)) y->wait_id[0] = 0; /* focus moved: cancel */
        else if (pf_find(fv->id) || strcmp(pf_busy_id, fv->id) || app_time() - y->wait_t > 25) {
            y->wait_id[0] = 0;
            yt_play(fv);
        }
    }
    if (y->cfg_seen != app_cfg_gen()) { /* new account uploaded via the phone */
        y->cfg_seen = app_cfg_gen();
        yt_resume(s);
    }
    if (y->has_chans) {
        float target = 0, avail = UI_W - (SIDE_W + 60) - 120;
        if (y->ccol * 230 > avail - 200) target = y->ccol * 230 - (avail - 200);
        y->csx = anim_to(y->csx, target, dt, 12);
    }
    if (y->page == PAGE_GRID) {
        int gr = y->row;
        float extra = y->has_chans && y->chans.n ? 300 : 0;
        float ty = gr < 0 ? 0 : gr <= 0 ? extra * 0.7f : extra + (gr - 1) * 360;
        if (ty < 0) ty = 0;
        y->sy = anim_to(y->sy, ty, dt, 12);
        return;
    }
    float ty = y->row <= 0 ? 0 : y->row * 400 - 40;
    y->sy = anim_to(y->sy, ty, dt, 12);
    float x0 = SIDE_W + 60, avail = UI_W - x0 - 120;
    for (int r = 0; r < y->n_rows; r++) {
        float target = 0;
        if (r == y->row) {
            float sel = y->col * 450;
            if (sel > avail - 420) target = sel - (avail - 420);
        }
        y->sx[r] = anim_to(y->sx[r], target, dt, 12);
    }
}

static void fmt_views(double v, char *b, size_t n) {
    if (v >= 1e6) snprintf(b, n, T("%.1fM views"), v / 1e6);
    else if (v >= 1e3) snprintf(b, n, T("%.0fK views"), v / 1e3);
    else if (v > 0) snprintf(b, n, T("%.0f views"), v);
    else b[0] = 0;
}

static void draw_video(Video *v, float x, float iy, bool f) {
    float fa = ui_focus(v, f), sc = 1 + 0.06f * fa, w = 420 * sc, h = 236 * sc, xx = x - (w - 420) / 2, yy = iy - (h - 236) / 2;
    ui_focus_under(xx, yy, w, h, 14, fa);
    fill_rrect(xx, yy, w, h, 14, C_SURFACE);
    char url[160];
    snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", v->id);
    img_draw_cover(img_get(url), xx, yy, w, h, 14, 1);
    if (v->is_live) {
        fill_rrect(xx + w - 90, yy + h - 44, 76, 32, 6, RGB(204, 0, 0));
        text_draw(F_BOLD, 18, xx + w - 52, yy + h - 38, C_TEXT, AL_CENTER, "LIVE");
    } else if (v->duration > 0) {
        char d[32];
        fmt_duration(v->duration, d, sizeof d);
        float dw = text_width(F_SEMI, 20, d) + 16;
        fill_rrect(xx + w - dw - 12, yy + h - 42, dw, 30, 6, RGBA(0, 0, 0, 200));
        text_draw(F_SEMI, 20, xx + w - dw / 2 - 12, yy + h - 38, C_TEXT, AL_CENTER, d);
    }
    ui_focus_over(xx, yy, w, h, 14, fa);
    text_draw_wrap(F_SEMI, 22, x, iy + 252, 420, 2, 28, f ? C_TEXT : C_TEXT2, v->title);
    char meta[160], vw[64];
    fmt_views(v->views, vw, sizeof vw);
    snprintf(meta, sizeof meta, "%s%s%s", v->channel ? v->channel : "", v->channel && vw[0] ? " · " : "", vw);
    text_draw_clip(F_REG, 20, x, iy + 310, 420, C_TEXT3, AL_LEFT, meta);
}

static void draw_channel_tile(Video *c, float x, float y, bool f) {
    float fa = ui_focus(c, f), r = 84 + 8 * fa, cx = x + 100, cy = y + 90;
    ui_focus_under(cx - r, cy - r, r * 2, r * 2, r, fa);
    fill_circle(cx, cy, r, C_SURFACE2);
    if (!img_draw_cover(img_get(c->thumb), cx - r, cy - r, r * 2, r * 2, r, 1)) {
        char ini[8] = {c->title && c->title[0] ? (char)c->title[0] : '?', 0};
        text_draw(F_BOLD, 64, cx, cy - 38, C_TEXT, AL_CENTER, ini);
    }
    ui_focus_over(cx - r, cy - r, r * 2, r * 2, r, fa);
    text_draw_clip(F_SEMI, 22, cx, y + 196, 210, f ? C_TEXT : C_TEXT2, AL_CENTER, c->title);
    if (c->views > 0) {
        char subs_s[48];
        if (c->views >= 1e6) snprintf(subs_s, sizeof subs_s, T("%.1fM subscribers"), c->views / 1e6);
        else if (c->views >= 1e3) snprintf(subs_s, sizeof subs_s, T("%.0fK subscribers"), c->views / 1e3);
        else snprintf(subs_s, sizeof subs_s, T("%.0f subscribers"), c->views);
        text_draw_clip(F_REG, 18, cx, y + 226, 210, C_TEXT3, AL_CENTER, subs_s);
    }
}

/* bottom bar with key hints: the content fades out behind it */
static void draw_hint_bar(float x0) {
    fill_vgrad(0, UI_H - 170, UI_W, 170, RGBA(7, 8, 12, 0), RGBA(7, 8, 12, 245)); /* the help text must not lie on top of the thumbnails */
    fill_vgrad(SIDE_W, UI_H - 150, UI_W - SIDE_W, 150, RGBA(7, 8, 12, 0), RGBA(7, 8, 12, 250));
    /* no button bar (it could not be reached in long lists): OK opens the video's sheet, ← the side menu */
    draw_hint(x0, UI_H - 62, "OK", T("Play, info, channel"));
    draw_hint(x0 + text_width(F_SEMI, 20, "OK") + 30 + text_width(F_REG, 22, T("Play, info, channel")) + 44, UI_H - 62, "←", T("Menu and search"));
}

static void yt_draw(Screen *s) {
    Yt *y = s->st;
    draw_background();
    draw_topbar(NULL);
    bool content_focus = !y->side.open;
    float x0 = SIDE_W + 60;
    clip_push(SIDE_W, 120, UI_W, UI_H - 120);
    if (y->page == PAGE_GRID && y->n_rows) {
        YRow *r = &y->rows[0];
        float top = 70 - y->sy;
        text_draw(F_BOLD, 44, x0, top, C_TEXT, AL_LEFT, r->title);
        if (y->has_chans && (y->chans.n || y->chans.loading)) {
            float cy = top + 80;
            text_draw(F_SEMI, 28, x0, cy, y->row < 0 && content_focus ? C_TEXT : C_TEXT2, AL_LEFT, T("Channels"));
            if (y->chans.loading && !y->chans.n) draw_spinner(x0 + 60, cy + 140, 20, C_TEXT2);
            for (int i = 0; i < y->chans.n; i++) {
                float x = x0 + i * 230 - y->csx;
                if (x > UI_W || x + 210 < SIDE_W) continue;
                draw_channel_tile(&y->chans.v[i], x, cy + 46, content_focus && y->row < 0 && y->ccol == i);
            }
            if (y->chans.n) top += 300;
        }
        if (r->loading && r->n == 0) draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT);
        else if (r->n == 0)
            text_draw(F_REG, 26, x0, top + 120, C_TEXT3, AL_LEFT, logged_in() || r->query[0] != ':' ?
                      T("Nothing found (or no connection to YouTube)") : T("Requires you to be signed in"));
        for (int i = 0; i < r->n; i++) {
            int gr = i / GCOLS, gc = i % GCOLS;
            float x = x0 + gc * 430, iy = top + 90 + gr * 360;
            if (iy > UI_H || iy + 360 < 0) continue;
            draw_video(&r->v[i], x, iy, content_focus && y->row == gr && y->col == gc);
        }
    } else {
        float ry = 120 - y->sy;
        if (y->chan_id[0]) { /* channel header with Subscribe */
            bool f = content_focus && y->row < 0, sub = subs_has(y->chan_id);
            text_draw_clip(F_BOLD, 52, x0, ry, 1000, C_TEXT, AL_LEFT, y->chan_name);
            const char *lbl = sub ? T("Subscribed") : T("Subscribe");
            float bw = text_width(F_SEMI, 26, lbl) + 100, bx = x0, by = ry + 80;
            Color bg = f ? C_FOCUS : sub ? RGBA(255, 255, 255, 36) : RGB(229, 45, 39);
            fill_rrect(bx, by, bw, 64, 32, bg);
            draw_icon(sub ? ICON_CHECK : ICON_STAR, bx + 38, by + 32, 24, f ? RGB(10, 10, 10) : C_TEXT);
            text_draw(F_SEMI, 26, bx + 64, by + 17, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, lbl);
            if (f && sub) text_draw(F_REG, 22, bx + bw + 20, by + 20, C_TEXT3, AL_LEFT, T("OK: unsubscribe"));
            ry += 200;
        }
        for (int r = 0; r < y->n_rows; r++) {
            YRow *row = &y->rows[r];
            text_draw(F_SEMI, 30, x0, ry, r == y->row && content_focus ? C_TEXT : C_TEXT2, AL_LEFT, row->title);
            float iy = ry + 56;
            if (row->loading && row->n == 0) draw_spinner(x0 + 100, iy + 110, 24, C_TEXT2);
            else if (row->failed) text_draw(F_REG, 24, x0, iy + 90, C_TEXT3, AL_LEFT, T("No results (or no connection to YouTube)"));
            for (int i = 0; i < row->n; i++) {
                float x = x0 + i * 450 - y->sx[r];
                if (x > UI_W || x + 420 < SIDE_W) continue;
                draw_video(&row->v[i], x, iy, content_focus && r == y->row && y->col == i);
            }
            ry += 400;
        }
    }
    clip_pop();
    if (!y->side.open && y->n_rows) draw_hint_bar(x0);
    side_draw(&y->side);
    if (y->wait_id[0]) {
        fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, 150));
        draw_spinner(UI_W / 2, UI_H / 2 - 30, 36, C_TEXT);
        text_draw(F_REG, 30, UI_W / 2, UI_H / 2 + 40, C_TEXT2, AL_CENTER, T("Loading video…"));
    }
}

/* the player menu; a leading "-" = shown greyed out, cannot be chosen (notatv-osd.lua): what this box cannot play */
static const char *yt_qualities[] = {"2160p", "1440p", "1080p", "720p", "480p", NULL};

/* recently watched videos (for the home screen): <data>/yt-watched.txt, id<TAB>title<TAB>channel<TAB>channel id */
#define N_WATCHED 12
static Video watched[N_WATCHED];
static int n_watched;
static void video_clear(Video *v) { free(v->id); free(v->title); free(v->channel); free(v->channel_id); free(v->thumb); }
static void watched_load(void) {
    for (int i = 0; i < n_watched; i++) video_clear(&watched[i]);
    memset(watched, 0, sizeof watched);
    n_watched = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/yt-watched.txt", data_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (n_watched < N_WATCHED && fgets(line, sizeof line, f)) {
        str_trim(line);
        char *p = line, *id = strsep(&p, "\t"), *t = strsep(&p, "\t"), *ch = strsep(&p, "\t"), *cid = strsep(&p, "\t");
        if (!id || !*id || !t) continue;
        Video *v = &watched[n_watched++];
        v->id = str_dup(id);
        v->title = str_dup(t);
        v->channel = ch && *ch ? str_dup(ch) : NULL;
        v->channel_id = cid && *cid ? str_dup(cid) : NULL;
        v->thumb = str_printf("https://i.ytimg.com/vi/%s/hqdefault.jpg", id);
    }
    fclose(f);
}
static void watched_add(const Video *v) {
    if (!v->id || v->is_live || v->is_channel) return;
    watched_load();
    char path[512];
    snprintf(path, sizeof path, "%s/yt-watched.txt", data_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\t%s\t%s\t%s\n", v->id, v->title ? v->title : "", v->channel ? v->channel : "", v->channel_id ? v->channel_id : "");
    for (int i = 0, w = 1; i < n_watched && w < N_WATCHED; i++) {
        if (!strcmp(watched[i].id, v->id)) continue;
        fprintf(f, "%s\t%s\t%s\t%s\n", watched[i].id, watched[i].title, watched[i].channel ? watched[i].channel : "",
                watched[i].channel_id ? watched[i].channel_id : "");
        w++;
    }
    fclose(f);
}

int youtube_recent(HomeItem **items) {
    static HomeItem hi[N_WATCHED];
    static char keys[N_WATCHED][16];
    watched_load();
    for (int i = 0; i < n_watched; i++) {
        snprintf(keys[i], sizeof keys[i], "yt:%d", i);
        hi[i] = (HomeItem){watched[i].title, watched[i].channel ? watched[i].channel : "YouTube", watched[i].thumb, keys[i], 0};
    }
    *items = hi;
    return n_watched;
}

void youtube_play_recent(int idx) {
    watched_load();
    if (idx < 0 || idx >= n_watched) return;
    Video v = watched[idx]; /* copy: yt_play_at -> watched_add reloads the list */
    v.id = str_dup(v.id); v.title = str_dup(v.title); v.channel = str_dup(v.channel);
    v.channel_id = str_dup(v.channel_id); v.thumb = NULL;
    yt_play_at(&v, 0);
    video_clear(&v);
}

static void video_clear(Video *v);
static bool yt_casting; /* playback started from the YouTube app (ytcast.c controls it) */
static void yt_cast_tick(void *ud) { (void)ud; ytcast_play_tick(); }

/* true = played to the end */
/* the player menu's own item: "Info and comments" stops the video and opens the info page */
static const char *yt_action_label(void *ud, int idx) { (void)ud; (void)idx; return T("Info and comments"); }
static bool yt_action(void *ud, int idx, char *msg, size_t n) { (void)ud; (void)idx; (void)msg; (void)n; return false; }

static bool yt_play_at(Video *v, double start) {
    watched_add(v);
    char url[128];
    snprintf(url, sizeof url, "https://www.youtube.com/watch?v=%s", v->id);
    char *f = yt_format();
    char *fmt = str_printf("--ytdl-format=%s", f);
    free(f);
    char ck[512], *ckopt = NULL;
    if (cookie_file(ck, sizeof ck)) ckopt = str_printf("--ytdl-raw-options-append=cookies=%s", ck);
    char subl[96];
    snprintf(subl, sizeof subl, "--ytdl-raw-options-append=sub-langs=%s", !strcmp(i18n_lang(), "da") ? (cfg_int("yt_autosubs", 1) ? "da.*,en.*" : "da,en") : (cfg_int("yt_autosubs", 1) ? "en.*" : "en"));
    /* subtitles cost ~1.6 s at start; off by default ("-.*" = no languages, ytdl_hook asks for all otherwise) */
    bool subs = cfg_int("yt_subs", 0);
    /* cast: the YouTube app sends the position in the stream - hours in for a livestream, and --start
       makes mpv stall on a live HLS without a known length. The OSD script instead jumps
       there after loading, but only if the video has a length (not live). */
    char *castopt = str_printf("--script-opts-append=notatv-start=%.1f", yt_casting ? start : 0.0);
    if (yt_casting) start = 0;
    const char *extra[] = {fmt, castopt, "--script-opts-append=ytdl_hook-try_ytdl_first=yes",
                           subs ? "--ytdl-raw-options-append=write-subs=" : "--ytdl-raw-options-append=sub-langs=-.*",
                           subs ? "--ytdl-raw-options-append=write-auto-subs=" : ckopt,
                           subs ? subl : NULL, subs ? ckopt : NULL, NULL};
    /* fast path: direct URLs found while the video was in focus (without subtitles) */
    Prefetch *p = v->is_live ? NULL : pf_find(v->id);
    char *afile = p && p->audio ? str_printf("--audio-file=%s", p->audio) : NULL;
    const char *fast[] = {"--ytdl=no", castopt, afile, NULL};
    char thumb[160], vw[64], sub[256];
    snprintf(thumb, sizeof thumb, "https://i.ytimg.com/vi/%s/hqdefault.jpg", v->id);
    fmt_views(v->views, vw, sizeof vw);
    snprintf(sub, sizeof sub, "%s%s%s", v->channel ? v->channel : "YouTube", vw[0] ? " · " : "", vw);
    char qnow[16];
    snprintf(qnow, sizeof qnow, "%dp", yt_quality());
    PlayRequest rq = {0};
    rq.url = p ? p->video : url;
    rq.title = v->title;
    rq.subtitle = sub;
    rq.poster = thumb;
    rq.start = start;
    rq.extra = p ? fast : extra;
    rq.qualities = v->is_live ? NULL : yt_qualities;
    rq.quality = qnow;
    rq.speed = !v->is_live;
    rq.channel = v->channel_id ? (v->channel ? v->channel : T("Channel")) : NULL;
    rq.is_live = false;
    if (yt_casting) rq.tick = yt_cast_tick;
    if (!p) app_toast(T("Loading video…"));
    /* "Info and comments" in the player menu (a NotaTV without it: not shown) */
    bool acts = player_set_actions && player_action;
    if (acts) player_set_actions(1, yt_action_label, yt_action, NULL);
    PlayResult res = player_run(&rq);
    int act = acts ? player_action() : 0;
    if (p && res.exit_code != 0 && res.dur <= 0 && !res.quality[0] && !res.goto_channel && !act) { /* the URLs had expired */
        p->id[0] = 0;
        rq.url = url;
        rq.extra = extra;
        if (acts) player_set_actions(1, yt_action_label, yt_action, NULL);
        res = player_run(&rq);
        act = acts ? player_action() : 0;
    }
    free(fmt);
    free(castopt);
    free(ckopt);
    free(afile);
    if (res.quality[0]) { /* new quality: start again at the same spot (without prefetch) */
        cfg_set_int("yt_quality", atoi(res.quality));
        if (p) p->id[0] = 0;
        app_toast(T("Quality: %s"), res.quality);
        return yt_play_at(v, res.pos);
    }
    if (res.goto_channel && v->channel_id) app_push(channel_screen(v->channel_id, v->channel ? v->channel : ""));
    if (act == 1) {
        Screen *is = info_screen(v);
        info_set_resume(is, v->is_live || (res.dur > 0 && res.pos > res.dur - 10) ? 0 : res.pos);
        app_push(is);
    }
    return res.eof;
}

/* YouTube cast: title and channel via oEmbed (public, no login), so the player can show them */
bool youtube_cast_play(const char *id, double start) {
    Video v = {0};
    v.id = str_dup(id);
    char *url = str_printf("https://www.youtube.com/oembed?format=json&url=https%%3A%%2F%%2Fwww.youtube.com%%2Fwatch%%3Fv%%3D%s", id);
    HttpBuf b;
    if (http_request("GET", url, NULL, NULL, &b, 4) && b.status == 200 && b.data) {
        cJSON *j = cJSON_Parse(b.data);
        cJSON *t = cJSON_GetObjectItem(j, "title"), *a = cJSON_GetObjectItem(j, "author_name");
        if (cJSON_IsString(t)) v.title = str_dup(t->valuestring);
        if (cJSON_IsString(a)) v.channel = str_dup(a->valuestring);
        cJSON_Delete(j);
    }
    http_free(&b);
    free(url);
    if (!v.title) v.title = str_dup("YouTube");
    yt_casting = true;
    bool ended = yt_play_at(&v, start);
    yt_casting = false;
    video_clear(&v);
    return ended;
}

static bool yt_input(Screen *s, const InputEvent *ev) {
    Yt *y = s->st;
    int act;
    if (side_input(&y->side, ev, &act)) {
        if (act >= 0) side_activate(y, act);
        return true;
    }
    YRow *gr = y->page == PAGE_GRID && y->n_rows ? &y->rows[0] : NULL;
    if (y->row < 0) { /* the main row: channel suggestions or the Subscribe button */
        switch (ev->a) {
        case A_DOWN: y->row = 0; y->col = 0; return true;
        case A_UP: return true;
        case A_LEFT:
            if (y->has_chans && y->page == PAGE_GRID && y->ccol > 0) y->ccol--;
            else side_open(&y->side);
            return true;
        case A_RIGHT: if (y->has_chans && y->page == PAGE_GRID && y->ccol < y->chans.n - 1) y->ccol++; return true;
        case A_OK:
            if (ev->repeat) return true;
            if (y->page == PAGE_GRID && y->has_chans && y->ccol < y->chans.n) {
                Video *c = &y->chans.v[y->ccol];
                app_push(channel_screen(c->channel_id, c->title));
            } else if (y->chan_id[0]) {
                if (subs_has(y->chan_id)) { subs_remove(y->chan_id); app_toast(T("Unsubscribed from %s"), y->chan_name); }
                else { subs_add(y->chan_id, y->chan_name); app_toast(T("You are now subscribed to %s"), y->chan_name); }
                qsort(subs, n_subs, sizeof(Sub), sub_cmp);
                subs_save();
                build_side(y);
            }
            return true;
        case A_BACK:
            if (y->page == PAGE_GRID && !y->chan_id[0]) { y->side.active = 0; show_home(y); return true; }
            return false;
        default: return false;
        }
    }
    switch (ev->a) {
    case A_UP:
        if (y->row == 0 && header_row(y)) { y->row = -1; return true; }
        if (gr) { if (y->row > 0) y->row--; }
        else if (y->row > 0) y->row--;
        if (!gr && y->col >= y->rows[y->row].n) y->col = y->rows[y->row].n ? y->rows[y->row].n - 1 : 0;
        return true;
    case A_DOWN:
        if (gr) {
            if (y->row >= grid_rows(gr) - 1) return false; /* bottom: the key bar */
            y->row++;
            if (y->row * GCOLS + y->col >= gr->n) y->col = (gr->n - 1) % GCOLS;
        } else if (y->row < y->n_rows - 1) {
            y->row++;
            if (y->col >= y->rows[y->row].n) y->col = y->rows[y->row].n ? y->rows[y->row].n - 1 : 0;
        } else return false;
        return true;
    case A_LEFT:
        if (y->col > 0) y->col--;
        else side_open(&y->side);
        return true;
    case A_RIGHT:
        if (gr) { if (y->col < GCOLS - 1 && y->row * GCOLS + y->col + 1 < gr->n) y->col++; }
        else if (y->n_rows && y->col < y->rows[y->row].n - 1) y->col++;
        return true;
    case A_OK: {
        Video *fv = focused_video(y);
        if (!fv) return true;
        if (!ev->repeat) open_video_sheet(y, fv);
        return true;
    }
    case A_PLAYPAUSE: case A_PLAY: { Video *fv = focused_video(y); if (fv) play_or_wait(y, fv); return true; }
    case A_INFO: { Video *fv = focused_video(y); if (fv) app_push(info_screen(fv)); return true; }
    case A_CHAR: {
        char init[8] = {(char)ev->ch, 0};
        keyboard_open(T("Search YouTube"), init, false, query_done, y);
        return true;
    }
    case A_MENU: side_open(&y->side); return true;
    case A_SEARCH: keyboard_open(T("Search YouTube"), "", false, query_done, y); return true;
    case A_BACK:
        if (y->page == PAGE_GRID) { y->side.active = 0; show_home(y); return true; }
        if (y->row > 0 || y->col > 0) { y->row = y->col = 0; return true; }
        return false;
    default: return false;
    }
}

Screen *youtube_screen(void) {
    Screen *s = xcalloc(1, sizeof *s);
    s->name = "youtube";
    s->st = xcalloc(1, sizeof(Yt));
    s->enter = yt_enter;
    s->resume = yt_resume;
    s->update = yt_update;
    s->draw = yt_draw;
    s->input = yt_input;
    s->destroy = yt_destroy;
    return s;
}

/* ================= Kanalside ================= */
static Screen *channel_screen(const char *id, const char *name) {
    Screen *s = youtube_screen();
    Yt *y = s->st;
    snprintf(y->chan_id, sizeof y->chan_id, "%s", id);
    snprintf(y->chan_name, sizeof y->chan_name, "%s", name && *name ? name : T("Channel"));
    s->name = "yt-channel";
    return s;
}

/* ================= Info page (description, channel, subscribe, comments) ================= */
typedef struct { char *author, *text; int likes; } Comment;
typedef struct {
    Video v;
    char *desc, *date;
    long likes;
    bool details_loading, comments_loading, show_comments;
    Comment *c;
    int n_c, sel, cscroll;
    int token;
    double resume; /* opened from the player menu: Play goes on from here */
} Info;
static void info_set_resume(Screen *s, double t) { ((Info *)s->st)->resume = t; }
static int info_token;

typedef struct { int token; char id[16]; char cookies[512]; bool comments; char *out; int gen; } InfoJob;

static void info_work(void *arg) {
    InfoJob *j = arg;
    char *url = str_printf("https://www.youtube.com/watch?v=%s", j->id);
    const char *argv[16];
    int n = 0;
    argv[n++] = "timeout"; argv[n++] = "60"; argv[n++] = "yt-dlp"; argv[n++] = "-J"; argv[n++] = "--skip-download"; argv[n++] = "--no-warnings";
    if (j->comments) { argv[n++] = "--write-comments"; argv[n++] = "--extractor-args"; argv[n++] = "youtube:max_comments=40,all,0,0;comment_sort=top"; }
    if (j->cookies[0]) { argv[n++] = "--cookies"; argv[n++] = j->cookies; }
    argv[n++] = "--"; argv[n++] = url; argv[n] = NULL;
    j->out = run_capture_argv(argv);
    free(url);
}

static Info *live_info;

static void info_done(void *arg) {
    InfoJob *j = arg;
    Info *in = live_info;
    if (j->gen != yt_gen) { free(j->out); free(j); return; } /* signed out */
    if (in && j->token == in->token) {
        cJSON *root = cJSON_Parse(j->out);
        if (!j->comments) {
            in->details_loading = false;
            cJSON *d = cJSON_GetObjectItem(root, "description"), *ud = cJSON_GetObjectItem(root, "upload_date");
            cJSON *lc = cJSON_GetObjectItem(root, "like_count"), *cid = cJSON_GetObjectItem(root, "channel_id");
            cJSON *ch = cJSON_GetObjectItem(root, "channel"), *vc = cJSON_GetObjectItem(root, "view_count");
            cJSON *du = cJSON_GetObjectItem(root, "duration");
            if (cJSON_IsString(d) && !in->desc) in->desc = str_dup(d->valuestring); /* the fast path may have it */
            if (cJSON_IsString(ud) && strlen(ud->valuestring) == 8 && !in->date)
                in->date = str_printf("%.2s.%.2s.%.4s", ud->valuestring + 6, ud->valuestring + 4, ud->valuestring);
            if (cJSON_IsNumber(lc)) in->likes = (long)lc->valuedouble;
            if (cJSON_IsString(cid) && !in->v.channel_id) in->v.channel_id = str_dup(cid->valuestring);
            if (cJSON_IsString(ch) && !in->v.channel) in->v.channel = str_dup(ch->valuestring);
            if (cJSON_IsNumber(vc)) in->v.views = vc->valuedouble;
            if (cJSON_IsNumber(du)) in->v.duration = du->valuedouble;
        } else {
            in->comments_loading = false;
            cJSON *c;
            cJSON *arr = cJSON_GetObjectItem(root, "comments");
            in->c = xcalloc(cJSON_GetArraySize(arr) + 1, sizeof(Comment));
            cJSON_ArrayForEach(c, arr) {
                cJSON *a = cJSON_GetObjectItem(c, "author"), *t = cJSON_GetObjectItem(c, "text"), *l = cJSON_GetObjectItem(c, "like_count");
                cJSON *par = cJSON_GetObjectItem(c, "parent");
                if (cJSON_IsString(par) && strcmp(par->valuestring, "root")) continue; /* top level only */
                if (!cJSON_IsString(t)) continue;
                Comment *cm = &in->c[in->n_c++];
                cm->author = str_dup(cJSON_IsString(a) ? a->valuestring : "");
                cm->text = str_dup(t->valuestring);
                cm->likes = cJSON_IsNumber(l) ? l->valueint : 0;
            }
            if (in->n_c == 0) app_toast(T("No comments (or they are turned off)"));
        }
        cJSON_Delete(root);
    }
    free(j->out);
    free(j);
}

/* fast path: YouTube's own player API answers in a fraction of a second with description, date, views, length
   and channel, while yt-dlp (below) takes seconds on the Pi and waits behind the stream lookups in JQ_SLOW.
   yt-dlp still runs afterwards for the likes (and as the fallback if this fails). */
typedef struct { int token; char id[16]; char *out; int gen; } QuickJob;
static void quick_work(void *arg) {
    QuickJob *j = arg;
    char *body = str_printf("{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20251030.01.00\"}},"
                            "\"videoId\":\"%s\"}", j->id);
    const char *hdr[] = {"Content-Type: application/json", NULL};
    HttpBuf b = {0};
    if (http_request("POST", "https://www.youtube.com/youtubei/v1/player?prettyPrint=false", hdr, body, &b, 8) && b.status == 200) {
        j->out = b.data;
        b.data = NULL;
    }
    http_free(&b);
    free(body);
}
static void quick_done(void *arg) {
    QuickJob *j = arg;
    if (j->gen != yt_gen) { free(j->out); free(j); return; } /* signed out */
    Info *in = live_info;
    cJSON *root = j->out ? cJSON_Parse(j->out) : NULL;
    cJSON *vd = cJSON_GetObjectItem(root, "videoDetails");
    if (in && j->token == in->token && in->details_loading && cJSON_IsObject(vd)) {
        cJSON *mf = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "microformat"), "playerMicroformatRenderer");
        cJSON *d = cJSON_GetObjectItem(vd, "shortDescription"), *vc = cJSON_GetObjectItem(vd, "viewCount");
        cJSON *ls = cJSON_GetObjectItem(vd, "lengthSeconds"), *au = cJSON_GetObjectItem(vd, "author");
        cJSON *cid = cJSON_GetObjectItem(vd, "channelId"), *pd = cJSON_GetObjectItem(mf, "publishDate");
        if (cJSON_IsString(d)) {
            in->details_loading = false;
            if (!in->desc) in->desc = str_dup(d->valuestring);
            if (cJSON_IsString(pd) && strlen(pd->valuestring) >= 10 && !in->date) /* 2009-10-24... */
                in->date = str_printf("%.2s.%.2s.%.4s", pd->valuestring + 8, pd->valuestring + 5, pd->valuestring);
            if (cJSON_IsString(vc)) in->v.views = atof(vc->valuestring);
            if (cJSON_IsString(ls) && atof(ls->valuestring) > 0) in->v.duration = atof(ls->valuestring);
            if (cJSON_IsString(cid) && !in->v.channel_id) in->v.channel_id = str_dup(cid->valuestring);
            if (cJSON_IsString(au) && !in->v.channel) in->v.channel = str_dup(au->valuestring);
        }
    }
    cJSON_Delete(root);
    free(j->out);
    free(j);
}

static void info_fetch(Info *in, bool comments) {
    if (!comments) {
        QuickJob *q = xcalloc(1, sizeof *q);
        q->token = in->token;
        q->gen = yt_gen;
        snprintf(q->id, sizeof q->id, "%s", in->v.id);
        job_submit_q(JQ_FAST, quick_work, quick_done, q);
    }
    InfoJob *j = xcalloc(1, sizeof *j);
    j->token = in->token;
    j->gen = yt_gen;
    j->comments = comments;
    snprintf(j->id, sizeof j->id, "%s", in->v.id);
    if (!cookie_file(j->cookies, sizeof j->cookies)) j->cookies[0] = 0;
    job_submit_q(JQ_SLOW, info_work, info_done, j);
}

enum { IB_PLAY, IB_CHANNEL, IB_SUB, IB_COMMENTS, IB_COUNT };

static void info_enter(Screen *s) {
    Info *in = s->st;
    live_info = in;
    in->token = ++info_token;
    in->details_loading = true;
    info_fetch(in, false);
}

static void info_destroy(Screen *s) {
    Info *in = s->st;
    if (live_info == in) live_info = NULL;
    free(in->v.id); free(in->v.title); free(in->v.channel); free(in->v.channel_id);
    free(in->desc); free(in->date);
    for (int i = 0; i < in->n_c; i++) { free(in->c[i].author); free(in->c[i].text); }
    free(in->c);
    free(in);
    free(s);
}

static void info_draw(Screen *s) {
    Info *in = s->st;
    draw_background();
    draw_topbar(NULL);
    char url[160];
    snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/hqdefault.jpg", in->v.id);
    float tx = 120, ty = 150, tw = 760, th = 428;
    fill_rrect(tx, ty, tw, th, 18, C_SURFACE);
    img_draw_cover(img_get(url), tx, ty, tw, th, 18, 1);
    /* knapper */
    const char *lbl[IB_COUNT] = {T("Play"), T("Go to channel"), subs_has(in->v.channel_id) ? T("Unsubscribe") : T("Subscribe"),
                                 in->show_comments ? T("Description") : T("Comments")};
    IconId ic[IB_COUNT] = {ICON_PLAY, ICON_TV, ICON_STAR, ICON_SEARCH};
    float bx = tx, by = ty + th + 40;
    for (int i = 0; i < IB_COUNT; i++) {
        bool f = in->sel == i && !(in->show_comments && in->cscroll >= 0 && in->sel == -1);
        float w = text_width(F_SEMI, 24, lbl[i]) + 90;
        if (bx + w > tx + tw + 40) { bx = tx; by += 84; }
        fill_rrect(bx, by, w, 66, 33, f ? C_FOCUS : RGBA(255, 255, 255, 36));
        draw_icon(ic[i], bx + 36, by + 33, 22, f ? RGB(10, 10, 10) : C_TEXT);
        text_draw(F_SEMI, 24, bx + 60, by + 19, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, lbl[i]);
        bx += w + 18;
    }
    /* text on the right */
    float rx = 940, rw = UI_W - rx - 120;
    text_draw_wrap(F_BOLD, 40, rx, ty, rw, 3, 50, C_TEXT, in->v.title);
    char meta[256], vw[64], d[32];
    fmt_views(in->v.views, vw, sizeof vw);
    snprintf(meta, sizeof meta, "%s%s%s%s%s", in->v.channel ? in->v.channel : "", vw[0] ? "  ·  " : "", vw,
             in->date ? "  ·  " : "", in->date ? in->date : "");
    text_draw_clip(F_REG, 24, rx, ty + 166, rw, C_TEXT2, AL_LEFT, meta);
    if (in->v.duration > 0 || in->likes > 0) {
        char m2[128];
        snprintf(m2, sizeof m2, "%s%s%s", in->v.duration > 0 ? fmt_duration(in->v.duration, d, sizeof d) : "",
                 in->likes > 0 ? "  ·  " : "", "");
        if (in->likes > 0) snprintf(m2 + strlen(m2), sizeof m2 - strlen(m2), T("%ld likes"), in->likes);
        text_draw(F_REG, 24, rx, ty + 202, C_TEXT3, AL_LEFT, m2);
    }
    float cy = ty + 260;
    if (!in->show_comments) {
        if (in->details_loading) draw_spinner(rx + 40, cy + 40, 20, C_TEXT2);
        else text_draw_wrap(F_REG, 24, rx, cy, rw, 18, 34, C_TEXT2, in->desc && *in->desc ? in->desc : T("No description"));
    } else {
        text_draw(F_SEMI, 28, rx, cy, C_TEXT, AL_LEFT, T("Comments"));
        if (in->comments_loading) { draw_spinner(rx + 40, cy + 100, 20, C_TEXT2); text_draw(F_REG, 22, rx + 80, cy + 88, C_TEXT3, AL_LEFT, T("Loading comments…")); }
        float y = cy + 56;
        clip_push(rx - 10, cy + 50, rw + 20, UI_H - cy - 110);
        for (int i = in->cscroll; i < in->n_c && y < UI_H - 60; i++) {
            Comment *c = &in->c[i];
            char head[200];
            snprintf(head, sizeof head, "%s%s", c->author, c->likes > 0 ? "" : "");
            text_draw_clip(F_SEMI, 22, rx, y, rw, C_TEXT, AL_LEFT, head);
            if (c->likes > 0) { char lk[32]; snprintf(lk, sizeof lk, "♥ %d", c->likes); text_draw(F_REG, 20, rx + rw, y + 2, C_TEXT3, AL_RIGHT, lk); }
            int lines = text_draw_wrap(F_REG, 22, rx, y + 32, rw, 4, 30, C_TEXT2, c->text);
            y += 32 + lines * 30 + 24;
        }
        clip_pop();
        if (in->n_c) draw_hint(rx, UI_H - 60, T("Up/down"), T("Scroll the comments (when Comments is selected)"));
    }
}

static bool info_input(Screen *s, const InputEvent *ev) {
    Info *in = s->st;
    switch (ev->a) {
    case A_LEFT: if (in->sel > 0) in->sel--; return true;
    case A_RIGHT: if (in->sel < IB_COUNT - 1) in->sel++; return true;
    case A_UP: if (in->show_comments && in->cscroll > 0) in->cscroll--; return true;
    case A_DOWN: if (in->show_comments && in->cscroll < in->n_c - 1) in->cscroll++; return true;
    case A_OK: case A_PLAYPAUSE:
        if (ev->repeat) return true;
        switch (in->sel) {
        case IB_PLAY: yt_play_at(&in->v, in->resume); break;
        case IB_CHANNEL:
            if (in->v.channel_id) app_push(channel_screen(in->v.channel_id, in->v.channel));
            else app_toast(T("The channel is not known yet - wait a moment"));
            break;
        case IB_SUB:
            if (!in->v.channel_id) { app_toast(T("The channel is not known yet - wait a moment")); break; }
            if (subs_has(in->v.channel_id)) { subs_remove(in->v.channel_id); app_toast(T("Unsubscribed")); }
            else { subs_add(in->v.channel_id, in->v.channel); app_toast(T("You are now subscribed to %s"), in->v.channel ? in->v.channel : T("the channel")); }
            qsort(subs, n_subs, sizeof(Sub), sub_cmp);
            subs_save();
            break;
        case IB_COMMENTS:
            in->show_comments = !in->show_comments;
            if (in->show_comments && !in->c && !in->comments_loading) { in->comments_loading = true; info_fetch(in, true); }
            break;
        }
        return true;
    default: return false;
    }
}

static Screen *info_screen(Video *src) {
    Screen *s = xcalloc(1, sizeof *s);
    Info *in = xcalloc(1, sizeof *in);
    in->v = *src;
    in->v.id = str_dup(src->id);
    in->v.title = str_dup(src->title);
    in->v.channel = str_dup(src->channel);
    in->v.channel_id = str_dup(src->channel_id);
    s->name = "yt-info";
    s->st = in;
    s->enter = info_enter;
    s->draw = info_draw;
    s->input = info_input;
    s->destroy = info_destroy;
    return s;
}

/* ================= Login / konti ================= */
typedef struct {
    char **names;
    int n, sel; /* sel: 0..n-1 profiles, n = without an account */
    int col;    /* 1 = the "Remove" button on the row */
    int cfg_seen;
    char last_prof[128];
    bool done; /* signed in: close when the screen is on top again */
} Login;

static void login_scan(Login *l) {
    for (int i = 0; i < l->n; i++) free(l->names[i]);
    free(l->names);
    l->names = NULL;
    l->n = 0;
    char dir[512];
    snprintf(dir, sizeof dir, "%s/youtube", data_dir());
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 8 && !strcmp(e->d_name + len - 8, ".cookies")) {
            l->names = xrealloc(l->names, sizeof(char *) * (l->n + 1));
            l->names[l->n++] = xstrndup(e->d_name, len - 8);
        }
    }
    if (d) closedir(d);
    if (l->sel > l->n) l->sel = l->n;
}

static void login_enter(Screen *s) {
    Login *l = s->st;
    login_scan(l);
    l->cfg_seen = app_cfg_gen();
    snprintf(l->last_prof, sizeof l->last_prof, "%s", cfg_get("yt_profile", ""));
}

static void login_update(Screen *s, float dt) {
    (void)dt;
    Login *l = s->st;
    if (l->done) { /* a dialog was on top when we signed in: close now that we are on top */
        if (app_top() == s) app_pop();
        return;
    }
    if (l->cfg_seen != app_cfg_gen()) { /* account uploaded via the phone */
        l->cfg_seen = app_cfg_gen();
        login_scan(l);
        const char *p = cfg_get("yt_profile", "");
        if (*p && strcmp(p, l->last_prof)) {
            app_toast(T("Signed in as %s"), p);
            l->done = true;
            if (app_top() == s) app_pop();
        }
    }
}

static void login_destroy(Screen *s) {
    Login *l = s->st;
    for (int i = 0; i < l->n; i++) free(l->names[i]);
    free(l->names);
    free(l);
    free(s);
}

static void login_draw(Screen *s) {
    Login *l = s->st;
    draw_background();
    draw_topbar(NULL);
    draw_icon(ICON_YOUTUBE, 150, 96, 60, RGB(255, 255, 255));
    text_draw(F_BOLD, 52, 210, 62, C_TEXT, AL_LEFT, T("Sign in to YouTube"));
    /* venstre: konti */
    float x = 120, y = 200;
    text_draw(F_SEMI, 28, x, y, C_TEXT2, AL_LEFT, T("Choose account"));
    y += 60;
    const char *cur = cfg_get("yt_profile", "");
    for (int i = 0; i <= l->n; i++) {
        bool f = l->sel == i;
        bool active = i < l->n ? !strcmp(cur, l->names[i]) : !*cur;
        fill_rrect(x, y, 700, 76, 18, f && l->col == 0 ? C_FOCUS : f ? C_SURFACE2 : C_SURFACE);
        bool rf = f && l->col == 0;
        Color tc = rf ? RGB(10, 10, 10) : C_TEXT;
        if (active) draw_icon(ICON_CHECK, x + 40, y + 38, 28, rf ? RGB(10, 10, 10) : C_OK);
        text_draw(F_SEMI, 28, x + 80, y + 21, tc, AL_LEFT, i < l->n ? l->names[i] : T("Without account"));
        if (i < l->n) { /* "Remove": → from the row */
            bool bf = f && l->col == 1;
            fill_rrect(x + 720, y + 12, 140, 52, 26, bf ? C_FOCUS : RGBA(255, 255, 255, 28));
            text_draw(F_SEMI, 24, x + 790, y + 24, bf ? RGB(10, 10, 10) : C_TEXT2, AL_CENTER, T("Remove"));
        }
        y += 90;
    }
    if (l->n == 0) text_draw(F_REG, 24, x, y + 10, C_TEXT3, AL_LEFT, T("No accounts yet - add one with your phone →"));
    /* right: QR + instructions */
    float qx = 1000, qy = 200;
    fill_rrect(qx - 30, qy - 30, 830, 760, 28, C_SURFACE);
    const char *ip = app_ip();
    char url[128];
    snprintf(url, sizeof url, "http://%s/#youtube", ip[0] ? ip : "notatv");
    draw_qr(url, qx, qy, 300);
    float tx = qx + 340;
    text_draw(F_SEMI, 28, tx, qy, C_TEXT, AL_LEFT, T("Add account"));
    static const char *steps[] = {
        N_("1. Sign in to youtube.com on your computer"),
        N_("2. Install the extension"),
        "    \"Get cookies.txt LOCALLY\"",
        N_("3. Export cookies.txt"),
        N_("4. Scan the QR code, give the account a"),
        N_("    name and choose the file"),
    };
    for (int i = 0; i < 6; i++) text_draw(F_REG, 22, tx, qy + 56 + i * 38, C_TEXT2, AL_LEFT, T(steps[i]));
    text_draw(F_REG, 22, qx, qy + 340, C_TEXT3, AL_LEFT, T("Or open this address in a browser on the same network:"));
    text_draw(F_SEMI, 30, qx, qy + 380, C_TEXT, AL_LEFT, url);
    text_draw_wrap(F_REG, 22, qx, qy + 450, 770, 4, 32, C_TEXT3,
                   T("Google does not allow direct login in third-party apps, so cookies from your browser are used. Cookies expire after a while - then just repeat the steps. The TV switches to the account automatically once it has been added."));
}

/* sign out of all YouTube accounts: cookies, subscriptions, history and what was watched */
void youtube_forget_all(void) {
    yt_gen++; /* replies from before the sign-out are ignored */
    char p[600];
    snprintf(p, sizeof p, "%s/youtube", data_dir());
    wipe_dir(p);
    const char *const files[] = {"yt-history.txt", "yt-watched.txt", "yt-subs.txt", NULL};
    for (int i = 0; files[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", data_dir(), files[i]);
        wipe_file(p);
    }
    cfg_set("yt_profile", "");
    ytcast_forget(); /* no cast device stays approved */
    wipe_session_traces();
}

static void remove_cb(void *ud, bool yes) {
    Login *l = ud;
    if (!yes || l->sel >= l->n) return;
    char path[600];
    snprintf(path, sizeof path, "%s/youtube/%s.cookies", data_dir(), l->names[l->sel]);
    wipe_file(path);
    if (!strcmp(cfg_get("yt_profile", ""), l->names[l->sel])) cfg_set("yt_profile", "");
    if (l->n <= 1) youtube_forget_all(); /* the last account: its subscriptions and history go too */
    app_toast(T("Account removed"));
    login_scan(l);
}

static bool login_input(Screen *s, const InputEvent *ev) {
    Login *l = s->st;
    if (l->done) return false; /* waiting to be closed */
    switch (ev->a) {
    case A_UP: if (l->sel > 0) l->sel--; l->col = 0; return true;
    case A_DOWN: if (l->sel < l->n) l->sel++; l->col = 0; return true;
    case A_RIGHT: if (l->sel < l->n) l->col = 1; return true;
    case A_LEFT: if (l->col) { l->col = 0; return true; } return false;
    case A_OK:
        if (l->col == 1 && l->sel < l->n) { InputEvent y = {A_YELLOW, 0, false}; l->col = 0; return login_input(s, &y); }
        cfg_set("yt_profile", l->sel < l->n ? l->names[l->sel] : "");
        app_toast(l->sel < l->n ? T("Account: %s") : T("Without account"), l->sel < l->n ? l->names[l->sel] : "");
        app_pop();
        return true;
    case A_YELLOW:
        if (l->sel < l->n) {
            char msg[200];
            snprintf(msg, sizeof msg, T("The account \"%s\" will be removed from NotaTV."), l->names[l->sel]);
            confirm_open(T("Remove account?"), msg, T("Remove"), T("Cancel"), remove_cb, l);
        }
        return true;
    default: return false;
    }
}

static Screen *login_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "yt-login";
    s->st = xcalloc(1, sizeof(Login));
    s->enter = login_enter;
    s->update = login_update;
    s->draw = login_draw;
    s->input = login_input;
    s->destroy = login_destroy;
    return s;
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
/* YouTube: "Link with TV code" (the code is fetched from YouTube in the background) */
typedef struct { char *code; } YtPair;
static void ytpair_work(void *a) { ((YtPair *)a)->code = ytcast_pairing_code(); }
static void ytpair_ok(void *ud, bool yes) { (void)ud; (void)yes; }
static void ytpair_done(void *a) {
    YtPair *p = a;
    if (!p->code) app_toast(T("Could not get a code from YouTube - try again"));
    else {
        char *msg = str_printf(T("Open YouTube on your phone: Settings → Watch on TV → Link with TV code, and enter\n\n%s\n\nThe code works for about 5 minutes."), p->code);
        confirm_open(T("YouTube: link with TV code"), msg, "OK", T("Close"), ytpair_ok, NULL);
        free(msg);
    }
    free(p->code);
    free(p);
}
static void act_ytpair(void) {
    app_toast(T("Getting code from YouTube…"));
    job_submit(ytpair_work, ytpair_done, xcalloc(1, sizeof(YtPair)));
}

/* low to high like the other number settings: Right = higher quality, Left = lower */
/* yt_init cuts the list at what the box can play (Settings cannot grey out a choice; the player menu does) */
static const char *ytq_o[] = {"480", "720", "1080", "1440", "2160", NULL}, *const ytq_l[] = {"480p", "720p", "1080p", "1440p", "4K", NULL};

static void act_yt_logout(void) { app_ask_signout("YouTube", youtube_forget_all); }
/* signed in = at least one account (<data>/youtube/<name>.cookies); looked up at most once a second */
static const char *yt_signed_out(void) {
    static double next;
    static bool any;
    if (app_time() >= next) {
        next = app_time() + 1;
        any = false;
        char p[600];
        snprintf(p, sizeof p, "%s/youtube", data_dir());
        DIR *d = opendir(p);
        if (d) {
            struct dirent *e;
            while (!any && (e = readdir(d))) {
                size_t len = strlen(e->d_name);
                any = len > 8 && !strcmp(e->d_name + len - 8, ".cookies");
            }
            closedir(d);
        }
    }
    return any ? NULL : ITEM_HIDDEN;
}
static const Item yt_items[] = {
    {N_("YouTube cast"), IT_TOGGLE, "ytcast_enabled", "1", set_onoff, .changed = ytcast_apply, .help = N_("The cast button in the YouTube app can play on NotaTV. Control goes through YouTube (Google)")},
    {N_("YouTube: link with TV code"), IT_ACTION, .action = act_ytpair, .help = N_("If NotaTV does not show up under Cast in the YouTube app: link with a code")},
    {N_("YouTube max quality"), IT_CYCLE, "yt_quality", "1080", ytq_o, ytq_l,
     .help = N_("Up to 1080p on a Raspberry Pi (YouTube has 1440p and 4K only in formats it cannot decode)")},
    {N_("YouTube subtitles"), IT_TOGGLE, "yt_subs", "0", set_onoff, .help = N_("Downloads English subtitles - makes start about 2 sec. slower")},
    {N_("Sign out of YouTube"), IT_ACTION, .action = act_yt_logout, .help = N_("Deletes the accounts (cookies), subscriptions, search history and what you have watched"),
     .locked = yt_signed_out},
};

static void yt_init(void) {
    if (yt_max() < 2160) {
        static const char *const grey[] = {"-2160p", "-1440p"};
        yt_qualities[0] = grey[0];
        yt_qualities[1] = grey[1];
        if (yt_max() < 1440) ytq_o[3] = NULL;
        if (cfg_int("yt_quality", 1080) > yt_max()) cfg_set_int("yt_quality", yt_max()); /* Settings shows it right */
    }
    ytcast_apply();
}

static void yt_row_open(const HomeItem *it, int idx) { (void)idx; if (it->key && !strncmp(it->key, "yt:", 3)) youtube_play_recent(atoi(it->key + 3)); }
/* the phone page sent login cookies for a YouTube account: added only when approved here on the TV */
typedef struct { char from[64]; char name[64]; } YtPending;
static void ytpending_cb(void *ud, bool yes) {
    YtPending *p = ud;
    char src[512], dst[512];
    snprintf(src, sizeof src, "%s/youtube/pending/%s.cookies", data_dir(), p->name);
    snprintf(dst, sizeof dst, "%s/youtube/%s.cookies", data_dir(), p->name);
    if (yes && rename(src, dst) == 0) { cfg_set("yt_profile", p->name); app_cfg_changed(); app_toast(T("YouTube account %s added"), p->name); }
    else { unlink(src); app_toast(T("YouTube account declined")); }
    free(p);
}
static void ytpending_show(const char *val) {
    YtPending *p = xcalloc(1, sizeof *p);
    sscanf(val, "%63[^\n]\n%63s", p->name, p->from);
    for (char *c = p->name; *c; c++) if (*c == '/' || *c == '.') *c = '_'; /* only a file name */
    char title[160], msg[256];
    snprintf(title, sizeof title, T("Add YouTube account \"%s\"?"), p->name);
    snprintf(msg, sizeof msg, T("The phone %s has sent login cookies for YouTube. The box uses them to watch as your account."), p->from);
    confirm_open(title, msg, T("Add"), T("Decline"), ytpending_cb, p);
}

static bool yt_inbox(const char *name, const char *val) {
    if (strcmp(name, "_ytpending")) return false;
    ytpending_show(val);
    return true;
}
const NotaApp nota_app = {
    NOTA_API, "youtube", "YouTube", N_("Search and watch videos on YouTube"), APP_VERSION, 40, 0,
    ICON_YOUTUBE, {229, 45, 39, 255}, {170, 18, 20, 255}, {255, 255, 255, 255},
    .open = youtube_screen, .init = yt_init, .tick = ytcast_tick, .apply = ytcast_apply, .forget = youtube_forget_all,
    .settings = yt_items, .n_settings = sizeof yt_items / sizeof yt_items[0],
    .row_title = N_("Recently watched"), .shelf_label = N_("RECENTLY WATCHED ON YOUTUBE"),
    .row = youtube_recent, .row_open = yt_row_open, .inbox = yt_inbox,
};
