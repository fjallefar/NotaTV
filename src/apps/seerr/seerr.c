/* NotaTV app package "seerr" - Seerr (Overseerr/Jellyseerr): discover and request movies and series */
#include "seerr.h"
#include "vendor/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TMDB_IMG "https://image.tmdb.org/t/p/"

static const char *seerr_url(void) { return cfg_get("seerr_url", ""); }
static void jar_path(char *p, size_t n) { snprintf(p, n, "%s/seerr.cookies", data_dir()); }

/* mediaInfo.status: 2 pending, 3 processing, 4 partially available, 5 available */
/* Netflix top 10: the country from the setup guide (Settings → Date and time) */
static const char *seerr_region_def(void) {
    const char *c = cfg_get("tz_country", "");
    return *c ? c : "US";
}
static const char *status_text(int st) {
    switch (st) {
    case 2: return T("Requested");
    case 3: return T("Processing");
    case 4: return T("Partly available");
    case 5: return T("Available");
    default: return NULL;
    }
}

typedef struct {
    int id, status, rank;
    char *type, *title, *poster, *backdrop, *year, *overview;
} SItem;

static void sitem_free(SItem *s) {
    free(s->type); free(s->title); free(s->poster); free(s->backdrop); free(s->year); free(s->overview);
}

static char *jdup(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? str_dup(v->valuestring) : NULL;
}

static void sitem_parse(SItem *s, cJSON *o) {
    memset(s, 0, sizeof *s);
    cJSON *id = cJSON_GetObjectItem(o, "id");
    s->id = cJSON_IsNumber(id) ? id->valueint : 0;
    s->type = jdup(o, "mediaType");
    s->title = jdup(o, "title");
    if (!s->title) s->title = jdup(o, "name");
    s->poster = jdup(o, "posterPath");
    s->backdrop = jdup(o, "backdropPath");
    s->overview = jdup(o, "overview");
    char *d = jdup(o, "releaseDate");
    if (!d) d = jdup(o, "firstAirDate");
    if (d) { s->year = strndup(d, 4); free(d); }
    cJSON *mi = cJSON_GetObjectItem(o, "mediaInfo");
    cJSON *st = cJSON_GetObjectItem(mi, "status");
    s->status = cJSON_IsNumber(st) ? st->valueint : 0;
}

/* ---------- HTTP against Seerr (with a session cookie) ---------- */
typedef struct {
    char *path, *body;
    const char *method;
    cJSON *json;
    long status;
    int token;
    bool keep; /* ignore seerr_token (the caller keeps track of its lifetime itself) */
    void (*done)(void *ud, cJSON *json, long status);
    void *ud;
} SReq;
static int seerr_token; /* increased when screens close */

static void sreq_work(void *arg) {
    SReq *r = arg;
    char url[1024], jar[512];
    snprintf(url, sizeof url, "%s%s", seerr_url(), r->path);
    jar_path(jar, sizeof jar);
    static const char *const hdr[] = {"Accept: application/json", "Content-Type: application/json", NULL};
    HttpBuf b;
    if (http_request_cookies(r->method, url, hdr, r->body, &b, 20, jar)) {
        r->status = b.status;
        if (b.data) r->json = cJSON_Parse(b.data);
    }
    http_free(&b);
}

static void sreq_done(void *arg) {
    SReq *r = arg;
    if ((r->keep || r->token == seerr_token) && r->done) r->done(r->ud, r->json, r->status);
    cJSON_Delete(r->json);
    free(r->path);
    free(r->body);
    free(r);
}

static void seerr_call_ex(const char *method, const char *path, const char *body,
                          void (*done)(void *, cJSON *, long), void *ud, bool keep) {
    SReq *r = calloc(1, sizeof *r);
    r->keep = keep;
    r->method = method;
    r->path = str_dup(path);
    r->body = str_dup(body);
    r->done = done;
    r->ud = ud;
    r->token = seerr_token;
    job_submit(sreq_work, sreq_done, r);
}

static void seerr_call(const char *method, const char *path, const char *body,
                       void (*done)(void *, cJSON *, long), void *ud) {
    seerr_call_ex(method, path, body, done, ud, false);
}

/* ---------- ratings for other apps (Plex) ---------- */
typedef struct { bool tv, retried; int tmdb; void (*cb)(void *, int, int, int); void *ud; } RatCtx;
static void rat_fetch(RatCtx *c);
static void rat_login_done(void *ud, cJSON *json, long status) {
    (void)json;
    RatCtx *c = ud;
    if (status == 200) rat_fetch(c);
    else free(c);
}
static void rat_done(void *ud, cJSON *json, long status) {
    RatCtx *c = ud;
    if ((status == 401 || status == 403) && !c->retried && *cfg_get("plex_token", "")) {
        c->retried = true; /* session expired: sign in with Plex and try again */
        char *body = str_printf("{\"authToken\":\"%s\"}", cfg_get("plex_token", ""));
        seerr_call_ex("POST", "/api/v1/auth/plex", body, rat_login_done, c, true);
        free(body);
        return;
    }
    int rc = -1, ra = -1, im = -1;
    if (status == 200 && json) {
        cJSON *rt = cJSON_GetObjectItem(json, "rt"), *imdb = cJSON_GetObjectItem(json, "imdb");
        if (!rt && cJSON_GetObjectItem(json, "criticsScore")) rt = json;
        cJSON *a = cJSON_GetObjectItem(rt, "criticsScore"), *b = cJSON_GetObjectItem(rt, "audienceScore");
        cJSON *i = cJSON_GetObjectItem(imdb, "criticsScore");
        if (cJSON_IsNumber(a)) rc = a->valueint;
        if (cJSON_IsNumber(b)) ra = b->valueint;
        if (cJSON_IsNumber(i)) im = (int)(i->valuedouble * 10 + 0.5);
    }
    c->cb(c->ud, rc, ra, im);
    free(c);
}
static void rat_fetch(RatCtx *c) {
    char *p = str_printf("/api/v1/%s/%d/%s", c->tv ? "tv" : "movie", c->tmdb, c->tv ? "ratings" : "ratingscombined");
    seerr_call_ex("GET", p, NULL, rat_done, c, true);
    free(p);
}
void seerr_ratings(bool tv, int tmdb, void (*cb)(void *ud, int rt_critic, int rt_audience, int imdb10), void *ud) {
    if (!*seerr_url() || tmdb <= 0) return;
    RatCtx *c = calloc(1, sizeof *c);
    c->tv = tv; c->tmdb = tmdb; c->cb = cb; c->ud = ud;
    rat_fetch(c);
}

/* ---------- main screen ---------- */
#define SCOLS 6
typedef struct {
    char *title, *path;
    SItem *it;
    int n, cap;
    bool loading, recent; /* recent: /media only gives ids - details are fetched per item */
    int pending;
} SRow;

enum { V_DISCOVER, V_GRID };
enum { SM_DISCOVER, SM_SEARCH, SM_RECENT, SM_MOVIES, SM_TV, SM_NETFLIX, SM_UPCOMING, SM_FILTER, SM_SETTINGS };

typedef struct {
    int state; /* 0 setup, 1 signing in, 2 ready, 3 login failed */
    char msg[200], user[128];
    SRow rows[8];
    int n_rows, row, col, view;
    float sy, sx[8];
    SideMenu side;
    int side_map[16];
    char grid_title[200];
    /* info at the top that follows the title you are on (like Plex's focus style), drawn in a layer */
    Layer *bgl;
    char bg_url[300], prev_url[300], bgl_key[320];
    float bg_fade, bgl_fade;
    int btn; /* button in focus on the setup screen */
} Seerr;
#define SHDR 330 /* height of the info area above the rows */
#define SBG_H 560

static Seerr *live;
static Screen *seerr_detail(SItem *it);

/* filter: 0 both, 1 movies only, 2 series only */
static int sfilter(void) { return cfg_int("seerr_filter", 0); }
static bool type_ok(const char *t) {
    int f = sfilter();
    if (f == 1) return t && !strcmp(t, "movie");
    if (f == 2) return t && !strcmp(t, "tv");
    return true;
}

static void row_push(SRow *r, SItem *it) {
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 24; r->it = realloc(r->it, sizeof(SItem) * r->cap); }
    int pos = r->n;
    if (it->rank > 0) /* Netflix top 10: insert in ranking order */
        while (pos > 0 && (r->it[pos - 1].rank == 0 || r->it[pos - 1].rank > it->rank)) pos--;
    memmove(r->it + pos + 1, r->it + pos, sizeof(SItem) * (r->n - pos));
    r->it[pos] = *it;
    r->n++;
}

