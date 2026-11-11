/* NotaTV app package "flimmer" - Flimmer: discover and request movies and series on a Flimmertv server
 *
 * Flimmertv (a friend's project) does the jobs of Seerr, Radarr and Sonarr in one. This app is a client for its
 * app API only (docs/app-api.md in Flimmertv): sign in with email and password, browse Discover, search, request
 * movies and series (choosing seasons), and follow and withdraw your own requests. The token works nowhere else
 * on the server. Written from the API description - none of Flimmertv's own code is used. */
#include "flimmer.h"
#include "vendor/cJSON.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACCENT RGB(251, 113, 133)

static const char *fl_url(void) { return cfg_get("flimmer_url", ""); }

/* ---------- the sign-in token: a file of its own (0600), never in the shared config ---------- */
static char tok[256];
static void tok_path(char *p, size_t n) { snprintf(p, n, "%s/flimmer.token", data_dir()); }
static void tok_load(void) {
    char p[512];
    tok_path(p, sizeof p);
    char *t = file_read_all(p, sizeof tok - 1);
    snprintf(tok, sizeof tok, "%s", t ? t : "");
    free(t);
    str_trim(tok);
}
static void tok_save(const char *t) {
    char p[512];
    tok_path(p, sizeof p);
    snprintf(tok, sizeof tok, "%s", t ? t : "");
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    fchmod(fd, 0600);
    if (write(fd, tok, strlen(tok)) < 0) SDL_Log("flimmer: could not save the sign-in");
    close(fd);
}
static void tok_wipe(void) {
    char p[512];
    tok_path(p, sizeof p);
    wipe_file(p);
    memset(tok, 0, sizeof tok);
}

/* ---------- items ---------- */
enum { ST_NONE, ST_PENDING, ST_PROCESSING, ST_PARTIAL, ST_AVAILABLE };
static int media_status(const char *s) {
    if (!s) return ST_NONE;
    if (!strcmp(s, "pending")) return ST_PENDING;
    if (!strcmp(s, "processing")) return ST_PROCESSING;
    if (!strcmp(s, "partial")) return ST_PARTIAL;
    if (!strcmp(s, "available")) return ST_AVAILABLE;
    return ST_NONE;
}
static const char *status_text(int st) {
    switch (st) {
    case ST_PENDING: return T("Requested");
    case ST_PROCESSING: return T("Processing");
    case ST_PARTIAL: return T("Partly available");
    case ST_AVAILABLE: return T("Available");
    default: return NULL;
    }
}

typedef struct {
    bool tv;
    int tmdb, year, status;
    char *title, *poster, *overview;
} FItem;

static void fitem_free(FItem *f) { free(f->title); free(f->poster); free(f->overview); }

static char *jdup(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? str_dup(v->valuestring) : NULL;
}
static int jint(cJSON *o, const char *k, int def) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}
/* only https images (TMDB through the server's URLs); anything else is not fetched */
static char *jimg(cJSON *o, const char *k) {
    char *u = jdup(o, k);
    if (u && strncmp(u, "https://", 8)) { free(u); u = NULL; }
    return u;
}

static bool fitem_parse(FItem *f, cJSON *o) {
    memset(f, 0, sizeof *f);
    char *mt = jdup(o, "mediaType");
    f->tv = mt && !strcmp(mt, "tv");
    bool ok = !mt || !strcmp(mt, "movie") || f->tv;
    free(mt);
    f->tmdb = jint(o, "tmdbId", 0);
    f->title = jdup(o, "title");
    if (!ok || f->tmdb <= 0 || !f->title) { fitem_free(f); return false; }
    f->year = jint(o, "year", 0);
    f->poster = jimg(o, "posterUrl");
    f->overview = jdup(o, "overview");
    char *st = jdup(o, "mediaStatus");
    f->status = media_status(st);
    free(st);
    return true;
}
static void fitem_copy(FItem *d, const FItem *s) {
    *d = *s;
    d->title = str_dup(s->title);
    d->poster = str_dup(s->poster);
    d->overview = str_dup(s->overview);
}

/* ---------- HTTP against Flimmertv (Bearer token) ---------- */
typedef struct {
    char *path, *body;
    const char *method;
    cJSON *json;
    long status;
    int gen;
    bool keep; /* deliver even after the screens changed (the caller checks itself) */
    void (*done)(void *ud, cJSON *json, long status);
    void *ud;
    char url[256], auth[300];
} FReq;
static int fl_gen; /* increased when the rows or the sign-in change: older answers are dropped */
static void fl_signed_out(void);

static void freq_work(void *arg) {
    FReq *r = arg;
    char *url = str_printf("%s%s", r->url, r->path);
    const char *hdr[4] = {"Accept: application/json", "Content-Type: application/json", r->auth[0] ? r->auth : NULL, NULL};
    HttpBuf b;
    if (http_request(r->method, url, hdr, r->body, &b, 20)) {
        r->status = b.status;
        if (b.data && b.len) r->json = cJSON_Parse(b.data);
    }
    http_free(&b);
    free(url);
}

static void freq_done(void *arg) {
    FReq *r = arg;
    bool current = r->keep || r->gen == fl_gen;
    /* the sign-in ended on the server (signed out elsewhere, new password, 90 days unused) */
    if (current && r->status == 401 && r->auth[0] && tok[0]) fl_signed_out();
    else if (current && r->done) r->done(r->ud, r->json, r->status);
    cJSON_Delete(r->json);
    free(r->path);
    free(r->body);
    free(r);
}

static void fl_call_ex(const char *method, const char *path, const char *body,
                       void (*done)(void *, cJSON *, long), void *ud, bool keep) {
    FReq *r = xcalloc(1, sizeof *r);
    r->method = method;
    r->path = str_dup(path);
    r->body = str_dup(body);
    r->done = done;
    r->ud = ud;
    r->keep = keep;
    r->gen = fl_gen;
    snprintf(r->url, sizeof r->url, "%s", fl_url());
    if (tok[0]) snprintf(r->auth, sizeof r->auth, "Authorization: Bearer %s", tok);
    job_submit(freq_work, freq_done, r);
}
static void fl_call(const char *method, const char *path, const char *body, void (*done)(void *, cJSON *, long), void *ud) {
    fl_call_ex(method, path, body, done, ud, false);
}

/* the server's own message ({"error": "..."}), written for people */
static void err_text(cJSON *json, long status, char *out, size_t n) {
    cJSON *e = cJSON_GetObjectItem(json, "error");
    if (cJSON_IsString(e) && e->valuestring[0]) snprintf(out, n, "%s", e->valuestring);
    else if (status == 0) snprintf(out, n, T("Cannot reach Flimmertv at %s"), fl_url());
    else if (status == 503) snprintf(out, n, "%s", T("Flimmertv is not set up for Discover yet (no TMDB key)"));
    else snprintf(out, n, T("Something went wrong (HTTP %ld)"), status);
}

/* ---------- main screen ---------- */
#define FCOLS 6
#define FROWS 12
typedef struct {
    char *title, *base; /* base: the path without &page= */
    FItem *it;
    int n, cap, page, pages;
    bool loading, foryou;
} FRow;

typedef struct {
    int id, status; /* RQ_* */
    FItem it;
    char seasons[96], date[32], reason[240];
} FRequest;
enum { RQ_PENDING, RQ_DECLINED, RQ_PROCESSING, RQ_PARTIAL, RQ_AVAILABLE, RQ_REMOVED };

enum { V_DISCOVER, V_GRID, V_REQUESTS };
enum { SM_DISCOVER, SM_SEARCH, SM_REQUESTS, SM_MOVIES, SM_SERIES, SM_DANISH, SM_STREAMING, SM_SETTINGS };
/* state: 0 setup, 1 busy, 2 ready, 3 failed (msg) */
typedef struct {
    int state;
    char msg[240], user[128];
    int left; /* requests left, -1 = no limit */
    FRow rows[FROWS];
    int n_rows, row, col, view;
    float sy, sx[FROWS];
    SideMenu side;
    int side_map[16];
    char grid_title[200];
    FRequest *rq;
    int n_rq, rsel;
    bool rq_loading;
    float rscroll;
    int btn; /* button in focus on the setup screen */
    bool from_detail; /* a detail page was opened: refresh My requests when it closes */
} Fl;
#define HDR_H 330 /* the info area above the rows (follows the title in focus) */

static Fl *live;
static Screen *fl_detail(const FItem *it);

static void row_push(FRow *r, FItem *it) {
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 24; r->it = xrealloc(r->it, sizeof(FItem) * r->cap); }
    r->it[r->n++] = *it;
}

static void row_fetch(FRow *r);
static void row_done(void *ud, cJSON *json, long status) {
    FRow *r = ud;
    r->loading = false;
    if (status != 200 || !json) return;
    cJSON *items = cJSON_IsArray(json) ? json : cJSON_GetObjectItem(json, "items"), *o; /* foryou: null while empty */
    cJSON_ArrayForEach(o, items) {
        FItem it;
        if (!fitem_parse(&it, o)) continue;
        bool dup = false;
        for (int i = 0; i < r->n && !dup; i++) dup = r->it[i].tmdb == it.tmdb && r->it[i].tv == it.tv;
        if (dup) { fitem_free(&it); continue; }
        row_push(r, &it);
    }
    r->pages = jint(json, "totalPages", r->page);
}
static void row_fetch(FRow *r) {
    r->loading = true;
    r->page++;
    char *p = r->foryou ? str_dup(r->base) : str_printf("%s%spage=%d", r->base, strchr(r->base, '?') ? "&" : "?", r->page);
    fl_call("GET", p, NULL, row_done, r);
    free(p);
}
/* the grid near its end: the next page (Discover lists have many) */
static void row_more(FRow *r) {
    if (!r->loading && !r->foryou && r->page < r->pages && r->page < 50) row_fetch(r);
}

static void free_rows(Fl *v) {
    for (int i = 0; i < v->n_rows; i++) {
        for (int k = 0; k < v->rows[i].n; k++) fitem_free(&v->rows[i].it[k]);
        free(v->rows[i].it);
        free(v->rows[i].title);
        free(v->rows[i].base);
    }
    v->n_rows = 0;
    fl_gen++; /* answers for the old rows are dropped */
}
static void free_requests(Fl *v) {
    for (int i = 0; i < v->n_rq; i++) fitem_free(&v->rq[i].it);
    free(v->rq);
    v->rq = NULL;
    v->n_rq = 0;
}

static void add_row(Fl *v, const char *title, const char *base, bool foryou) {
    if (v->n_rows >= FROWS) return;
    FRow *r = &v->rows[v->n_rows];
    memset(r, 0, sizeof *r);
    r->title = str_dup(title);
    r->base = str_dup(base);
    r->foryou = foryou;
    v->sx[v->n_rows] = 0;
    v->n_rows++;
    row_fetch(r);
}

/* the country from the setup guide (Settings → Date and time), for "popular on Netflix" */
static const char *region(void) {
    const char *c = cfg_get("tz_country", "");
    return *c ? c : "DK";
}

static void show_discover(Fl *v) {
    free_rows(v);
    v->view = V_DISCOVER;
    add_row(v, T("Recommended for you"), "/api/v1/discover/foryou", true);
    add_row(v, T("Trending movies"), "/api/v1/discover?list=trending", false);
    add_row(v, T("Trending series"), "/api/v1/discover?type=tv&list=trending", false);
    char p[96], t[160];
    snprintf(p, sizeof p, "/api/v1/discover?provider=8&region=%s", region());
    snprintf(t, sizeof t, T("Popular on Netflix (%s)"), tz_country_name(region()));
    add_row(v, t, p, false);
    add_row(v, T("Popular movies"), "/api/v1/discover?list=popular", false);
    add_row(v, T("Popular series"), "/api/v1/discover?type=tv&list=popular", false);
    add_row(v, T("Upcoming movies"), "/api/v1/discover?list=upcoming", false);
    add_row(v, T("Danish movies"), "/api/v1/discover?language=da", false);
    add_row(v, T("Top rated movies"), "/api/v1/discover?list=top_rated", false);
    v->row = v->col = 0;
    v->sy = 0;
}

static void show_grid(Fl *v, const char *title, const char *base) {
    free_rows(v);
    v->view = V_GRID;
    snprintf(v->grid_title, sizeof v->grid_title, "%s", title);
    add_row(v, title, base, false);
    v->row = v->col = 0;
    v->sy = 0;
}

/* ---------- my requests ---------- */
static int request_status(const char *s) {
    if (!s) return RQ_PENDING;
    if (!strcmp(s, "declined")) return RQ_DECLINED;
    if (!strcmp(s, "processing")) return RQ_PROCESSING;
    if (!strcmp(s, "partial")) return RQ_PARTIAL;
    if (!strcmp(s, "available")) return RQ_AVAILABLE;
    if (!strcmp(s, "removed")) return RQ_REMOVED;
    return RQ_PENDING;
}
static const char *request_text(int st) {
    switch (st) {
    case RQ_DECLINED: return T("Declined");
    case RQ_PROCESSING: return T("Approved - on its way");
    case RQ_PARTIAL: return T("Partly available");
    case RQ_AVAILABLE: return T("Available");
    case RQ_REMOVED: return T("Removed");
    default: return T("Waiting for approval");
    }
}
static Color request_color(int st) {
    switch (st) {
    case RQ_DECLINED: case RQ_REMOVED: return C_TEXT3;
    case RQ_AVAILABLE: case RQ_PARTIAL: return C_OK;
    case RQ_PROCESSING: return ACCENT;
    default: return C_TEXT2;
    }
}

static void requests_done(void *ud, cJSON *json, long status) {
    Fl *v = ud;
    if (v != live) return;
    v->rq_loading = false;
    if (status != 200 || !cJSON_IsArray(json)) {
        err_text(json, status, v->msg, sizeof v->msg);
        return;
    }
    v->msg[0] = 0;
    free_requests(v);
    int n = cJSON_GetArraySize(json), k = 0;
    v->rq = xcalloc(n + 1, sizeof *v->rq);
    cJSON *o;
    cJSON_ArrayForEach(o, json) {
        FRequest *r = &v->rq[k];
        if (!fitem_parse(&r->it, o)) continue;
        r->id = jint(o, "id", 0);
        char *st = jdup(o, "status");
        r->status = request_status(st);
        free(st);
        char *at = jdup(o, "requestedAt"); /* ISO time: the day is enough */
        if (at) snprintf(r->date, sizeof r->date, "%.10s", at);
        free(at);
        char *why = jdup(o, "reason");
        if (why) snprintf(r->reason, sizeof r->reason, "%s", why);
        free(why);
        cJSON *se = cJSON_GetObjectItem(o, "seasons"), *s;
        if (cJSON_IsArray(se) && cJSON_GetArraySize(se) > 0) {
            snprintf(r->seasons, sizeof r->seasons, "%s", cJSON_GetArraySize(se) == 1 ? T("Season") : T("Seasons"));
            bool first = true;
            cJSON_ArrayForEach(s, se) {
                if (!cJSON_IsNumber(s)) continue;
                size_t l = strlen(r->seasons);
                snprintf(r->seasons + l, sizeof r->seasons - l, "%s%d", first ? " " : ", ", s->valueint);
                first = false;
            }
        }
        k++;
    }
    v->n_rq = k;
    if (v->rsel >= k) v->rsel = k ? k - 1 : 0;
}
static void show_requests(Fl *v) {
    free_rows(v);
    v->view = V_REQUESTS;
    v->rq_loading = true;
    v->msg[0] = 0;
    fl_call("GET", "/api/v1/requests", NULL, requests_done, v);
}