static void row_done(void *ud, cJSON *json, long status) {
    SRow *r = ud;
    r->loading = false;
    if (status != 200 || !json) return;
    cJSON *res = cJSON_GetObjectItem(json, "results"), *o;
    cJSON_ArrayForEach(o, res) {
        char *mt = jdup(o, "mediaType");
        bool ok = !mt || !strcmp(mt, "movie") || !strcmp(mt, "tv");
        free(mt);
        if (!ok) continue; /* skip people */
        SItem it;
        sitem_parse(&it, o);
        if (!it.type) it.type = str_dup(strstr(r->path, "/tv") ? "tv" : "movie");
        if (!type_ok(it.type)) { sitem_free(&it); continue; }
        row_push(r, &it);
    }
}

/* "recently added": fetch details for each media item */
typedef struct { SRow *r; } RecentCtx;
static void recent_item_done(void *ud, cJSON *json, long status) {
    SRow *r = ud;
    r->pending--;
    if (status != 200 || !json) return;
    SItem it;
    sitem_parse(&it, json);
    cJSON *mi = cJSON_GetObjectItem(json, "mediaInfo"), *st = cJSON_GetObjectItem(mi, "status");
    if (cJSON_IsNumber(st)) it.status = st->valueint;
    if (!it.type) it.type = str_dup(jdup(json, "firstAirDate") ? "tv" : "movie");
    row_push(r, &it);
}
static void recent_done(void *ud, cJSON *json, long status) {
    SRow *r = ud;
    r->loading = false;
    if (status != 200 || !json) return;
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(json, "results")) {
        cJSON *tid = cJSON_GetObjectItem(o, "tmdbId");
        char *mt = jdup(o, "mediaType");
        if (cJSON_IsNumber(tid) && mt && type_ok(mt)) {
            char *p = str_printf("/api/v1/%s/%d?language=%s", strcmp(mt, "tv") ? "movie" : "tv", tid->valueint, i18n_lang());
            r->pending++;
            seerr_call("GET", p, NULL, recent_item_done, r);
            free(p);
        }
        free(mt);
    }
}

/* ---- Netflix top 10 (Netflix's official weekly data, last week for the country) ---- */
typedef struct { SRow *r; bool tv; int rank; char *title; } NfFind;
typedef struct { SRow *r; bool tv; int token; char region[8]; char titles[10][256]; int n; int ranks[10]; } NfJob;

static void nf_search_done(void *ud, cJSON *json, long status) {
    NfFind *f = ud;
    f->r->pending--;
    if (status == 200 && json) {
        cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItem(json, "results")) {
            char *mt = jdup(o, "mediaType");
            bool match = mt && !strcmp(mt, f->tv ? "tv" : "movie");
            free(mt);
            if (!match) continue;
            SItem it;
            sitem_parse(&it, o);
            it.rank = f->rank;
            row_push(f->r, &it);
            break;
        }
    }
    free(f->title);
    free(f);
}

static void nf_work(void *arg) {
    NfJob *j = arg;
    char path[512];
    snprintf(path, sizeof path, "%s/netflix-top10.tsv", data_dir());
    struct stat st;
    if (stat(path, &st) != 0 || time(NULL) - st.st_mtime > 24 * 3600)
        http_download_ua("https://www.netflix.com/tudum/top10/data/all-weeks-countries.tsv", path, 120, "Mozilla/5.0");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024], best_week[16] = "";
    const char *cat = j->tv ? "TV" : "Films";
    /* two passes: find the latest week, then gather the top 10 */
    for (int pass = 0; pass < 2; pass++) {
        rewind(f);
        while (fgets(line, sizeof line, f)) {
            char *fld[8];
            int n = 0;
            for (char *p = line, *t; n < 8 && (t = strsep(&p, "\t\n")); ) fld[n++] = t;
            if (n < 7 || strcmp(fld[1], j->region) || strncmp(fld[3], cat, strlen(cat))) continue;
            if (pass == 0) { if (strcmp(fld[2], best_week) > 0) snprintf(best_week, sizeof best_week, "%s", fld[2]); }
            else if (!strcmp(fld[2], best_week) && j->n < 10) {
                j->ranks[j->n] = atoi(fld[4]);
                snprintf(j->titles[j->n], sizeof j->titles[0], "%s", fld[5]);
                j->n++;
            }
        }
    }
    fclose(f);
}

static void nf_done(void *arg) {
    NfJob *j = arg;
    if (j->token == seerr_token) {
        j->r->loading = false;
        for (int i = 0; i < j->n; i++) {
            NfFind *f = calloc(1, sizeof *f);
            f->r = j->r; f->tv = j->tv; f->rank = j->ranks[i]; f->title = str_dup(j->titles[i]);
            char *e = url_encode(j->titles[i]);
            char *p = str_printf("/api/v1/search?query=%s&page=1", e);
            j->r->pending++;
            seerr_call("GET", p, NULL, nf_search_done, f);
            free(e);
            free(p);
        }
    }
    free(j);
}

static void free_rows(Seerr *v) {
    for (int i = 0; i < v->n_rows; i++) {
        for (int k = 0; k < v->rows[i].n; k++) sitem_free(&v->rows[i].it[k]);
        free(v->rows[i].it);
        free(v->rows[i].title);
        free(v->rows[i].path);
    }
    v->n_rows = 0;
    seerr_token++; /* ignore replies for the old rows */
}

static void add_row(Seerr *v, const char *title, const char *path) {
    if (v->n_rows >= 8) return;
    SRow *r = &v->rows[v->n_rows];
    memset(r, 0, sizeof *r);
    r->title = str_dup(title);
    r->path = str_dup(path);
    r->loading = true;
    v->sx[v->n_rows] = 0;
    v->n_rows++;
    if (!strncmp(path, "/api/v1/media?", 14)) { r->recent = true; seerr_call("GET", path, NULL, recent_done, r); }
    else if (!strncmp(path, "@netflix:", 9)) {
        NfJob *j = calloc(1, sizeof *j);
        j->r = r;
        j->tv = !strcmp(path + 9, "tv");
        j->token = seerr_token;
        snprintf(j->region, sizeof j->region, "%s", cfg_get("seerr_region", seerr_region_def()));
        job_submit_q(JQ_SLOW, nf_work, nf_done, j);
    } else seerr_call("GET", path, NULL, row_done, r);
}

#define RECENT_PATH "/api/v1/media?filter=allavailable&take=20&skip=0&sort=mediaAdded"


static void show_discover(Seerr *v) {
    free_rows(v);
    v->view = V_DISCOVER;
    int f = sfilter();
    char p[300];
    add_row(v, T("Trending now"), "/api/v1/discover/trending?page=1");
    add_row(v, T("Recently added"), RECENT_PATH);
    (void)p;
    if (f != 2) add_row(v, T("Popular movies"), "/api/v1/discover/movies?page=1");
    if (f != 1) add_row(v, T("Popular shows"), "/api/v1/discover/tv?page=1");
    if (f != 2) add_row(v, T("Upcoming movies"), "/api/v1/discover/movies/upcoming?page=1");
    v->row = v->col = 0;
}

static void show_grid(Seerr *v, const char *title, const char *path) {
    free_rows(v);
    v->view = V_GRID;
    snprintf(v->grid_title, sizeof v->grid_title, "%s", title);
    add_row(v, title, path);
    v->row = v->col = 0;
}

static const char *filter_label(void) {
    static const char *l[] = {N_("Show: movies and shows"), N_("Show: movies only"), N_("Show: shows only")};
    int f = sfilter();
    return T(l[f >= 0 && f <= 2 ? f : 0]);
}

static void build_side(Seerr *v) {
    SideMenu *m = &v->side;
    int keep = m->n ? v->side_map[m->active] : SM_DISCOVER;
    side_clear(m);
    snprintf(m->subtitle, sizeof m->subtitle, "%s", v->user);
#define ADD(lbl, ic, gap, id) do { v->side_map[side_add(m, lbl, ic, gap)] = id; } while (0)
    ADD(T("Discover"), ICON_STAR, false, SM_DISCOVER);
    ADD(T("Search"), ICON_SEARCH, false, SM_SEARCH);
    ADD(T("Recently added"), ICON_CHECK, true, SM_RECENT);
    if (sfilter() != 2) ADD(T("Popular movies"), ICON_FILM, false, SM_MOVIES);
    if (sfilter() != 1) ADD(T("Popular shows"), ICON_TV, false, SM_TV);
    ADD(T("Netflix top 10"), ICON_PLAY, false, SM_NETFLIX);
    if (sfilter() != 2) ADD(T("Upcoming movies"), ICON_MOON, false, SM_UPCOMING);
    ADD(filter_label(), ICON_GEAR, true, SM_FILTER);
    ADD(T("Server and login"), ICON_LOCK, false, SM_SETTINGS);
#undef ADD
    m->active = 0;
    for (int i = 0; i < m->n; i++) if (v->side_map[i] == keep) m->active = i;
}