/* OK on a request: its sheet - Open first; a request still waiting can be withdrawn */
static int rq_sheet_id;
static void withdraw_done(void *ud, cJSON *json, long status) {
    Fl *v = ud;
    if (v != live) return;
    if (status == 204 || status == 200) { app_toast("%s", T("Request withdrawn")); show_requests(v); }
    else { char m[240]; err_text(json, status, m, sizeof m); app_toast("%s", m); }
}
static void rq_sheet_done(void *ud, int idx) {
    Fl *v = ud;
    if (v != live || idx < 0 || v->view != V_REQUESTS) return;
    FRequest *r = NULL;
    for (int i = 0; i < v->n_rq; i++) if (v->rq[i].id == rq_sheet_id) r = &v->rq[i];
    if (!r) return;
    if (idx == 0) { v->from_detail = true; app_push(fl_detail(&r->it)); }
    else {
        char p[64];
        snprintf(p, sizeof p, "/api/v1/requests/%d", r->id);
        fl_call("DELETE", p, NULL, withdraw_done, v);
    }
}
static void open_rq_sheet(Fl *v, FRequest *r) {
    rq_sheet_id = r->id;
    const char *items[2] = {T("Open"), T("Withdraw request")};
    choice_open(r->it.title, items, r->status == RQ_PENDING ? 2 : 1, 0, rq_sheet_done, v);
}

/* ---------- side menu ---------- */
static void build_side(Fl *v) {
    SideMenu *m = &v->side;
    int keep = m->n ? v->side_map[m->active] : SM_DISCOVER;
    side_clear(m);
    if (v->left >= 0) snprintf(m->subtitle, sizeof m->subtitle, T("%s · %d requests left"), v->user, v->left);
    else snprintf(m->subtitle, sizeof m->subtitle, "%s", v->user);
#define ADD(lbl, ic, gap, id) do { v->side_map[side_add(m, lbl, ic, gap)] = id; } while (0)
    ADD(T("Discover"), ICON_STAR, false, SM_DISCOVER);
    ADD(T("Search"), ICON_SEARCH, false, SM_SEARCH);
    ADD(T("My requests"), ICON_CHECK, false, SM_REQUESTS);
    ADD(T("Popular movies"), ICON_FILM, true, SM_MOVIES);
    ADD(T("Popular series"), ICON_TV, false, SM_SERIES);
    ADD(T("Danish movies"), ICON_FILM, false, SM_DANISH);
    ADD(T("Popular on Netflix"), ICON_PLAY, false, SM_STREAMING);
    ADD(T("Server and sign-in"), ICON_LOCK, true, SM_SETTINGS);
#undef ADD
    m->active = 0;
    for (int i = 0; i < m->n; i++) if (v->side_map[i] == keep) m->active = i;
}

/* ---------- sign-in ---------- */
static void me_parse(Fl *v, cJSON *u) {
    char *n = jdup(u, "name");
    snprintf(v->user, sizeof v->user, "%s", n ? n : "");
    free(n);
    cJSON *l = cJSON_GetObjectItem(u, "requestsLeft");
    v->left = cJSON_IsNumber(l) ? l->valueint : -1;
}
static void ready(Fl *v) {
    v->state = 2;
    v->msg[0] = 0;
    build_side(v);
    show_discover(v);
}
static void me_done(void *ud, cJSON *json, long status) {
    Fl *v = ud;
    if (v != live) return;
    if (status == 200 && json) { me_parse(v, json); ready(v); return; }
    v->state = 3;
    err_text(json, status, v->msg, sizeof v->msg);
}

static char login_email[256];
static void login_done(void *ud, cJSON *json, long status) {
    Fl *v = ud;
    if (v != live) return;
    cJSON *t = cJSON_GetObjectItem(json, "token");
    if ((status == 200 || status == 201) && cJSON_IsString(t) && t->valuestring[0]) {
        tok_save(t->valuestring);
        me_parse(v, cJSON_GetObjectItem(json, "user"));
        ready(v);
        return;
    }
    v->state = 3;
    if (status == 401) snprintf(v->msg, sizeof v->msg, "%s", T("Wrong email or password. Accounts that only sign in with Plex or Jellyfin cannot sign in here."));
    else err_text(json, status, v->msg, sizeof v->msg);
}
static void pw_done(void *ud, const char *pw) {
    Fl *v = ud;
    if (!pw || !*pw || v != live) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "email", login_email);
    cJSON_AddStringToObject(o, "password", pw);
    cJSON_AddStringToObject(o, "device", "NotaTV");
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    memset(login_email, 0, sizeof login_email);
    v->state = 1;
    snprintf(v->msg, sizeof v->msg, "%s", T("Signing in…"));
    fl_gen++;
    fl_call("POST", "/api/v1/app/login", body, login_done, v);
    memset(body, 0, strlen(body)); /* the password leaves memory */
    free(body);
}
static void email_done(void *ud, const char *e) {
    Fl *v = ud;
    if (!e || !*e || v != live) return;
    snprintf(login_email, sizeof login_email, "%s", e);
    str_trim(login_email);
    keyboard_open(T("Flimmertv: password"), "", true, pw_done, v);
}
static void ask_login(Fl *v) { keyboard_open(T("Flimmertv: email"), "", false, email_done, v); }

/* the sign-in ended (401 with a token): everything about it goes, and the user signs in again */
static void fl_signed_out(void) {
    tok_wipe();
    fl_gen++;
    if (!live) return;
    free_rows(live);
    free_requests(live);
    live->state = 3;
    live->btn = 0;
    snprintf(live->msg, sizeof live->msg, "%s", T("Signed out. Sign in again."));
}

/* address typed or found: plain http on the local network is Flimmertv's normal way (it has no https itself) */
static void url_done(void *ud, const char *text) {
    Fl *v = ud;
    if (!text || !*text || v != live) return;
    char *t = str_dup(text);
    str_trim(t);
    size_t n = strlen(t);
    while (n && t[n - 1] == '/') t[--n] = 0;
    char *u;
    if (strstr(t, "://")) u = str_dup(t);
    else u = str_printf("http://%s%s", t, strchr(t, ':') ? "" : ":7676");
    bool changed = strcmp(u, fl_url()) != 0;
    cfg_set("flimmer_url", u);
    free(t);
    free(u);
    if (changed && tok[0]) tok_wipe(); /* a token belongs to one server */
    if (tok[0]) { v->state = 1; fl_call("GET", "/api/v1/app/me", NULL, me_done, v); }
    else ask_login(v);
}

static FlFound found[FL_MAX_FOUND];
static int n_found;
static void enter_address(Fl *v) {
    keyboard_open(T("Flimmertv address (e.g. 192.168.1.10:7676)"), fl_url(), false, url_done, v);
}
static void found_pick(void *ud, int idx) {
    Fl *v = ud;
    if (v != live || idx < 0) return;
    if (idx >= n_found) { enter_address(v); return; } /* "Enter address manually" */
    url_done(v, found[idx].url);
}
static int disc_prev_state;
static void disc_done(void *ud, const FlFound *f, int n) {
    Fl *v = ud;
    if (v != live || v->state != 1) return;
    v->state = disc_prev_state;
    if (!n) { app_toast("%s", T("No Flimmertv server found - enter the address")); return; }
    n_found = n < FL_MAX_FOUND ? n : FL_MAX_FOUND;
    const char *items[FL_MAX_FOUND + 1];
    char lbl[FL_MAX_FOUND][200];
    for (int i = 0; i < n_found; i++) {
        found[i] = f[i];
        snprintf(lbl[i], sizeof lbl[i], "%s   ·   %s", f[i].name, f[i].url + 7);
        items[i] = lbl[i];
    }
    items[n_found] = T("Enter address manually");
    choice_open(n_found == 1 ? T("Found 1 Flimmertv server") : T("Flimmertv servers on the network"), items, n_found + 1, 0, found_pick, v);
}
static void find_servers(Fl *v) {
    disc_prev_state = v->state == 1 ? 0 : v->state;
    v->state = 1;
    snprintf(v->msg, sizeof v->msg, "%s", T("Searching for Flimmertv on the network…"));
    flimmer_discover(disc_done, v);
}

static void sign_out_done(void) {
    if (!live) return;
    live->state = 0;
    live->btn = 0;
    live->user[0] = 0;
    live->left = -1;
    build_side(live);
}
static void act_signout(void);
static void settings_cb(void *ud, int idx) {
    Fl *v = ud;
    if (v != live) return;
    if (idx == 0) enter_address(v);
    else if (idx == 1) find_servers(v);
    else if (idx == 2) act_signout();
}
static void open_settings(Fl *v) {
    const char *items[] = {T("Change Flimmertv address"), T("Find server on the network"), T("Sign out of Flimmer")};
    choice_open("Flimmer", items, tok[0] ? 3 : 2, 0, settings_cb, v);
}

static void search_done(void *ud, const char *q) {
    Fl *v = ud;
    if (!q || !*q || v != live) return;
    char *e = url_encode(q);
    char *path = str_printf("/api/v1/discover?type=all&term=%s", e);
    char *t = str_printf(T("Results for \"%s\""), q);
    show_grid(v, t, path);
    free(e); free(path); free(t);
}

static void side_activate(Fl *v, int idx) {
    int id = v->side_map[idx];
    char p[96];
    if (id != SM_SETTINGS && id != SM_SEARCH) v->side.active = idx;
    switch (id) {
    case SM_DISCOVER: show_discover(v); break;
    case SM_SEARCH: v->side.active = idx; keyboard_open(T("Search Flimmer"), "", false, search_done, v); break;
    case SM_REQUESTS: show_requests(v); break;
    case SM_MOVIES: show_grid(v, T("Popular movies"), "/api/v1/discover?list=popular"); break;
    case SM_SERIES: show_grid(v, T("Popular series"), "/api/v1/discover?type=tv&list=popular"); break;
    case SM_DANISH: show_grid(v, T("Danish movies"), "/api/v1/discover?language=da"); break;
    case SM_STREAMING:
        snprintf(p, sizeof p, "/api/v1/discover?provider=8&region=%s", region());
        show_grid(v, T("Popular on Netflix"), p);
        break;
    case SM_SETTINGS: open_settings(v); break;
    }
}

static void fl_enter(Screen *s) {
    Fl *v = s->st;
    live = v;
    fl_gen++;
    v->left = -1;
    side_init(&v->side, "Flimmer", ICON_FILM, ACCENT);
    build_side(v);
    tok_load();
    if (!*fl_url()) { v->state = 0; find_servers(v); return; }
    if (!tok[0]) { v->state = 0; return; }
    v->state = 1;
    snprintf(v->msg, sizeof v->msg, "%s", T("Connecting…"));
    fl_call("GET", "/api/v1/app/me", NULL, me_done, v);
}
/* back from a detail page: a request made there shows in the rows */
static void fl_resume(Screen *s) {
    Fl *v = s->st;
    live = v;
    if (v->state == 2 && v->view == V_REQUESTS && v->from_detail) show_requests(v);
    v->from_detail = false;
}

static void fl_destroy(Screen *s) {
    Fl *v = s->st;
    free_rows(v);
    free_requests(v);
    if (live == v) live = NULL;
    free(v);
    free(s);
}