static void me_done(void *ud, cJSON *json, long status);

static void login_done(void *ud, cJSON *json, long status) {
    (void)json;
    Seerr *v = ud;
    if (status == 200) seerr_call("GET", "/api/v1/auth/me", NULL, me_done, v);
    else {
        v->state = 3;
        if (status == 0) snprintf(v->msg, sizeof v->msg, T("Cannot reach Seerr at %s"), seerr_url());
        else if (status == 401 || status == 403) snprintf(v->msg, sizeof v->msg, T("Your user has no access to Seerr - ask the administrator"));
        else snprintf(v->msg, sizeof v->msg, T("Login failed (HTTP %ld)"), status);
    }
}

static void login(Seerr *v) {
    v->state = 1;
    snprintf(v->msg, sizeof v->msg, T("Signing in…"));
    const char *pt = cfg_get("plex_token", "");
    if (*pt) {
        char *body = str_printf("{\"authToken\":\"%s\"}", pt);
        seerr_call("POST", "/api/v1/auth/plex", body, login_done, v);
        free(body);
    } else {
        v->state = 3;
        snprintf(v->msg, sizeof v->msg, T("Sign in to Plex first, or use local login in the menu"));
    }
}

static void me_done(void *ud, cJSON *json, long status) {
    Seerr *v = ud;
    if (status == 200 && json) {
        char *dn = jdup(json, "displayName");
        if (!dn) dn = jdup(json, "plexUsername");
        snprintf(v->user, sizeof v->user, "%s", dn ? dn : "");
        free(dn);
        v->state = 2;
        v->msg[0] = 0;
        build_side(v);
        show_discover(v);
    } else login(v); /* the session has expired: sign in again */
}

static void url_done(void *ud, const char *text) {
    Seerr *v = ud;
    if (!text || !*text || v != live) return;
    char *t = str_dup(text);
    str_trim(t);
    size_t n = strlen(t);
    while (n && t[n - 1] == '/') t[--n] = 0;
    char *u = strstr(t, "://") ? str_dup(t) : str_printf("http://%s", t);
    cfg_set("seerr_url", u);
    free(t);
    free(u);
    char jar[512];
    jar_path(jar, sizeof jar);
    unlink(jar);
    login(v);
}

static char local_email[200];
static void pw_done(void *ud, const char *pw) {
    Seerr *v = ud;
    if (!pw || v != live) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "email", local_email);
    cJSON_AddStringToObject(o, "password", pw);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    v->state = 1;
    seerr_call("POST", "/api/v1/auth/local", body, login_done, v);
    free(body);
}
static void email_done(void *ud, const char *e) {
    if (!e || !*e) return;
    snprintf(local_email, sizeof local_email, "%s", e);
    keyboard_open(T("Seerr: password"), "", true, pw_done, ud);
}

static void settings_cb(void *ud, int idx) {
    Seerr *v = ud;
    if (v != live) return;
    if (idx == 0) keyboard_open(T("Seerr address (e.g. 192.168.1.10:5055)"), seerr_url(), false, url_done, v);
    else if (idx == 1) keyboard_open(T("Seerr: email"), "", false, email_done, v);
    else if (idx == 2) login(v);
}
static void open_settings(Seerr *v) {
    const char *items[] = {T("Change Seerr address"), T("Local login (email)"), T("Sign in with Plex again")};
    choice_open("Seerr", items, 3, 0, settings_cb, v);
}

static void search_done(void *ud, const char *q) {
    Seerr *v = ud;
    if (!q || !*q || v != live) return;
    char *e = url_encode(q);
    char *path = str_printf("/api/v1/search?query=%s&page=1", e);
    char *t = str_printf(T("Results for \"%s\""), q);
    show_grid(v, t, path);
    free(e); free(path); free(t);
}

static void side_activate(Seerr *v, int idx) {
    int id = v->side_map[idx];
    char p[300];
    switch (id) {
    case SM_DISCOVER: v->side.active = idx; show_discover(v); break;
    case SM_SEARCH: v->side.active = idx; keyboard_open(T("Search Seerr"), "", false, search_done, v); break;
    case SM_RECENT: v->side.active = idx; show_grid(v, T("Recently added"), RECENT_PATH); break;
    case SM_MOVIES: v->side.active = idx; show_grid(v, T("Popular movies"), "/api/v1/discover/movies?page=1"); break;
    case SM_TV: v->side.active = idx; show_grid(v, T("Popular shows"), "/api/v1/discover/tv?page=1"); break;
    case SM_UPCOMING: v->side.active = idx; show_grid(v, T("Upcoming movies"), "/api/v1/discover/movies/upcoming?page=1"); break;
    case SM_NETFLIX:
        v->side.active = idx;
        free_rows(v);
        v->view = V_DISCOVER;
        (void)p;
        {
            char t1[160], t2[160];
            const char *cn = tz_country_name(seerr_region_def());
            snprintf(t1, sizeof t1, T("Netflix top 10 · movies (%s, this week)"), cn);
            snprintf(t2, sizeof t2, T("Netflix top 10 · shows (%s, this week)"), cn);
            if (sfilter() != 2) add_row(v, t1, "@netflix:film");
            if (sfilter() != 1) add_row(v, t2, "@netflix:tv");
        }
        v->row = v->col = 0;
        break;
    case SM_FILTER:
        cfg_set_int("seerr_filter", (sfilter() + 1) % 3);
        app_toast("%s", filter_label());
        build_side(v);
        show_discover(v);
        side_open(&v->side);
        for (int i = 0; i < v->side.n; i++) if (v->side_map[i] == SM_FILTER) v->side.sel = i;
        break;
    case SM_SETTINGS: open_settings(v); break;
    }
}

static void seerr_enter(Screen *s) {
    Seerr *v = s->st;
    live = v;
    seerr_token++;
    side_init(&v->side, "Seerr", ICON_STAR, RGB(129, 140, 248));
    build_side(v);
    if (!*seerr_url()) { v->state = 0; return; }
    v->state = 1;
    snprintf(v->msg, sizeof v->msg, T("Connecting…"));
    seerr_call("GET", "/api/v1/auth/me", NULL, me_done, v);
}

static void seerr_destroy(Screen *s) {
    Seerr *v = s->st;
    free_rows(v);
    layer_free(v->bgl);
    live = NULL;
    free(v);
    free(s);
}

static void seerr_update(Screen *s, float dt) {
    Seerr *v = s->st;
    side_update(&v->side, dt);
    if (v->view == V_GRID) {
        float ty = v->row <= 0 ? 0 : v->row * 480 - 200;
        v->sy = anim_to(v->sy, ty < 0 ? 0 : ty, dt, 12);
        return;
    }
    float ty = v->row * 530.0f; /* the active row is right below the info area */
    v->sy = anim_to(v->sy, ty, dt, 12);
    v->bg_fade = anim_to(v->bg_fade, 1, dt, 6);
    float avail = UI_W - (SIDE_W + 60) - 120;
    for (int r = 0; r < v->n_rows; r++) {
        float target = 0;
        if (r == v->row) {
            float sel = v->col * 280;
            if (sel > avail - 250) target = sel - (avail - 250);
        }
        v->sx[r] = anim_to(v->sx[r], target, dt, 12);
    }
}

static void draw_poster(SItem *it, float x, float y, float w, float h, bool f) {
    float sc = f ? 1.07f : 1, ww = w * sc, hh = h * sc, xx = x - (ww - w) / 2, yy = y - (hh - h) / 2;
    if (f) draw_shadow(xx, yy, ww, hh, 14, 26, 1);
    fill_rrect(xx, yy, ww, hh, 14, C_SURFACE);
    char url[300] = "";
    if (it->poster) snprintf(url, sizeof url, TMDB_IMG "w342%s", it->poster);
    if (!img_draw_cover(img_get(url[0] ? url : NULL), xx, yy, ww, hh, 14, 1))
        text_draw_wrap(F_SEMI, 24, xx + 16, yy + 20, ww - 32, 4, 30, C_TEXT2, it->title);
    const char *st = status_text(it->status);
    if (st) {
        Color c = it->status >= 4 ? C_OK : RGB(99, 102, 241);
        float bw = text_width(F_BOLD, 18, st) + 20;
        fill_rrect(xx + 10, yy + 10, bw, 30, 15, c);
        text_draw(F_BOLD, 18, xx + 10 + bw / 2, yy + 15, C_TEXT, AL_CENTER, st);
    }
    if (it->rank > 0) {
        char rk[8];
        snprintf(rk, sizeof rk, "%d", it->rank);
        fill_rrect(xx + ww - 58, yy + hh - 58, 48, 48, 12, RGB(229, 9, 20));
        text_draw(F_BOLD, 28, xx + ww - 34, yy + hh - 51, C_TEXT, AL_CENTER, rk);
    }
    if (f) stroke_rrect(xx - 4, yy - 4, ww + 8, hh + 8, 18, 4, C_FOCUS);
    text_draw_clip(F_SEMI, 22, x, y + h + 14, w, f ? C_TEXT : C_TEXT2, AL_LEFT, it->title);
    char sub[64];
    snprintf(sub, sizeof sub, "%s%s%s", it->year ? it->year : "", it->year ? " · " : "", it->type && !strcmp(it->type, "tv") ? T("Show") : T("Movie"));
    text_draw_clip(F_REG, 20, x, y + h + 42, w, C_TEXT3, AL_LEFT, sub);
}