static void fl_update(Screen *s, float dt) {
    Fl *v = s->st;
    side_update(&v->side, dt);
    if (v->view == V_REQUESTS) {
        float t = v->rsel * 150.0f - 450;
        v->rscroll = anim_to(v->rscroll, t > 0 ? t : 0, dt, 12);
        return;
    }
    if (v->view == V_GRID) {
        float ty = v->row <= 0 ? 0 : v->row * 480 - 200;
        v->sy = anim_to(v->sy, ty < 0 ? 0 : ty, dt, 12);
        return;
    }
    v->sy = anim_to(v->sy, v->row * 530.0f, dt, 12);
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

static void draw_poster(const FItem *it, float x, float y, float w, float h, bool f) {
    float fa = ui_focus(it, f), sc = 1 + 0.07f * fa, ww = w * sc, hh = h * sc, xx = x - (ww - w) / 2, yy = y - (hh - h) / 2;
    ui_focus_under(xx, yy, ww, hh, 14, fa);
    fill_rrect(xx, yy, ww, hh, 14, C_SURFACE);
    if (!img_draw_cover(img_get(it->poster), xx, yy, ww, hh, 14, 1))
        text_draw_wrap(F_SEMI, 24, xx + 16, yy + 20, ww - 32, 4, 30, C_TEXT2, it->title);
    const char *st = status_text(it->status);
    if (st) {
        Color c = it->status >= ST_PARTIAL ? C_OK : ACCENT;
        float bw = text_width(F_BOLD, 18, st) + 20;
        fill_rrect(xx + 10, yy + 10, bw, 30, 15, c);
        text_draw(F_BOLD, 18, xx + 10 + bw / 2, yy + 15, C_TEXT, AL_CENTER, st);
    }
    ui_focus_over(xx, yy, ww, hh, 14, fa);
    text_draw_clip(F_SEMI, 22, x, y + h + 14, w, f ? C_TEXT : C_TEXT2, AL_LEFT, it->title);
    char sub[64];
    if (it->year) snprintf(sub, sizeof sub, "%d · %s", it->year, it->tv ? T("Series") : T("Movie"));
    else snprintf(sub, sizeof sub, "%s", it->tv ? T("Series") : T("Movie"));
    text_draw_clip(F_REG, 20, x, y + h + 42, w, C_TEXT3, AL_LEFT, sub);
}

static int setup_buttons(Fl *v, const char **b) {
    if (*fl_url()) { b[0] = T("Sign in"); b[1] = T("Change address"); b[2] = T("Find server on the network"); return 3; }
    b[0] = T("Find server on the network"); b[1] = T("Enter address"); return 2;
}

/* help line at the bottom: what OK does, and ← for the menu (no button bar) */
static void draw_help(float x, const char *ok) {
    fill_vgrad(SIDE_W, UI_H - 150, UI_W - SIDE_W, 150, RGBA(12, 14, 20, 0), RGBA(12, 14, 20, 245)); /* the rows fade out behind it */
    draw_hint(x, UI_H - 62, "OK", ok);
    draw_hint(x + text_width(F_SEMI, 20, "OK") + 30 + text_width(F_REG, 22, ok) + 44, UI_H - 62, "←", T("Menu and search"));
}

static void draw_requests(Fl *v, float x0, float cx) {
    text_draw(F_BOLD, 44, x0, 70, C_TEXT, AL_LEFT, T("My requests"));
    if (v->rq_loading && !v->n_rq) { draw_spinner(cx, UI_H / 2, 30, C_TEXT); return; }
    if (!v->n_rq) {
        text_draw_wrap(F_REG, 26, x0, 160, 1100, 3, 36, C_TEXT3,
                       v->msg[0] ? v->msg : T("No requests yet. Find a movie or a series and choose Request."));
        return;
    }
    bool cf = !v->side.open;
    clip_push(SIDE_W, 140, UI_W, UI_H - 220);
    for (int i = 0; i < v->n_rq; i++) {
        FRequest *r = &v->rq[i];
        float y = 160 + i * 150 - v->rscroll;
        if (y > UI_H || y + 140 < 0) continue;
        bool f = cf && i == v->rsel;
        float w = UI_W - x0 - 120;
        float fa = ui_focus(r, f);
        ui_focus_under(x0, y, w, 132, 18, fa);
        fill_rrect(x0, y, w, 132, 18, color_mix(RGBA(255, 255, 255, 14), RGBA(255, 255, 255, 40), fa));
        fill_rrect(x0 + 12, y + 10, 75, 112, 10, C_SURFACE);
        img_draw_cover(img_get(r->it.poster), x0 + 12, y + 10, 75, 112, 10, 1);
        float tx = x0 + 112;
        char head[300];
        if (r->it.year) snprintf(head, sizeof head, "%s (%d)", r->it.title, r->it.year);
        else snprintf(head, sizeof head, "%s", r->it.title);
        text_draw_clip(F_SEMI, 28, tx, y + 18, w - 520, C_TEXT, AL_LEFT, head);
        char sub[200];
        snprintf(sub, sizeof sub, "%s%s%s%s%s", r->it.tv ? T("Series") : T("Movie"), r->seasons[0] ? " · " : "", r->seasons,
                 r->date[0] ? " · " : "", r->date);
        text_draw_clip(F_REG, 22, tx, y + 58, w - 520, C_TEXT2, AL_LEFT, sub);
        if (r->reason[0]) text_draw_clip(F_REG, 22, tx, y + 90, w - 160, C_TEXT3, AL_LEFT, r->reason);
        text_draw(F_SEMI, 24, x0 + w - 30, y + 20, request_color(r->status), AL_RIGHT, request_text(r->status));
        ui_focus_over(x0, y, w, 132, 18, fa);
    }
    clip_pop();
    if (cf) draw_help(x0, v->rsel < v->n_rq && v->rq[v->rsel].status == RQ_PENDING ? T("Open, withdraw") : T("Open"));
}

static void fl_draw(Screen *s) {
    Fl *v = s->st;
    draw_background();
    draw_topbar(NULL);
    float x0 = SIDE_W + 60, cx = x0 + (UI_W - x0) / 2;
    if (v->state != 2) {
        draw_icon(ICON_FILM, cx, 300, 120, ACCENT);
        if (v->state == 0) {
            text_draw(F_BOLD, 42, cx, 430, C_TEXT, AL_CENTER, T("Connect to Flimmertv"));
            text_draw_wrap(F_REG, 28, cx - 520, 500, 1040, 6, 40, C_TEXT2,
                           T("Find and request movies and series on your Flimmertv server.\n\nFind it on the network, or enter its address, e.g. 192.168.1.10:7676. Then sign in with your Flimmertv email and password."));
        } else if (v->state == 1) {
            draw_spinner(cx, 520, 30, C_TEXT);
            text_draw(F_REG, 26, cx, 580, C_TEXT2, AL_CENTER, v->msg);
        } else {
            text_draw(F_BOLD, 42, cx, 430, C_TEXT, AL_CENTER, T("Could not sign in"));
            text_draw_wrap(F_REG, 28, cx - 520, 500, 1040, 4, 40, C_TEXT2, v->msg);
        }
        if (v->state != 1) {
            const char *b[3];
            int nb = setup_buttons(v, b);
            draw_buttons(cx, 780, b, nb, v->side.open ? -1 : v->btn);
            text_draw(F_REG, 22, cx, UI_H - 70, C_TEXT3, AL_CENTER, T("Left (from the first button): menu"));
        }
        side_draw(&v->side);
        return;
    }
    if (v->view == V_REQUESTS) { draw_requests(v, x0, cx); side_draw(&v->side); return; }
    bool cf = !v->side.open;
    float rows_y = 140;
    if (v->view == V_DISCOVER && v->n_rows && v->row < v->n_rows && v->col < v->rows[v->row].n) {
        /* the title in focus, at the top */
        FItem *h = &v->rows[v->row].it[v->col];
        text_draw_clip(F_BOLD, 56, x0, 120, UI_W - x0 - 300, C_TEXT, AL_LEFT, h->title);
        char meta[200], yr[16] = "";
        const char *st = status_text(h->status);
        if (h->year) snprintf(yr, sizeof yr, "%d  ·  ", h->year);
        snprintf(meta, sizeof meta, "%s%s%s%s", yr, h->tv ? T("Series") : T("Movie"), st ? "  ·  " : "", st ? st : "");
        text_draw(F_SEMI, 27, x0, 200, C_TEXT2, AL_LEFT, meta);
        text_draw_wrap(F_REG, 27, x0, 248, 1050, 3, 37, color_alpha(C_TEXT, 0.9f), h->overview ? h->overview : "");
        rows_y = 140 + HDR_H;
    }
    clip_push(SIDE_W, rows_y - 20, UI_W, UI_H - rows_y + 20);
    if (v->view == V_GRID && v->n_rows) {
        FRow *r = &v->rows[0];
        float top = 70 - v->sy;
        text_draw(F_BOLD, 44, x0, top, C_TEXT, AL_LEFT, v->grid_title);
        if (r->loading && r->n == 0) draw_spinner(cx, UI_H / 2, 30, C_TEXT);
        else if (r->n == 0) text_draw(F_REG, 26, x0, top + 120, C_TEXT3, AL_LEFT, T("Nothing found"));
        for (int i = 0; i < r->n; i++) {
            int gr = i / FCOLS, gc = i % FCOLS;
            float x = x0 + gc * 280, y = top + 90 + gr * 480;
            if (y > UI_H || y + 480 < 0) continue;
            draw_poster(&r->it[i], x, y, 250, 375, cf && v->row == gr && v->col == gc);
        }
    } else {
        float y = rows_y - v->sy;
        for (int r = 0; r < v->n_rows; r++) {
            FRow *row = &v->rows[r];
            text_draw(F_SEMI, 30, x0, y, r == v->row && cf ? C_TEXT : C_TEXT2, AL_LEFT, row->title);
            float iy = y + 56;
            if (row->loading && row->n == 0) draw_spinner(x0 + 100, iy + 150, 24, C_TEXT2);
            else if (row->n == 0)
                text_draw(F_REG, 24, x0, iy + 120, C_TEXT3, AL_LEFT, row->foryou ? T("Comes when there are movies in the library") : T("Nothing found"));
            for (int i = 0; i < row->n; i++) {
                float x = x0 + i * 280 - v->sx[r];
                if (x > UI_W || x + 250 < SIDE_W) continue;
                draw_poster(&row->it[i], x, iy, 250, 375, cf && r == v->row && v->col == i);
            }
            y += 530;
        }
    }
    clip_pop();
    if (cf) draw_help(x0, T("Details and request"));
    side_draw(&v->side);
}

static FItem *focused(Fl *v) {
    if (!v->n_rows) return NULL;
    if (v->view == V_GRID) {
        FRow *g = &v->rows[0];
        int i = v->row * FCOLS + v->col;
        return i >= 0 && i < g->n ? &g->it[i] : NULL;
    }
    FRow *r = &v->rows[v->row];
    return v->col < r->n ? &r->it[v->col] : NULL;
}

static bool fl_input(Screen *s, const InputEvent *ev) {
    Fl *v = s->st;
    int act;
    if (side_input(&v->side, ev, &act)) {
        if (act >= 0) side_activate(v, act);
        return true;
    }
    if (v->state != 2) {
        if (v->state == 1) return ev->a != A_BACK && ev->a != A_HOME;
        const char *b[3];
        int nb = setup_buttons(v, b);
        if (buttons_nav(&v->btn, nb, ev)) return true;
        if (ev->a == A_OK) {
            if (ev->repeat) return true;
            const char *l = b[v->btn < nb ? v->btn : 0];
            if (!strcmp(l, T("Sign in"))) ask_login(v);
            else if (!strcmp(l, T("Find server on the network"))) find_servers(v);
            else enter_address(v);
            return true;
        }
        if (ev->a == A_LEFT || ev->a == A_MENU) { side_open(&v->side); return true; }
        return false;
    }
    if (ev->a == A_MENU) { side_open(&v->side); return true; }
    if (ev->a == A_SEARCH || ev->a == A_CHAR) {
        char init[8] = {0};
        if (ev->a == A_CHAR && ev->ch > ' ' && ev->ch < 128) init[0] = (char)ev->ch;
        keyboard_open(T("Search Flimmer"), init, false, search_done, v);
        return true;
    }
    if (v->view == V_REQUESTS) {
        switch (ev->a) {
        case A_UP: if (v->rsel > 0) v->rsel--; return true;
        case A_DOWN: if (v->rsel < v->n_rq - 1) v->rsel++; return true;
        case A_LEFT: side_open(&v->side); return true;
        case A_OK: if (!ev->repeat && v->rsel < v->n_rq) open_rq_sheet(v, &v->rq[v->rsel]); return true;
        case A_BACK: v->side.active = 0; show_discover(v); return true;
        default: return true;
        }
    }
    FRow *gr = v->view == V_GRID && v->n_rows ? &v->rows[0] : NULL;
    switch (ev->a) {
    case A_UP:
        if (v->row > 0) { v->row--; if (!gr) v->col = 0; }
        return true;
    case A_DOWN:
        if (gr) {
            if (!gr->n) return true;
            if ((v->row + 1) * FCOLS < gr->n) v->row++;
            if (v->row * FCOLS + v->col >= gr->n) v->col = (gr->n - 1) % FCOLS;
            if ((v->row + 3) * FCOLS >= gr->n) row_more(gr); /* near the end: the next page */
        } else if (v->row < v->n_rows - 1) { v->row++; v->col = 0; }
        return true;
    case A_LEFT:
        if (v->col > 0) v->col--;
        else side_open(&v->side);
        return true;
    case A_RIGHT:
        if (gr) { if (v->col < FCOLS - 1 && v->row * FCOLS + v->col + 1 < gr->n) v->col++; }
        else if (v->n_rows && v->col < v->rows[v->row].n - 1) {
            v->col++;
            if (v->col >= v->rows[v->row].n - 4) row_more(&v->rows[v->row]);
        }
        return true;
    case A_OK: {
        FItem *it = focused(v);
        if (it && !ev->repeat) app_push(fl_detail(it));
        return true;
    }
    case A_BACK:
        if (v->view == V_GRID) { v->side.active = 0; show_discover(v); return true; }
        if (v->row > 0 || v->col > 0) { v->row = v->col = 0; return true; }
        return false;
    default: return false;
    }
}

Screen *flimmer_screen(void) {
    Screen *s = xcalloc(1, sizeof *s);
    s->name = "flimmer";
    s->st = xcalloc(1, sizeof(Fl));
    s->enter = fl_enter;
    s->resume = fl_resume;
    s->update = fl_update;
    s->draw = fl_draw;
    s->input = fl_input;
    s->destroy = fl_destroy;
    return s;
}

/* ---------- detail page: the title, its buttons (Request, Withdraw, Trailer) and "More like this" ---------- */
#define MAX_SEASONS 64
typedef struct {
    FItem it;
    char *backdrop, *trailer, *tagline;
    char genres[200], extra[160];
    int request_id;
    int seasons[MAX_SEASONS], sstat[MAX_SEASONS], eps[MAX_SEASONS], n_seasons;
    bool picker, pick[MAX_SEASONS];
    int psel;
    float pscroll;
    bool requesting, loaded, failed;
    FItem sim[24];
    int n_sim, scol, focus, btn; /* focus: 0 buttons, 1 "More like this" */
    float scroll, ssx;
} FDetail;
static FDetail *open_details[16];
static bool detail_alive(FDetail *d) {
    for (int i = 0; i < 16; i++) if (open_details[i] == d) return true;
    return false;
}

enum { B_REQUEST, B_WITHDRAW, B_TRAILER };
static int detail_buttons(FDetail *d, int *k) {
    int n = 0;
    k[n++] = B_REQUEST;
    if (d->request_id > 0 && d->it.status == ST_PENDING) k[n++] = B_WITHDRAW;
    if (d->trailer) k[n++] = B_TRAILER;
    return n;
}
static bool can_request(FDetail *d) { return d->it.status == ST_NONE || (d->it.status == ST_PARTIAL && d->it.tv); }

static void add_sim(FDetail *d, cJSON *arr) {
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (d->n_sim >= 24) return;
        FItem it;
        if (!fitem_parse(&it, o)) continue;
        bool dup = it.tmdb == d->it.tmdb && it.tv == d->it.tv;
        for (int i = 0; i < d->n_sim && !dup; i++) dup = d->sim[i].tmdb == it.tmdb && d->sim[i].tv == it.tv;
        if (dup) { fitem_free(&it); continue; }
        d->sim[d->n_sim++] = it;
    }
}

static void details_done(void *ud, cJSON *json, long status) {
    FDetail *d = ud;
    if (!detail_alive(d)) return;
    if (status != 200 || !json) { d->failed = true; return; }
    d->loaded = true;
    char *st = jdup(json, "mediaStatus");
    if (st) d->it.status = media_status(st);
    free(st);
    char *ov = jdup(json, "overview");
    if (ov) { free(d->it.overview); d->it.overview = ov; }
    free(d->backdrop); d->backdrop = jimg(json, "backdropUrl");
    free(d->tagline); d->tagline = jdup(json, "tagline");
    char *tr = jdup(json, "trailerUrl");
    if (tr && (strstr(tr, "youtube.com/") || strstr(tr, "youtu.be/")) && !strncmp(tr, "https://", 8)) d->trailer = tr;
    else free(tr);
    d->request_id = jint(json, "requestId", 0);
    cJSON *g;
    d->genres[0] = 0;
    cJSON_ArrayForEach(g, cJSON_GetObjectItem(json, "genres")) {
        cJSON *n = cJSON_IsString(g) ? g : cJSON_GetObjectItem(g, "name");
        if (!cJSON_IsString(n)) continue;
        size_t l = strlen(d->genres);
        snprintf(d->genres + l, sizeof d->genres - l, "%s%s", l ? ", " : "", n->valuestring);
    }
    int rt = jint(json, "runtime", 0);
    if (!rt) rt = jint(json, "episodeRuntime", 0);
    char *cert = jdup(json, "certification");
    d->extra[0] = 0;
    if (rt > 0) snprintf(d->extra, sizeof d->extra, T("%d min"), rt);
    if (cert) { size_t l = strlen(d->extra); snprintf(d->extra + l, sizeof d->extra - l, "%s%s", l ? "   " : "", cert); }
    free(cert);
    d->n_seasons = 0;
    cJSON *se;
    cJSON_ArrayForEach(se, cJSON_GetObjectItem(json, "seasons")) {
        int num = jint(se, "seasonNumber", -1);
        if (num <= 0 || d->n_seasons >= MAX_SEASONS) continue; /* season 0 = specials: not offered */
        char *ss = jdup(se, "status");
        d->sstat[d->n_seasons] = media_status(ss);
        free(ss);
        d->eps[d->n_seasons] = jint(se, "episodeCount", 0);
        d->seasons[d->n_seasons++] = num;
    }
    if (d->it.tv && d->n_seasons) {
        size_t l = strlen(d->extra);
        snprintf(d->extra + l, sizeof d->extra - l, d->n_seasons == 1 ? T("%s%d season") : T("%s%d seasons"), l ? "   " : "", d->n_seasons);
    }
    add_sim(d, cJSON_GetObjectItem(json, "recommendations"));
    add_sim(d, cJSON_GetObjectItem(json, "similar"));
}

static void detail_enter(Screen *s) {
    FDetail *d = s->st;
    char p[64];
    snprintf(p, sizeof p, "/api/v1/discover/%s/%d", d->it.tv ? "tv" : "movie", d->it.tmdb);
    fl_call_ex("GET", p, NULL, details_done, d, true);
}

static void detail_update(Screen *s, float dt) {
    FDetail *d = s->st;
    d->scroll = anim_to(d->scroll, d->focus ? 470 : 0, dt, 12);
    float t = d->scol * 230 - 560;
    d->ssx = anim_to(d->ssx, t > 0 ? t : 0, dt, 14);
    float pt = (d->psel - 7) * 66.0f;
    d->pscroll = anim_to(d->pscroll, pt > 0 ? pt : 0, dt, 14);
}