static void seerr_draw(Screen *s) {
    Seerr *v = s->st;
    draw_background();
    draw_topbar(NULL);
    float x0 = SIDE_W + 60, cx = x0 + (UI_W - x0) / 2;
    if (v->state != 2) {
        draw_icon(ICON_STAR, cx, 300, 120, RGB(129, 140, 248));
        if (v->state == 0) {
            text_draw(F_BOLD, 42, cx, 430, C_TEXT, AL_CENTER, T("Connect to Seerr"));
            text_draw_wrap(F_REG, 28, cx - 520, 500, 1040, 6, 40, C_TEXT2,
                           T("Find and request movies and shows for your Plex server.\n\nEnter the address of your Overseerr/Jellyseerr/Seerr server, e.g. 192.168.1.10:5055. You are signed in with your Plex account."));
        } else if (v->state == 1) {
            draw_spinner(cx, 520, 30, C_TEXT);
            text_draw(F_REG, 26, cx, 580, C_TEXT2, AL_CENTER, v->msg);
        } else {
            text_draw(F_BOLD, 42, cx, 430, C_TEXT, AL_CENTER, T("Could not sign in"));
            text_draw(F_REG, 28, cx, 500, C_TEXT2, AL_CENTER, v->msg);
        }
        if (v->state != 1) {
            bool retry = v->state == 3 && *seerr_url();
            const char *b[2] = {retry ? T("Try again") : T("Enter address"), T("Change address")};
            draw_buttons(cx, 780, b, retry ? 2 : 1, v->side.open ? -1 : v->btn);
            text_draw(F_REG, 22, cx, UI_H - 70, C_TEXT3, AL_CENTER, T("Left (from the first button): menu"));
        }
        side_draw(&v->side);
        return;
    }
    bool cf = !v->side.open;
    float rows_y = 140;
    if (v->view != V_GRID && v->n_rows && v->row < v->n_rows && v->col < v->rows[v->row].n) {
        SItem *h = &v->rows[v->row].it[v->col];
        char want[300] = "";
        if (h->backdrop) snprintf(want, sizeof want, TMDB_IMG "w1280%s", h->backdrop);
        if (strcmp(want, v->bg_url)) {
            snprintf(v->prev_url, sizeof v->prev_url, "%s", v->bg_url);
            snprintf(v->bg_url, sizeof v->bg_url, "%s", want);
            v->bg_fade = 0;
        }
        /* the neighbors' backdrops are fetched in advance */
        SRow *cr = &v->rows[v->row];
        for (int d = -1; d <= 2; d++) {
            int c = v->col + d;
            if (d && c >= 0 && c < cr->n && cr->it[c].backdrop) {
                char u[300];
                snprintf(u, sizeof u, TMDB_IMG "w1280%s", cr->it[c].backdrop);
                img_get(u);
            }
        }
        Image *ni = v->bg_url[0] ? img_get(v->bg_url) : NULL, *pi = v->prev_url[0] ? img_get(v->prev_url) : NULL;
        bool ready = ni && img_state(ni) == IMG_READY;
        if (!ready) v->bg_fade = 0;
        float fa = v->bg_fade;
        if (!v->bgl) { v->bgl = layer_new(UI_W, UI_H); layer_set_opaque(v->bgl, true); }
        char key[320];
        snprintf(key, sizeof key, "%s|%d", v->bg_url, ready);
        bool settled = fa > 0.995f && v->bgl_fade > 0.995f;
        if ((!layer_valid(v->bgl) || strcmp(key, v->bgl_key) || !settled) && layer_begin(v->bgl)) {
            draw_background();
            if (pi && fa < 1) img_draw_cover(pi, 0, 0, UI_W, SBG_H, 0, 1 - fa);
            if (ni) img_draw_cover(ni, 0, 0, UI_W, SBG_H, 0, fa);
            fill_hgrad(0, 0, UI_W * 0.75f, SBG_H, RGBA(8, 9, 13, 235), RGBA(8, 9, 13, 0));
            fill_vgrad(0, SBG_H - 330, UI_W, 330, RGBA(12, 14, 20, 0), RGBA(12, 14, 20, 255));
            layer_end(v->bgl);
            snprintf(v->bgl_key, sizeof v->bgl_key, "%s", key);
            v->bgl_fade = fa;
        }
        layer_draw(v->bgl, 0, 0, 1);
        draw_topbar(NULL);
        float hx = x0, hy = 120;
        text_draw_clip(F_BOLD, 56, hx, hy, UI_W - hx - 300, C_TEXT, AL_LEFT, h->title ? h->title : "");
        char meta[200];
        const char *st = status_text(h->status);
        snprintf(meta, sizeof meta, "%s%s%s%s%s", h->year ? h->year : "", h->year ? "  ·  " : "",
                 h->type && !strcmp(h->type, "tv") ? T("Show") : T("Movie"), st ? "  ·  " : "", st ? st : "");
        text_draw(F_SEMI, 27, hx, hy + 80, C_TEXT2, AL_LEFT, meta);
        text_draw_wrap(F_REG, 27, hx, hy + 128, 1050, 3, 37, color_alpha(C_TEXT, 0.9f), h->overview ? h->overview : "");
        rows_y = 140 + SHDR;
    }
    clip_push(SIDE_W, rows_y - 20, UI_W, UI_H - rows_y + 20);
    if (v->view == V_GRID && v->n_rows) {
        SRow *r = &v->rows[0];
        float top = 70 - v->sy;
        text_draw(F_BOLD, 44, x0, top, C_TEXT, AL_LEFT, v->grid_title);
        if ((r->loading || r->pending) && r->n == 0) draw_spinner(cx, UI_H / 2, 30, C_TEXT);
        else if (r->n == 0) text_draw(F_REG, 26, x0, top + 120, C_TEXT3, AL_LEFT, T("Nothing found"));
        for (int i = 0; i < r->n; i++) {
            int gr = i / SCOLS, gc = i % SCOLS;
            float x = x0 + gc * 280, y = top + 90 + gr * 480;
            if (y > UI_H || y + 480 < 0) continue;
            draw_poster(&r->it[i], x, y, 250, 375, cf && v->row == gr && v->col == gc);
        }
    } else {
        float y = rows_y - v->sy;
        for (int r = 0; r < v->n_rows; r++) {
            SRow *row = &v->rows[r];
            text_draw(F_SEMI, 30, x0, y, r == v->row && cf ? C_TEXT : C_TEXT2, AL_LEFT, row->title);
            float iy = y + 56;
            if ((row->loading || row->pending) && row->n == 0) draw_spinner(x0 + 100, iy + 150, 24, C_TEXT2);
            else if (row->n == 0) text_draw(F_REG, 24, x0, iy + 120, C_TEXT3, AL_LEFT, T("Nothing found"));
            for (int i = 0; i < row->n; i++) {
                float x = x0 + i * 280 - v->sx[r];
                if (x > UI_W || x + 250 < SIDE_W) continue;
                draw_poster(&row->it[i], x, iy, 250, 375, cf && r == v->row && v->col == i);
            }
            y += 530;
        }
    }
    clip_pop();
    side_draw(&v->side);
}

static bool seerr_input(Screen *s, const InputEvent *ev) {
    Seerr *v = s->st;
    int act;
    if (side_input(&v->side, ev, &act)) {
        if (act >= 0) side_activate(v, act);
        return true;
    }
    if (v->state != 2) {
        if (v->state == 1) return ev->a != A_BACK && ev->a != A_HOME;
        bool retry = v->state == 3 && *seerr_url();
        int nb = retry ? 2 : 1;
        if (buttons_nav(&v->btn, nb, ev)) return true;
        if (ev->a == A_OK) {
            if (ev->repeat) return true;
            if (retry && v->btn == 0) login(v);
            else keyboard_open(T("Seerr address (e.g. 192.168.1.10:5055)"), seerr_url(), false, url_done, v);
            return true;
        }
        if (ev->a == A_LEFT || ev->a == A_MENU) { side_open(&v->side); return true; }
        return false;
    }
    SRow *gr = v->view == V_GRID && v->n_rows ? &v->rows[0] : NULL;
    switch (ev->a) {
    case A_UP:
        if (v->row > 0) { v->row--; if (!gr) v->col = 0; } /* new row: start from the first title */
        return true;
    case A_DOWN:
        if (gr) {
            if ((v->row + 1) * SCOLS < gr->n) v->row++;
            if (v->row * SCOLS + v->col >= gr->n) v->col = (gr->n - 1) % SCOLS;
        } else if (v->row < v->n_rows - 1) {
            v->row++;
            v->col = 0;
        }
        return true;
    case A_LEFT:
        if (v->col > 0) v->col--;
        else side_open(&v->side);
        return true;
    case A_RIGHT:
        if (gr) { if (v->col < SCOLS - 1 && v->row * SCOLS + v->col + 1 < gr->n) v->col++; }
        else if (v->col < v->rows[v->row].n - 1) v->col++;
        return true;
    case A_OK: {
        SItem *it = NULL;
        if (gr) { int i = v->row * SCOLS + v->col; if (i < gr->n) it = &gr->it[i]; }
        else if (v->row < v->n_rows && v->col < v->rows[v->row].n) it = &v->rows[v->row].it[v->col];
        if (it) app_push(seerr_detail(it));
        return true;
    }
    case A_CHAR:
        keyboard_open(T("Search Seerr"), (char[]){(char)ev->ch, 0}, false, search_done, v);
        return true;
    case A_SEARCH: keyboard_open(T("Search Seerr"), "", false, search_done, v); return true;
    case A_MENU: side_open(&v->side); return true;
    case A_BACK:
        if (v->view == V_GRID) { v->side.active = 0; show_discover(v); return true; }
        if (v->row > 0 || v->col > 0) { v->row = v->col = 0; return true; }
        return false;
    default: return false;
    }
}

Screen *seerr_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "seerr";
    s->st = calloc(1, sizeof(Seerr));
    s->enter = seerr_enter;
    s->update = seerr_update;
    s->draw = seerr_draw;
    s->input = seerr_input;
    s->destroy = seerr_destroy;
    return s;
}

/* ---------- detaljeside + anmodning ---------- */
typedef struct {
    SItem it;
    int seasons[64], n_seasons;
    int sstat[64], eps[64]; /* per season: status (like mediaInfo, 2+ = requested/exists) and number of episodes */
    bool picker, pick[64];  /* season picker when requesting */
    int psel;
    float pscroll;
    bool requesting, details_loaded;
    int token;
    char genres[200], runtime[40];
    char origin[240]; /* "Original language: … · Country: …" */
    int rt_critic, rt_audience, imdb10; /* -1 = unknown; imdb10 = IMDb * 10 */
    char trailer[32]; /* YouTube key */
    int btn;          /* 0 = anmod, 1 = trailer */
    /* "More like this": 0 same collection, 1 recommendations, 2 similar - merged in that order */
    struct { SItem *it; int n; bool done; } bk[3];
    SItem *sim[24];
    int n_sim, scol, focus; /* focus: 0 buttons, 1 the row */
    float scroll, ssx;
} SDetail;
static SDetail *live_detail;

/* Danish names for original languages (ISO 639-1) and countries (ISO 3166-1); unknown ones are shown as Seerr provides them */
static const char *lang_da(const char *c) {
    static const char *m[][2] = {{"en", N_("English")}, {"da", N_("Danish")}, {"sv", N_("Swedish")}, {"no", N_("Norwegian")}, {"nb", N_("Norwegian")}, {"de", N_("German")},
        {"fr", N_("French")}, {"es", N_("Spanish")}, {"it", N_("Italian")}, {"pt", N_("Portuguese")}, {"nl", N_("Dutch")}, {"fi", N_("Finnish")},
        {"is", N_("Icelandic")}, {"pl", N_("Polish")}, {"ru", N_("Russian")}, {"uk", N_("Ukrainian")}, {"tr", N_("Turkish")}, {"ar", N_("Arabic")},
        {"fa", N_("Persian")}, {"he", N_("Hebrew")}, {"hi", N_("Hindi")}, {"ta", N_("Tamil")}, {"te", N_("Telugu")}, {"ja", N_("Japanese")},
        {"ko", N_("Korean")}, {"zh", N_("Chinese")}, {"cn", N_("Cantonese")}, {"th", N_("Thai")}, {"id", N_("Indonesian")}, {"cs", N_("Czech")},
        {"hu", N_("Hungarian")}, {"ro", N_("Romanian")}, {"el", N_("Greek")}, {"sr", N_("Serbian")}, {"hr", N_("Croatian")}, {"tl", N_("Tagalog")}};
    for (size_t i = 0; c && i < sizeof m / sizeof m[0]; i++) if (!strcmp(c, m[i][0])) return T(m[i][1]);
    return NULL;
}
static const char *country_da(const char *c) {
    static const char *m[][2] = {{"US", N_("United States")}, {"GB", N_("United Kingdom")}, {"DK", N_("Denmark")}, {"SE", N_("Sweden")}, {"NO", N_("Norway")},
        {"DE", N_("Germany")}, {"FR", N_("France")}, {"ES", N_("Spain")}, {"IT", N_("Italy")}, {"NL", N_("Netherlands")}, {"BE", N_("Belgium")},
        {"FI", N_("Finland")}, {"IS", N_("Iceland")}, {"IE", N_("Ireland")}, {"PL", N_("Poland")}, {"RU", N_("Russia")}, {"UA", N_("Ukraine")},
        {"TR", N_("Turkey")}, {"CA", N_("Canada")}, {"AU", N_("Australia")}, {"NZ", N_("New Zealand")}, {"MX", N_("Mexico")}, {"BR", N_("Brazil")},
        {"AR", N_("Argentina")}, {"IN", N_("India")}, {"JP", N_("Japan")}, {"KR", N_("South Korea")}, {"CN", N_("China")}, {"HK", N_("Hong Kong")},
        {"TW", N_("Taiwan")}, {"TH", N_("Thailand")}, {"ZA", N_("South Africa")}, {"IR", N_("Iran")}, {"IL", N_("Israel")}, {"EG", N_("Egypt")},
        {"AT", N_("Austria")}, {"CH", N_("Switzerland")}, {"CZ", N_("Czechia")}, {"HU", N_("Hungary")}, {"PT", N_("Portugal")}, {"GR", N_("Greece")}};
    for (size_t i = 0; c && i < sizeof m / sizeof m[0]; i++) if (!strcmp(c, m[i][0])) return T(m[i][1]);
    return NULL;
}

static void parse_origin(SDetail *d, cJSON *json) {
    d->origin[0] = 0;
    char *ol = jdup(json, "originalLanguage"), lang[64] = "";
    if (ol) {
        const char *da = lang_da(ol);
        cJSON *sl;
        if (!da) /* unknown code: use Seerr's English language name */
            cJSON_ArrayForEach(sl, cJSON_GetObjectItem(json, "spokenLanguages")) {
                cJSON *iso = cJSON_GetObjectItem(sl, "iso_639_1"), *en = cJSON_GetObjectItem(sl, "english_name");
                if (cJSON_IsString(iso) && !strcmp(iso->valuestring, ol) && cJSON_IsString(en)) { da = en->valuestring; break; }
            }
        snprintf(lang, sizeof lang, "%s", da ? da : ol);
        free(ol);
    }
    char lands[160] = "";
    cJSON *c, *oc = cJSON_GetObjectItem(json, "originCountry"); /* serier: ["KR"] */
    int n = 0;
    cJSON_ArrayForEach(c, oc) {
        if (!cJSON_IsString(c) || n >= 3) continue;
        const char *da = country_da(c->valuestring);
        snprintf(lands + strlen(lands), sizeof lands - strlen(lands), "%s%s", n++ ? ", " : "", da ? da : c->valuestring);
    }
    if (!n) cJSON_ArrayForEach(c, cJSON_GetObjectItem(json, "productionCountries")) { /* film */
        cJSON *iso = cJSON_GetObjectItem(c, "iso_3166_1"), *nm = cJSON_GetObjectItem(c, "name");
        if (n >= 3) break;
        const char *da = cJSON_IsString(iso) ? country_da(iso->valuestring) : NULL;
        if (!da && cJSON_IsString(nm)) da = nm->valuestring;
        if (da) snprintf(lands + strlen(lands), sizeof lands - strlen(lands), "%s%s", n++ ? ", " : "", da);
    }
    if (lang[0] && lands[0]) snprintf(d->origin, sizeof d->origin, T("Original language: %s   ·   %s: %s"), lang, n > 1 ? T("Countries") : T("Country"), lands);
    else if (lang[0]) snprintf(d->origin, sizeof d->origin, T("Original language: %s"), lang);
    else if (lands[0]) snprintf(d->origin, sizeof d->origin, "%s: %s", n > 1 ? T("Countries") : T("Country"), lands);
}
static SDetail *open_details[16]; /* all open detail pages (you can open a new one from "More like this") */
static bool detail_alive(SDetail *d) {
    for (int i = 0; i < 16; i++) if (open_details[i] == d) return true;
    return false;
}