static void request_done(void *ud, cJSON *json, long status) {
    FDetail *d = ud;
    if (!detail_alive(d)) return;
    d->requesting = false;
    if (status == 200 || status == 201) {
        char *st = jdup(json, "status"); /* pending, or straight to processing (auto-approve) */
        int rs = request_status(st);
        free(st);
        d->request_id = jint(json, "id", d->request_id);
        bool rest = false;
        for (int i = 0; i < d->n_seasons; i++) { if (d->pick[i]) d->sstat[i] = rs == RQ_PENDING ? ST_PENDING : ST_PROCESSING; if (d->sstat[i] == ST_NONE) rest = true; }
        d->it.status = rest ? ST_PARTIAL : rs == RQ_PENDING ? ST_PENDING : ST_PROCESSING;
        d->btn = 0;
        app_toast(rs == RQ_PENDING ? T("Requested: %s - waiting for approval") : T("Requested: %s - on its way"), d->it.title);
        if (live && live->left > 0) { live->left--; build_side(live); }
    } else {
        char m[240];
        err_text(json, status, m, sizeof m); /* 409 already there/requested, 429 quota used: the server says it */
        app_toast("%s", m);
    }
}
static void send_request(FDetail *d) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mediaType", d->it.tv ? "tv" : "movie");
    cJSON_AddNumberToObject(o, "tmdbId", d->it.tmdb);
    if (d->it.tv) {
        cJSON *arr = cJSON_AddArrayToObject(o, "seasons");
        for (int i = 0; i < d->n_seasons; i++)
            if (d->pick[i]) cJSON_AddItemToArray(arr, cJSON_CreateNumber(d->seasons[i]));
    }
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    d->requesting = true;
    fl_call_ex("POST", "/api/v1/requests", body, request_done, d, true);
    free(body);
}
static void withdrawn(void *ud, cJSON *json, long status) {
    FDetail *d = ud;
    if (!detail_alive(d)) return;
    if (status == 204 || status == 200) {
        d->it.status = ST_NONE;
        d->request_id = 0;
        for (int i = 0; i < d->n_seasons; i++) if (d->sstat[i] == ST_PENDING) d->sstat[i] = ST_NONE;
        d->btn = 0;
        app_toast("%s", T("Request withdrawn"));
        if (live && live->left >= 0) { live->left++; build_side(live); }
    } else { char m[240]; err_text(json, status, m, sizeof m); app_toast("%s", m); }
}
static void withdraw_ok(void *ud, bool yes) {
    FDetail *d = ud;
    if (!yes || !detail_alive(d) || d->request_id <= 0) return;
    char p[64];
    snprintf(p, sizeof p, "/api/v1/requests/%d", d->request_id);
    fl_call_ex("DELETE", p, NULL, withdrawn, d, true);
}

static void play_trailer(FDetail *d) {
    char *t = str_printf(T("%s - trailer"), d->it.title);
    static const char *extra[] = {"--ytdl-format=bv*[height<=1080][vcodec!^=av01]+ba/b", NULL};
    PlayRequest rq = {0};
    rq.url = d->trailer;
    rq.title = t;
    rq.subtitle = T("Trailer");
    rq.description = d->it.overview;
    rq.poster = d->it.poster;
    rq.extra = extra;
    player_run(&rq);
    free(t);
}

static bool picker_input(FDetail *d, const InputEvent *ev) {
    int rows = d->n_seasons + 2;
    switch (ev->a) {
    case A_UP: if (d->psel > 0) d->psel--; break;
    case A_DOWN: if (d->psel < rows - 1) d->psel++; break;
    case A_BACK: d->picker = false; break;
    case A_OK:
        if (ev->repeat) break;
        if (d->psel == 0) { /* all seasons that can still be requested, or none */
            bool all = true;
            for (int i = 0; i < d->n_seasons; i++) if (d->sstat[i] == ST_NONE && !d->pick[i]) all = false;
            for (int i = 0; i < d->n_seasons; i++) d->pick[i] = d->sstat[i] == ST_NONE && !all;
        } else if (d->psel < rows - 1) {
            int i = d->psel - 1;
            if (d->sstat[i] == ST_NONE) d->pick[i] = !d->pick[i];
        } else {
            int n = 0;
            for (int i = 0; i < d->n_seasons; i++) n += d->pick[i];
            if (!n) { app_toast("%s", T("Select at least one season")); break; }
            d->picker = false;
            send_request(d);
        }
        break;
    default: break;
    }
    return true;
}

static void detail_destroy(Screen *s) {
    FDetail *d = s->st;
    for (int i = 0; i < 16; i++) if (open_details[i] == d) open_details[i] = NULL;
    fitem_free(&d->it);
    free(d->backdrop); free(d->trailer); free(d->tagline);
    for (int i = 0; i < d->n_sim; i++) fitem_free(&d->sim[i]);
    free(d);
    free(s);
}

static void draw_btn(float x, float y, const char *label, IconId ic, bool f, Color bg_off) {
    float w = text_width(F_SEMI, 28, label) + 110;
    Color bg = f ? C_FOCUS : bg_off;
    if (f) ui_focus_under(x, y, w, 76, 38, 1);
    fill_rrect(x, y, w, 76, 38, bg);
    Color fg = f ? RGB(10, 10, 10) : C_TEXT;
    draw_icon(ic, x + 42, y + 38, 26, fg);
    text_draw(F_SEMI, 28, x + 70, y + 20, fg, AL_LEFT, label);
    if (f) ui_focus_over(x, y, w, 76, 38, 1);
}

static void detail_draw(Screen *s) {
    FDetail *d = s->st;
    FItem *it = &d->it;
    draw_background();
    if (img_draw_cover(img_get(d->backdrop), 0, 0, UI_W, UI_H, 0, 0.5f)) {
        fill_hgrad(0, 0, UI_W * 0.8f, UI_H, RGBA(8, 9, 13, 250), RGBA(8, 9, 13, 0));
        fill_vgrad(0, UI_H * 0.5f, UI_W, UI_H * 0.5f, RGBA(8, 9, 13, 0), RGBA(8, 9, 13, 240));
    }
    float sc = d->scroll;
    fill_rrect(120, 200 - sc, 400, 600, 18, C_SURFACE);
    img_draw_cover(img_get(it->poster), 120, 200 - sc, 400, 600, 18, 1);
    float x = 580, y = 200 - sc;
    text_draw_clip(F_BOLD, 64, x, y, UI_W - x - 120, C_TEXT, AL_LEFT, it->title);
    char meta[400], yr[16] = "";
    if (it->year) snprintf(yr, sizeof yr, "%d   ", it->year);
    snprintf(meta, sizeof meta, "%s%s   %s   %s", yr, it->tv ? T("Series") : T("Movie"), d->extra, d->genres);
    text_draw_clip(F_REG, 26, x, y + 96, UI_W - x - 120, C_TEXT2, AL_LEFT, meta);
    float oy = y + 150;
    if (d->tagline) { text_draw_clip(F_SEMI, 26, x, oy, UI_W - x - 160, C_TEXT2, AL_LEFT, d->tagline); oy += 50; }
    if (d->failed) text_draw(F_REG, 26, x, oy, C_TEXT3, AL_LEFT, T("Could not fetch the details"));
    text_draw_wrap(F_REG, 28, x, oy, UI_W - x - 160, d->tagline ? 6 : 7, 40, C_TEXT, it->overview ? it->overview : "");
    /* the buttons */
    int k[3], nb = detail_buttons(d, k);
    if (d->btn >= nb) d->btn = nb - 1;
    float bx = x, by = 720 - sc;
    for (int i = 0; i < nb; i++) {
        bool f = !d->focus && !d->picker && d->btn == i;
        char lbl[64];
        if (k[i] == B_REQUEST) {
            const char *st = status_text(it->status);
            if (d->requesting) snprintf(lbl, sizeof lbl, "%s", T("Sending…"));
            else if (can_request(d)) snprintf(lbl, sizeof lbl, "%s", it->status == ST_PARTIAL ? T("Request more seasons") : T("Request"));
            else snprintf(lbl, sizeof lbl, "%s", st ? st : T("Requested"));
            bool on = can_request(d) && !d->requesting;
            draw_btn(bx, by, lbl, on ? ICON_STAR : ICON_CHECK, f, on ? RGBA(255, 255, 255, 40) : it->status >= ST_PARTIAL ? C_OK : ACCENT);
        } else if (k[i] == B_WITHDRAW) {
            snprintf(lbl, sizeof lbl, "%s", T("Withdraw request"));
            draw_btn(bx, by, lbl, ICON_BACK, f, RGBA(255, 255, 255, 40));
        } else {
            snprintf(lbl, sizeof lbl, "%s", T("Trailer"));
            draw_btn(bx, by, lbl, ICON_FILM, f, RGBA(255, 255, 255, 40));
        }
        bx += text_width(F_SEMI, 28, lbl) + 110 + 24;
    }
    if (it->status == ST_AVAILABLE) text_draw(F_REG, 24, x, 820 - sc, C_TEXT2, AL_LEFT, T("Already in the library - watch it in Plex or Jellyfin."));
    /* More like this */
    if (d->n_sim || !d->loaded) {
        float ry0 = 900 - sc;
        text_draw(F_SEMI, 30, 120, ry0, d->focus ? C_TEXT : C_TEXT2, AL_LEFT, T("More like this"));
        if (!d->n_sim && !d->failed) draw_spinner(220, ry0 + 200, 24, C_TEXT2);
        clip_push(0, ry0 + 30, UI_W, 460);
        for (int i = 0; i < d->n_sim; i++) {
            float px = 120 + i * 230 - d->ssx;
            if (px > UI_W || px + 200 < 0) continue;
            draw_poster(&d->sim[i], px, ry0 + 60, 200, 300, d->focus && d->scol == i);
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
            float ry = py + 100 + r * rh - d->pscroll;
            if (ry < py + 100 - rh || ry > py + ph) continue;
            bool f = d->psel == r;
            if (f) fill_rrect(px + 16, ry + 4, pw - 32, rh - 8, 14, r == rows - 1 ? C_FOCUS : RGBA(255, 255, 255, 30));
            if (r == rows - 1) {
                int n = 0;
                for (int i = 0; i < d->n_seasons; i++) n += d->pick[i];
                char t[64];
                if (!n) snprintf(t, sizeof t, "%s", T("Select at least one season"));
                else snprintf(t, sizeof t, n == 1 ? T("Request %d season") : T("Request %d seasons"), n);
                text_draw(F_SEMI, 28, px + pw / 2, ry + 16, f ? RGB(10, 10, 10) : C_TEXT, AL_CENTER, t);
                continue;
            }
            int i = r - 1;
            bool can = r == 0 || d->sstat[i] == ST_NONE, on;
            if (r == 0) {
                bool any = false;
                on = true;
                for (int q = 0; q < d->n_seasons; q++) if (d->sstat[q] == ST_NONE) { any = true; if (!d->pick[q]) on = false; }
                on = on && any;
            } else on = d->pick[i] || d->sstat[i] != ST_NONE;
            float cbx = px + 44, cby = ry + rh / 2 - 16;
            if (on) {
                fill_rrect(cbx, cby, 32, 32, 8, can ? C_FOCUS : (d->sstat[i] >= ST_PARTIAL ? C_OK : ACCENT));
                draw_icon(ICON_CHECK, cbx + 16, cby + 16, 20, can ? RGB(10, 10, 10) : C_TEXT);
            } else stroke_rrect(cbx, cby, 32, 32, 8, 3, can ? C_TEXT : C_TEXT3);
            char lbl[48];
            if (r == 0) snprintf(lbl, sizeof lbl, "%s", T("All seasons"));
            else snprintf(lbl, sizeof lbl, T("Season %d"), d->seasons[i]);
            text_draw(F_SEMI, 28, cbx + 56, ry + 16, can ? C_TEXT : C_TEXT3, AL_LEFT, lbl);
            char info[48] = "";
            if (r > 0) {
                const char *st = d->sstat[i] != ST_NONE ? status_text(d->sstat[i]) : NULL;
                if (st) snprintf(info, sizeof info, "%s", st);
                else if (d->eps[i]) snprintf(info, sizeof info, T("%d episodes"), d->eps[i]);
            }
            text_draw(F_REG, 24, px + pw - 44, ry + 20, C_TEXT3, AL_RIGHT, info);
        }
        clip_pop();
    }
}