static void sim_rebuild(SDetail *d) {
    d->n_sim = 0;
    for (int b = 0; b < 3; b++)
        for (int i = 0; i < d->bk[b].n && d->n_sim < 24; i++) {
            SItem *it = &d->bk[b].it[i];
            bool dup = it->id == d->it.id;
            for (int k = 0; k < d->n_sim && !dup; k++) dup = d->sim[k]->id == it->id;
            if (!dup) d->sim[d->n_sim++] = it;
        }
    if (d->scol >= d->n_sim) d->scol = d->n_sim ? d->n_sim - 1 : 0;
}

typedef struct { SDetail *d; int b; const char *type; } SimCtx;
static void sim_done(void *ud, cJSON *json, long status) {
    SimCtx *c = ud;
    SDetail *d = c->d;
    if (detail_alive(d)) {
        d->bk[c->b].done = true;
        cJSON *arr = cJSON_GetObjectItem(json, c->b == 0 ? "parts" : "results"), *o;
        if (status == 200 && arr) {
            int n = cJSON_GetArraySize(arr), k = 0;
            d->bk[c->b].it = calloc(n + 1, sizeof(SItem));
            cJSON_ArrayForEach(o, arr) {
                SItem *it = &d->bk[c->b].it[k];
                sitem_parse(it, o);
                if (!it->id || !it->title) { sitem_free(it); continue; }
                if (!it->type) it->type = str_dup(c->type);
                k++;
            }
            d->bk[c->b].n = k;
            sim_rebuild(d);
        }
    }
    free(c);
}
static void sim_fetch(SDetail *d, int b, const char *path, const char *type) {
    SimCtx *c = calloc(1, sizeof *c);
    c->d = d; c->b = b; c->type = type;
    seerr_call("GET", path, NULL, sim_done, c);
}

static void details_done(void *ud, cJSON *json, long status) {
    SDetail *d = ud;
    if (d != live_detail || status != 200 || !json) return;
    d->details_loaded = true;
    cJSON *mi = cJSON_GetObjectItem(json, "mediaInfo"), *st = cJSON_GetObjectItem(mi, "status");
    if (cJSON_IsNumber(st)) d->it.status = st->valueint;
    char *ov = jdup(json, "overview");
    if (ov) { free(d->it.overview); d->it.overview = ov; }
    char *bd = jdup(json, "backdropPath");
    if (bd) { free(d->it.backdrop); d->it.backdrop = bd; }
    cJSON *g;
    d->genres[0] = 0;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(json, "genres")) {
        cJSON *n = cJSON_GetObjectItem(g, "name");
        if (!cJSON_IsString(n)) continue;
        if (d->genres[0]) strncat(d->genres, ", ", sizeof d->genres - strlen(d->genres) - 1);
        strncat(d->genres, n->valuestring, sizeof d->genres - strlen(d->genres) - 1);
    }
    cJSON *rt = cJSON_GetObjectItem(json, "runtime");
    if (cJSON_IsNumber(rt) && rt->valueint > 0) snprintf(d->runtime, sizeof d->runtime, "%d min", rt->valueint);
    parse_origin(d, json);
    cJSON *vid;
    for (int pass = 0; pass < 2 && !d->trailer[0]; pass++) /* official trailer first, otherwise a teaser */
        cJSON_ArrayForEach(vid, cJSON_GetObjectItem(json, "relatedVideos")) {
            cJSON *site = cJSON_GetObjectItem(vid, "site"), *key = cJSON_GetObjectItem(vid, "key"), *type = cJSON_GetObjectItem(vid, "type");
            if (!cJSON_IsString(site) || strcmp(site->valuestring, "YouTube") || !cJSON_IsString(key)) continue;
            if (pass == 0 && (!cJSON_IsString(type) || strcmp(type->valuestring, "Trailer"))) continue;
            snprintf(d->trailer, sizeof d->trailer, "%s", key->valuestring);
            break;
        }
    d->n_seasons = 0;
    cJSON *se;
    cJSON_ArrayForEach(se, cJSON_GetObjectItem(json, "seasons")) {
        cJSON *num = cJSON_GetObjectItem(se, "seasonNumber");
        if (cJSON_IsNumber(num) && num->valueint > 0 && d->n_seasons < 64) {
            cJSON *ec = cJSON_GetObjectItem(se, "episodeCount");
            d->eps[d->n_seasons] = cJSON_IsNumber(ec) ? ec->valueint : 0;
            d->sstat[d->n_seasons] = 0;
            d->seasons[d->n_seasons++] = num->valueint;
        }
    }
    /* what exists / has already been requested */
    cJSON_ArrayForEach(se, cJSON_GetObjectItem(mi, "seasons")) {
        cJSON *num = cJSON_GetObjectItem(se, "seasonNumber"), *ss = cJSON_GetObjectItem(se, "status");
        for (int i = 0; i < d->n_seasons; i++)
            if (cJSON_IsNumber(num) && cJSON_IsNumber(ss) && d->seasons[i] == num->valueint) d->sstat[i] = ss->valueint;
    }
    cJSON *rq;
    cJSON_ArrayForEach(rq, cJSON_GetObjectItem(mi, "requests")) {
        cJSON *rs = cJSON_GetObjectItem(rq, "status");
        if (cJSON_IsNumber(rs) && rs->valueint == 3) continue; /* afvist */
        cJSON_ArrayForEach(se, cJSON_GetObjectItem(rq, "seasons")) {
            cJSON *num = cJSON_GetObjectItem(se, "seasonNumber");
            for (int i = 0; i < d->n_seasons; i++)
                if (cJSON_IsNumber(num) && d->seasons[i] == num->valueint && d->sstat[i] < 2) d->sstat[i] = 2;
        }
    }
    cJSON *col = cJSON_GetObjectItem(json, "collection"), *cid = cJSON_GetObjectItem(col, "id");
    if (cJSON_IsNumber(cid)) { /* a movie in a collection: the other parts first (Fall 2 -> Fall 1) */
        char *p = str_printf("/api/v1/collection/%d?language=%s", cid->valueint, i18n_lang());
        sim_fetch(d, 0, p, "movie");
        free(p);
    } else d->bk[0].done = true;
    if (cJSON_IsNumber(cJSON_GetObjectItem(json, "numberOfSeasons")) && d->n_seasons > 0)
        snprintf(d->runtime, sizeof d->runtime, T("%d seasons"), d->n_seasons);
}

static void ratings_done(void *ud, cJSON *json, long status) {
    SDetail *d = ud;
    if (d != live_detail || status != 200 || !json) return;
    cJSON *rt = cJSON_GetObjectItem(json, "rt"), *imdb = cJSON_GetObjectItem(json, "imdb");
    if (!rt && cJSON_GetObjectItem(json, "criticsScore")) rt = json; /* series: only RT directly */
    cJSON *c = cJSON_GetObjectItem(rt, "criticsScore"), *a = cJSON_GetObjectItem(rt, "audienceScore");
    if (cJSON_IsNumber(c)) d->rt_critic = c->valueint;
    if (cJSON_IsNumber(a)) d->rt_audience = a->valueint;
    cJSON *i = cJSON_GetObjectItem(imdb, "criticsScore");
    if (cJSON_IsNumber(i)) d->imdb10 = (int)(i->valuedouble * 10 + 0.5);
}

static void request_done(void *ud, cJSON *json, long status) {
    SDetail *d = ud;
    if (d != live_detail) return;
    d->requesting = false;
    if (status == 200 || status == 201) {
        bool rest = false;
        for (int i = 0; i < d->n_seasons; i++) { if (d->pick[i]) d->sstat[i] = 2; if (d->sstat[i] < 2) rest = true; }
        d->it.status = rest ? 4 : 2; /* some seasons left: can still request the rest */
        app_toast(T("Request sent: %s"), d->it.title ? d->it.title : "");
    } else {
        char *m = json ? jdup(json, "message") : NULL;
        app_toast(T("Request failed%s%s"), m ? ": " : "", m ? m : "");
        free(m);
    }
}

static void detail_enter(Screen *s) {
    SDetail *d = s->st;
    live_detail = d;
    bool tv = d->it.type && !strcmp(d->it.type, "tv");
    char *p = str_printf("/api/v1/%s/%d?language=%s", tv ? "tv" : "movie", d->it.id, i18n_lang());
    seerr_call("GET", p, NULL, details_done, d);
    free(p);
    d->rt_critic = d->rt_audience = d->imdb10 = -1;
    p = str_printf("/api/v1/%s/%d/%s", tv ? "tv" : "movie", d->it.id, tv ? "ratings" : "ratingscombined");
    seerr_call("GET", p, NULL, ratings_done, d);
    free(p);
    const char *t = tv ? "tv" : "movie";
    p = str_printf("/api/v1/%s/%d/recommendations?page=1&language=%s", t, d->it.id, i18n_lang());
    sim_fetch(d, 1, p, t);
    free(p);
    p = str_printf("/api/v1/%s/%d/similar?page=1&language=%s", t, d->it.id, i18n_lang());
    sim_fetch(d, 2, p, t);
    free(p);
}

static void detail_resume(Screen *s) { live_detail = s->st; }

static void detail_update(Screen *s, float dt) {
    SDetail *d = s->st;
    d->scroll = anim_to(d->scroll, d->focus ? 470 : 0, dt, 12);
    float t = d->scol * 230 - 560;
    d->ssx = anim_to(d->ssx, t > 0 ? t : 0, dt, 14);
    float pt = (d->psel - 7) * 66.0f;
    d->pscroll = anim_to(d->pscroll, pt > 0 ? pt : 0, dt, 14);
}

static void send_request(SDetail *d, bool tv) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mediaType", tv ? "tv" : "movie");
    cJSON_AddNumberToObject(o, "mediaId", d->it.id);
    if (tv) {
        cJSON *arr = cJSON_AddArrayToObject(o, "seasons");
        for (int i = 0; i < d->n_seasons; i++)
            if (d->pick[i]) cJSON_AddItemToArray(arr, cJSON_CreateNumber(d->seasons[i]));
    }
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    d->requesting = true;
    seerr_call("POST", "/api/v1/request", body, request_done, d);
    free(body);
}

static bool picker_input(SDetail *d, const InputEvent *ev) {
    int rows = d->n_seasons + 2;
    switch (ev->a) {
    case A_UP: if (d->psel > 0) d->psel--; break;
    case A_DOWN: if (d->psel < rows - 1) d->psel++; break;
    case A_BACK: d->picker = false; break;
    case A_OK:
        if (ev->repeat) break;
        if (d->psel == 0) {
            bool all = true;
            for (int i = 0; i < d->n_seasons; i++) if (d->sstat[i] < 2 && !d->pick[i]) all = false;
            for (int i = 0; i < d->n_seasons; i++) d->pick[i] = d->sstat[i] < 2 && !all;
        } else if (d->psel < rows - 1) {
            int i = d->psel - 1;
            if (d->sstat[i] < 2) d->pick[i] = !d->pick[i];
        } else {
            int n = 0;
            for (int i = 0; i < d->n_seasons; i++) n += d->pick[i];
            if (!n) { app_toast(T("Select at least one season")); break; }
            d->picker = false;
            send_request(d, true);
        }
        break;
    default: break;
    }
    return true;
}

static void detail_destroy(Screen *s) {
    SDetail *d = s->st;
    if (live_detail == d) live_detail = NULL;
    for (int i = 0; i < 16; i++) if (open_details[i] == d) open_details[i] = NULL;
    sitem_free(&d->it);
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < d->bk[b].n; i++) sitem_free(&d->bk[b].it[i]);
        free(d->bk[b].it);
    }
    free(d);
    free(s);
}

static void detail_draw(Screen *s) {
    SDetail *d = s->st;
    SItem *it = &d->it;
    draw_background();
    char url[300] = "";
    if (it->backdrop) snprintf(url, sizeof url, TMDB_IMG "w1280%s", it->backdrop);
    if (img_draw_cover(img_get(url[0] ? url : NULL), 0, 0, UI_W, UI_H, 0, 0.5f)) {
        fill_hgrad(0, 0, UI_W * 0.8f, UI_H, RGBA(8, 9, 13, 250), RGBA(8, 9, 13, 0));
        fill_vgrad(0, UI_H * 0.5f, UI_W, UI_H * 0.5f, RGBA(8, 9, 13, 0), RGBA(8, 9, 13, 240));
    }
    float sc = d->scroll;
    char purl[300] = "";
    if (it->poster) snprintf(purl, sizeof purl, TMDB_IMG "w500%s", it->poster);
    fill_rrect(120, 200 - sc, 400, 600, 18, C_SURFACE);
    img_draw_cover(img_get(purl[0] ? purl : NULL), 120, 200 - sc, 400, 600, 18, 1);
    float x = 580, y = 200 - sc;
    text_draw_clip(F_BOLD, 64, x, y, UI_W - x - 120, C_TEXT, AL_LEFT, it->title);
    char meta[300];
    snprintf(meta, sizeof meta, "%s   %s   %s   %s", it->year ? it->year : "", it->type && !strcmp(it->type, "tv") ? T("Show") : T("Movie"),
             d->runtime, d->genres);
    text_draw_clip(F_REG, 26, x, y + 96, UI_W - x - 120, C_TEXT2, AL_LEFT, meta);
    float dy = 0;
    if (d->origin[0]) { text_draw_clip(F_REG, 24, x, y + 136, UI_W - x - 120, C_TEXT3, AL_LEFT, d->origin); dy = 40; }
    float ry = y + 150 + dy, rx = x;
    rx = draw_ratings(rx, ry, d->rt_critic, d->rt_audience, d->imdb10);
    float oy = (rx > x ? y + 210 : y + 160) + dy;
    text_draw_wrap(F_REG, 28, x, oy, UI_W - x - 160, dy > 0 ? 6 : 7, 40, C_TEXT, it->overview ? it->overview : ""); /* room for the buttons */
    const char *st = status_text(it->status);
    bool can_request = it->status < 2 || (it->status == 4 && it->type && !strcmp(it->type, "tv"));
    char lbl[64];
    if (d->requesting) snprintf(lbl, sizeof lbl, "%s", T("Sending…"));
    else if (can_request) snprintf(lbl, sizeof lbl, it->status == 4 ? T("Request the rest") : T("Request"));
    else snprintf(lbl, sizeof lbl, "%s", st ? st : T("Requested"));
    float w = text_width(F_SEMI, 28, lbl) + 110;
    bool f0 = d->btn == 0 && !d->focus;
    Color bg = !can_request ? (it->status >= 4 ? C_OK : RGB(99, 102, 241)) : (f0 ? C_FOCUS : RGBA(255, 255, 255, 40));
    fill_rrect(x, 720 - sc, w, 76, 38, bg);
    if (f0 && !can_request) stroke_rrect(x - 4, 716 - sc, w + 8, 84, 42, 4, C_FOCUS);
    Color fg = can_request && f0 ? RGB(10, 10, 10) : C_TEXT;
    draw_icon(can_request ? ICON_STAR : ICON_CHECK, x + 42, 758 - sc, 26, fg);
    text_draw(F_SEMI, 28, x + 70, 740 - sc, fg, AL_LEFT, lbl);
    if (d->trailer[0]) {
        bool f1 = d->btn == 1 && !d->focus;
        float tx = x + w + 24, tw = text_width(F_SEMI, 28, T("Trailer")) + 110;
        fill_rrect(tx, 720 - sc, tw, 76, 38, f1 ? C_FOCUS : RGBA(255, 255, 255, 40));
        draw_icon(ICON_FILM, tx + 42, 758 - sc, 26, f1 ? RGB(10, 10, 10) : C_TEXT);
        text_draw(F_SEMI, 28, tx + 70, 740 - sc, f1 ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, T("Trailer"));
    }
    if (it->status == 5) text_draw(F_REG, 24, x, 820 - sc, C_TEXT2, AL_LEFT, T("Already on Plex - open it in the Plex app."));
    /* More like this */
    bool loading = !d->bk[0].done || !d->bk[1].done || !d->bk[2].done;
    if (d->n_sim || loading) {
        float ry0 = 900 - sc;
        text_draw(F_SEMI, 30, 120, ry0, d->focus ? C_TEXT : C_TEXT2, AL_LEFT, T("More like this"));
        if (!d->n_sim) draw_spinner(220, ry0 + 200, 24, C_TEXT2);
        clip_push(0, ry0 + 30, UI_W, 460);
        for (int i = 0; i < d->n_sim; i++) {
            float px = 120 + i * 230 - d->ssx;
            if (px > UI_W || px + 200 < 0) continue;
            draw_poster(d->sim[i], px, ry0 + 60, 200, 300, d->focus && d->scol == i);
        }
        clip_pop();
    }
    if (d->picker) { /* season picker */
        fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, 170));
        int rows = d->n_seasons + 2;
        float rh = 66, pw = 760, vis = rows < 10 ? rows : 10, ph = 130 + vis * rh;
        float px = (UI_W - pw) / 2, py = (UI_H - ph) / 2;
        draw_shadow(px, py, pw, ph, 24, 30, 1);
        fill_rrect(px, py, pw, ph, 24, C_SURFACE2);
        text_draw(F_BOLD, 34, px + 40, py + 30, C_TEXT, AL_LEFT, T("Choose seasons"));
        clip_push(px, py + 100, pw, vis * rh);
        for (int r = 0; r < rows; r++) {
            float y = py + 100 + r * rh - d->pscroll;
            if (y < py + 100 - rh || y > py + ph) continue;
            bool f = d->psel == r;
            if (f) fill_rrect(px + 16, y + 4, pw - 32, rh - 8, 14, r == rows - 1 ? C_FOCUS : RGBA(255, 255, 255, 30));
            if (r == rows - 1) {
                int n = 0;
                for (int i = 0; i < d->n_seasons; i++) n += d->pick[i];
                char t[64];
                if (!n) snprintf(t, sizeof t, "%s", T("Select at least one season"));
                else snprintf(t, sizeof t, n == 1 ? T("Request %d season") : T("Request %d seasons"), n);
                text_draw(F_SEMI, 28, px + pw / 2, y + 16, f ? RGB(10, 10, 10) : C_TEXT, AL_CENTER, t);
                continue;
            }
            int i = r - 1;
            bool can = r == 0 || d->sstat[i] < 2;
            bool on = false;
            if (r == 0) { on = true; bool any = false;
                for (int k = 0; k < d->n_seasons; k++) if (d->sstat[k] < 2) { any = true; if (!d->pick[k]) on = false; }
                on = on && any; }
            else on = d->pick[i] || d->sstat[i] >= 2;
            float bx = px + 44, by = y + rh / 2 - 16;
            Color bc = can ? C_TEXT : C_TEXT3;
            if (on) { fill_rrect(bx, by, 32, 32, 8, can ? C_FOCUS : (d->sstat[i] >= 4 ? C_OK : RGB(99, 102, 241)));
                      draw_icon(ICON_CHECK, bx + 16, by + 16, 20, can ? RGB(10, 10, 10) : C_TEXT); }
            else stroke_rrect(bx, by, 32, 32, 8, 3, bc);
            char lbl[48];
            if (r == 0) snprintf(lbl, sizeof lbl, T("All seasons"));
            else snprintf(lbl, sizeof lbl, T("Season %d"), d->seasons[i]);
            text_draw(F_SEMI, 28, bx + 56, y + 16, can ? C_TEXT : C_TEXT3, AL_LEFT, lbl);
            char info[48] = "";
            if (r > 0) {
                const char *st = d->sstat[i] >= 2 ? status_text(d->sstat[i]) : NULL;
                if (st) snprintf(info, sizeof info, "%s", st);
                else if (d->eps[i]) snprintf(info, sizeof info, T("%d episodes"), d->eps[i]);
            }
            text_draw(F_REG, 24, px + pw - 44, y + 20, C_TEXT3, AL_RIGHT, info);
        }
        clip_pop();
    }
}