static bool detail_input(Screen *s, const InputEvent *ev) {
    FDetail *d = s->st;
    if (d->picker) return picker_input(d, ev);
    if (d->focus) { /* the "More like this" row */
        if (ev->a == A_UP) d->focus = 0;
        else if (ev->a == A_LEFT) { if (d->scol > 0) d->scol--; }
        else if (ev->a == A_RIGHT) { if (d->scol < d->n_sim - 1) d->scol++; }
        else if (ev->a == A_OK && !ev->repeat && d->scol < d->n_sim) app_push(fl_detail(&d->sim[d->scol]));
        else if (ev->a != A_DOWN) return false;
        return true;
    }
    int k[3], nb = detail_buttons(d, k);
    if (ev->a == A_DOWN) { if (d->n_sim) d->focus = 1; return true; }
    if (ev->a == A_LEFT) { if (d->btn > 0) d->btn--; return true; }
    if (ev->a == A_RIGHT) { if (d->btn < nb - 1) d->btn++; return true; }
    if ((ev->a != A_OK && ev->a != A_PLAYPAUSE) || ev->repeat) return false;
    if (d->btn >= nb) d->btn = 0;
    if (k[d->btn] == B_TRAILER) { play_trailer(d); return true; }
    if (k[d->btn] == B_WITHDRAW) {
        confirm_open(T("Withdraw request?"), d->it.title, T("Withdraw"), T("Cancel"), withdraw_ok, d);
        return true;
    }
    if (!can_request(d) || d->requesting) return true;
    if (!d->loaded) { app_toast("%s", d->failed ? T("Could not fetch the details") : T("Loading…")); return true; }
    if (d->it.tv) {
        int n = 0;
        for (int i = 0; i < d->n_seasons; i++) { d->pick[i] = d->sstat[i] == ST_NONE; n += d->pick[i]; }
        if (!n) { app_toast("%s", T("All seasons are already requested or available")); return true; }
        d->picker = true;
        d->psel = 0;
        d->pscroll = 0;
        return true;
    }
    send_request(d);
    return true;
}

static Screen *fl_detail(const FItem *src) {
    Screen *s = xcalloc(1, sizeof *s);
    FDetail *d = xcalloc(1, sizeof *d);
    fitem_copy(&d->it, src);
    bool slot = false;
    for (int i = 0; i < 16 && !slot; i++) if (!open_details[i]) { open_details[i] = d; slot = true; }
    s->name = "flimmer-detail";
    s->st = d;
    s->enter = detail_enter;
    s->update = detail_update;
    s->draw = detail_draw;
    s->input = detail_input;
    s->destroy = detail_destroy;
    return s;
}

/* ---------- sign out: the token ends at Flimmertv, and the address and token are deleted here ---------- */
void flimmer_forget(void) {
    if (!tok[0]) tok_load();
    if (*fl_url() && tok[0]) {
        char *url = str_printf("%s/api/v1/app/logout", fl_url());
        char auth[300];
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", tok);
        const char *hdr[] = {"Accept: application/json", auth, NULL};
        HttpBuf b;
        http_request("POST", url, hdr, "", &b, 4);
        http_free(&b);
        memset(auth, 0, sizeof auth);
        free(url);
    }
    tok_wipe();
    cfg_set("flimmer_url", "");
    fl_gen++; /* answers on their way are dropped */
    if (live) { free_rows(live); free_requests(live); }
    wipe_session_traces();
    sign_out_done();
}

/* ---------- the app (see NotaApp in tv.h) ---------- */
static void act_signout(void) { app_ask_signout("Flimmer", flimmer_forget); }
static const char *fl_signed_out_item(void) {
    if (!tok[0]) tok_load();
    return tok[0] ? NULL : ITEM_HIDDEN;
}
static const char *server_value(void) { return *fl_url() ? fl_url() + (strncmp(fl_url(), "http://", 7) ? 0 : 7) : T("Not set"); }
static const Item fl_items[] = {
    {N_("Server"), IT_INFO, .value = server_value, .help = N_("The Flimmertv server the app talks to. Change it from inside the app (Server and sign-in)")},
    {N_("Sign out of Flimmer"), IT_ACTION, .action = act_signout, .locked = fl_signed_out_item,
     .help = N_("Ends the sign-in at Flimmertv and deletes the address and sign-in from this box")},
};

const NotaApp nota_app = {
    NOTA_API, "flimmer", "Flimmer", N_("Find and request movies and series on your Flimmertv server"), APP_VERSION, 61, APP_DEFAULT_OFF,
    ICON_FILM, {251, 113, 133, 255}, {124, 29, 79, 255}, {255, 255, 255, 255},
    .open = flimmer_screen, .forget = flimmer_forget,
    .settings = fl_items, .n_settings = sizeof fl_items / sizeof fl_items[0],
};