static bool detail_input(Screen *s, const InputEvent *ev) {
    SDetail *d = s->st;
    if (d->picker) return picker_input(d, ev);
    if (d->focus) { /* the "More like this" row */
        if (ev->a == A_UP) d->focus = 0;
        else if (ev->a == A_LEFT) { if (d->scol > 0) d->scol--; }
        else if (ev->a == A_RIGHT) { if (d->scol < d->n_sim - 1) d->scol++; }
        else if (ev->a == A_OK && !ev->repeat && d->scol < d->n_sim) app_push(seerr_detail(d->sim[d->scol]));
        else if (ev->a != A_DOWN) return false;
        return true;
    }
    if (ev->a == A_DOWN) { if (d->n_sim) d->focus = 1; return true; }
    if (ev->a == A_LEFT) { d->btn = 0; return true; }
    if (ev->a == A_RIGHT) { if (d->trailer[0]) d->btn = 1; return true; }
    if (ev->a != A_OK && ev->a != A_PLAYPAUSE) return false;
    if (d->btn == 1 && d->trailer[0]) {
        char url[96];
        snprintf(url, sizeof url, "https://www.youtube.com/watch?v=%s", d->trailer);
        char *t = str_printf("%s - trailer", d->it.title ? d->it.title : "");
        static const char *extra[] = {"--ytdl-format=bv*[height<=1080][vcodec!^=av01]+ba/b", NULL};
        PlayRequest rq = {0};
        rq.url = url;
        rq.title = t;
        rq.subtitle = T("Trailer");
        rq.description = d->it.overview;
        char purl[300] = "";
        if (d->it.poster) snprintf(purl, sizeof purl, TMDB_IMG "w342%s", d->it.poster);
        rq.poster = purl[0] ? purl : NULL;
        rq.extra = extra;
        player_run(&rq);
        free(t);
        return true;
    }
    bool tv = d->it.type && !strcmp(d->it.type, "tv");
    bool can_request = d->it.status < 2 || (d->it.status == 4 && tv);
    if (!can_request || d->requesting) return true;
    if (tv && !d->details_loaded) { app_toast(T("Loading seasons…")); return true; }
    if (tv) {
        int n = 0;
        for (int i = 0; i < d->n_seasons; i++) { d->pick[i] = d->sstat[i] < 2; n += d->pick[i]; }
        if (!n) { app_toast(T("All seasons are already requested or available")); return true; }
        d->picker = true;
        d->psel = 0;
        d->pscroll = 0;
        return true;
    }
    send_request(d, false);
    return true;
}

static Screen *seerr_detail(SItem *src) {
    Screen *s = calloc(1, sizeof *s);
    SDetail *d = calloc(1, sizeof *d);
    d->it = *src;
    d->it.type = str_dup(src->type); d->it.title = str_dup(src->title); d->it.poster = str_dup(src->poster);
    d->it.backdrop = str_dup(src->backdrop); d->it.year = str_dup(src->year); d->it.overview = str_dup(src->overview);
    s->name = "seerr-detail";
    s->st = d;
    s->enter = detail_enter;
    s->update = detail_update;
    s->resume = detail_resume;
    for (int i = 0; i < 16; i++) if (!open_details[i]) { open_details[i] = d; break; }
    s->draw = detail_draw;
    s->input = detail_input;
    s->destroy = detail_destroy;
    return s;
}

/* sign out: the session is closed at Seerr, and the session cookie and the address are deleted here */
void seerr_forget(void) {
    char jar[512];
    jar_path(jar, sizeof jar);
    if (*seerr_url()) {
        char url[1024];
        snprintf(url, sizeof url, "%s/api/v1/auth/logout", seerr_url());
        static const char *const hdr[] = {"Accept: application/json", "Content-Type: application/json", NULL};
        HttpBuf b;
        http_request_cookies("POST", url, hdr, "{}", &b, 4, jar);
        http_free(&b);
    }
    wipe_file(jar);
    cfg_set("seerr_url", "");
    seerr_token++; /* answers on their way are ignored */
    wipe_session_traces();
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
static void act_seerr_logout(void) { app_ask_signout("Seerr", seerr_forget); }
static const Item seerr_items[] = {
    {N_("Sign out of Seerr"), IT_ACTION, .action = act_seerr_logout, .help = N_("Closes the session at Seerr and deletes the address and login from this box")},
};

static const SeerrIface seerr_iface = {seerr_ratings};
const NotaApp nota_app = {
    NOTA_API, "seerr", "Seerr", N_("Find and request movies and shows for Plex"), "1.0.0", 60, 0,
    ICON_STAR, {99, 102, 241, 255}, {55, 48, 163, 255}, {255, 255, 255, 255},
    .open = seerr_screen, .forget = seerr_forget,
    .settings = seerr_items, .n_settings = sizeof seerr_items / sizeof seerr_items[0],
    .iface = &seerr_iface,
};
