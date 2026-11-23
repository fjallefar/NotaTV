/* NotaTV app package "immich" - Immich: photos and videos from your own Immich server (open source API).
 * Sign in with the server address and an API key (typed with the remote or from the phone). Tabs: On this day,
 * Recent, Albums, People; OK on a photo = full screen (left/right, Play = slideshow), OK on a video = mpv.
 * Images are always the server's "preview" (JPEG, ~1440 px): the small "thumbnail" is WebP, which stb_image cannot
 * read, and the original can be huge. The key goes in the address as apiKey= (log_url masks it). */
#define _GNU_SOURCE
#include "tv.h"
#include "vendor/cJSON.h"
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const NotaApp nota_app;

#define COLS 6
#define TILE 264
#define GAP 28
#define PITCH (TILE + 70)
#define MAXN 2000

typedef struct { char id[40]; bool video; char label[64]; } Asset;
typedef struct { char id[40], name[96], thumb[40]; int count; bool person; } Coll;

enum { T_MEMORIES, T_RECENT, T_ALBUMS, T_PEOPLE, T_COUNT };
enum { K_MEMORIES, K_RECENT, K_ALBUMS, K_PEOPLE, K_ALBUM, K_PERSON, K_LOGIN };

typedef struct {
    bool top;          /* the app's first screen (with tabs); false = one album/person */
    int tab, kind;
    char coll_id[40], title[96];
    Asset *a;
    int na;
    Coll *c;
    int nc;
    int sel, chip;
    bool on_chips;
    float scroll;
    bool loading, failed;
    unsigned gen;
    int btn;           /* login screen */
} Im;

static const char *im_url(void) { return cfg_get("im_url", ""); }
static const char *im_key(void) { return cfg_get("im_key", ""); }
static bool signed_in(void) { return *im_url() && *im_key(); }

/* ---------- server ---------- */
static cJSON *im_call(const char *base, const char *key, const char *method, const char *path, const char *body, long *status) {
    char *url = str_printf("%s%s", base, path), *kh = str_printf("x-api-key: %s", key);
    const char *hdr[] = {kh, "Accept: application/json", "Content-Type: application/json", NULL};
    HttpBuf b;
    memset(&b, 0, sizeof b);
    cJSON *j = NULL;
    if (http_request(method, url, hdr, body, &b, 20)) {
        if (status) *status = b.status;
        if (b.status >= 200 && b.status < 300 && b.data) j = cJSON_Parse(b.data);
    } else if (status) *status = 0;
    http_free(&b);
    free(url);
    free(kh);
    return j;
}

static char *asset_img(const char *id) {
    char *k = url_encode(im_key());
    char *u = str_printf("%s/api/assets/%s/thumbnail?size=preview&apiKey=%s", im_url(), id, k);
    free(k);
    return u;
}
static char *person_img(const char *id) {
    char *k = url_encode(im_key());
    char *u = str_printf("%s/api/people/%s/thumbnail?apiKey=%s", im_url(), id, k);
    free(k);
    return u;
}

/* "2021-11-22T14:03:00.000Z" -> "22.11.2021" */
static void fmt_date(const char *iso, char *out, size_t n) {
    int y, m, d;
    if (iso && sscanf(iso, "%d-%d-%d", &y, &m, &d) == 3) snprintf(out, n, "%d.%d.%d", d, m, y);
    else snprintf(out, n, "%s", "");
}

static void add_asset(Asset **a, int *n, const cJSON *o, const char *label) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(o, "id"));
    if (!id || *n >= MAXN || strlen(id) >= sizeof((*a)->id)) return;
    *a = xrealloc(*a, sizeof **a * (*n + 1));
    Asset *x = &(*a)[(*n)++];
    memset(x, 0, sizeof *x);
    snprintf(x->id, sizeof x->id, "%s", id);
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(o, "type"));
    x->video = t && !strcmp(t, "VIDEO");
    if (label) snprintf(x->label, sizeof x->label, "%s", label);
    else fmt_date(cJSON_GetStringValue(cJSON_GetObjectItem(o, "localDateTime")), x->label, sizeof x->label);
}

typedef struct {
    unsigned gen;
    int kind;
    char base[512], key[160], id[40];
    Asset *a;
    int na;
    Coll *c;
    int nc;
    bool failed;
    long status;
} Job;

static void search_assets(Job *j, const char *person) {
    char body[256];
    if (person) snprintf(body, sizeof body, "{\"size\":250,\"order\":\"desc\",\"personIds\":[\"%s\"]}", person);
    else snprintf(body, sizeof body, "{\"size\":250,\"order\":\"desc\"}");
    cJSON *r = im_call(j->base, j->key, "POST", "/api/search/metadata", body, &j->status);
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(cJSON_GetObjectItem(r, "assets"), "items")) add_asset(&j->a, &j->na, it, NULL);
    j->failed = !r;
    cJSON_Delete(r);
}

static void job_work(void *arg) {
    Job *j = arg;
    cJSON *r = NULL;
    const cJSON *it;
    switch (j->kind) {
    case K_LOGIN:
        r = im_call(j->base, j->key, "GET", "/api/users/me", NULL, &j->status);
        j->failed = !r;
        break;
    case K_MEMORIES: {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char path[96];
        snprintf(path, sizeof path, "/api/memories?for=%04d-%02d-%02dT00:00:00.000Z", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        r = im_call(j->base, j->key, "GET", path, NULL, &j->status);
        j->failed = !r;
        cJSON_ArrayForEach(it, r) {
            int year = (int)json_num(cJSON_GetObjectItem(it, "data"), "year"), ago = tm.tm_year + 1900 - year;
            char label[64] = "";
            if (year > 0 && ago == 1) snprintf(label, sizeof label, "%s", T("1 year ago"));
            else if (year > 0) snprintf(label, sizeof label, T("%d years ago"), ago);
            const cJSON *as;
            cJSON_ArrayForEach(as, cJSON_GetObjectItem(it, "assets")) add_asset(&j->a, &j->na, as, label[0] ? label : NULL);
        }
        break;
    }
    case K_RECENT: search_assets(j, NULL); break;
    case K_PERSON: search_assets(j, j->id); break;
    case K_ALBUM: {
        char path[96];
        snprintf(path, sizeof path, "/api/albums/%s", j->id);
        r = im_call(j->base, j->key, "GET", path, NULL, &j->status);
        j->failed = !r;
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(r, "assets")) add_asset(&j->a, &j->na, it, NULL);
        break;
    }
    case K_ALBUMS:
    case K_PEOPLE: {
        bool people = j->kind == K_PEOPLE;
        r = im_call(j->base, j->key, "GET", people ? "/api/people?withHidden=false" : "/api/albums", NULL, &j->status);
        j->failed = !r;
        const cJSON *list = people ? cJSON_GetObjectItem(r, "people") : r;
        cJSON_ArrayForEach(it, list) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, people ? "name" : "albumName"));
            if (!id || !name || !*name || strlen(id) >= 40 || j->nc >= MAXN) continue; /* unnamed faces: left out */
            j->c = xrealloc(j->c, sizeof *j->c * (j->nc + 1));
            Coll *c = &j->c[j->nc++];
            memset(c, 0, sizeof *c);
            snprintf(c->id, sizeof c->id, "%s", id);
            snprintf(c->name, sizeof c->name, "%s", name);
            const char *th = cJSON_GetStringValue(cJSON_GetObjectItem(it, "albumThumbnailAssetId"));
            if (th && strlen(th) < sizeof c->thumb) snprintf(c->thumb, sizeof c->thumb, "%s", th);
            c->count = (int)json_num(it, "assetCount");
            c->person = people;
        }
        break;
    }
    }
    cJSON_Delete(r);
}

static Im *live;
static unsigned load_gen;
static void load(Im *v);

static void job_done(void *arg) {
    Job *j = arg;
    Im *v = live;
    if (v && j->gen == load_gen) {
        v->loading = false;
        if (j->kind == K_LOGIN) {
            if (j->failed) {
                app_toast(j->status == 401 || j->status == 403 ? T("The API key was not accepted") :
                          T("Cannot reach the Immich server"));
            } else {
                cfg_set("im_url", j->base);
                cfg_set("im_key", j->key);
                load(v);
            }
        } else {
            free(v->a);
            free(v->c);
            v->a = j->a; v->na = j->na;
            v->c = j->c; v->nc = j->nc;
            j->a = NULL; j->c = NULL;
            v->failed = j->failed;
            if (v->sel >= (v->na ? v->na : v->nc)) v->sel = 0;
        }
    }
    free(j->a);
    free(j->c);
    explicit_bzero(j->key, sizeof j->key);
    free(j);
}

static void submit(Im *v, int kind, const char *base, const char *key, const char *id) {
    Job *j = xcalloc(1, sizeof *j);
    j->gen = ++load_gen;
    j->kind = kind;
    snprintf(j->base, sizeof j->base, "%s", base);
    snprintf(j->key, sizeof j->key, "%s", key);
    if (id) snprintf(j->id, sizeof j->id, "%s", id);
    v->loading = true;
    job_submit_q(JQ_SLOW, job_work, job_done, j);
}

static void load(Im *v) {
    if (!signed_in()) return;
    v->failed = false;
    if (!v->top) { submit(v, v->kind, im_url(), im_key(), v->coll_id); return; }
    static const int kinds[] = {K_MEMORIES, K_RECENT, K_ALBUMS, K_PEOPLE};
    free(v->a); v->a = NULL; v->na = 0;
    free(v->c); v->c = NULL; v->nc = 0;
    v->sel = 0; v->scroll = 0;
    submit(v, kinds[v->tab], im_url(), im_key(), NULL);
}

/* ---------- sign in: address, then key ---------- */
static char pending_url[512];

static void key_done(void *ud, const char *text) {
    Im *v = ud;
    if (!text || !*text || v != live) return;
    char *k = str_dup(text);
    str_trim(k);
    submit(v, K_LOGIN, pending_url, k, NULL);
    explicit_bzero(k, strlen(k));
    free(k);
}

static void url_done(void *ud, const char *text) {
    Im *v = ud;
    if (!text || !*text || v != live) return;
    char *t = str_dup(text);
    str_trim(t);
    size_t n = strlen(t);
    while (n && t[n - 1] == '/') t[--n] = 0;
    if (n > 4 && !strcmp(t + n - 4, "/api")) t[n - 4] = 0; /* people often paste the API address */
    if (strstr(t, "://")) snprintf(pending_url, sizeof pending_url, "%s", t);
    else snprintf(pending_url, sizeof pending_url, "http://%s", t); /* Immich runs on the local network, usually :2283 */
    free(t);
    keyboard_open(T("API key (Immich → Account settings → API keys)"), "", true, key_done, v);
}

static void start_login(Im *v) {
    keyboard_open(T("Immich server address (e.g. 192.168.1.10:2283)"), *im_url() ? im_url() : "", false, url_done, v);
}

/* ---------- full screen viewer ---------- */
typedef struct {
    Asset *a;
    int n, i, prev;
    bool show;          /* slideshow */
    double shown_at, changed_at;
    float fade;
} View;

static void play_video(const Asset *x) {
    char *url = str_printf("%s/api/assets/%s/video/playback", im_url(), x->id);
    char *hdr = str_printf("--http-header-fields-append=x-api-key: %s", im_key());
    const char *extra[] = {hdr, NULL};
    PlayRequest rq = {0};
    rq.url = url;
    rq.title = x->label[0] ? x->label : "Immich";
    rq.extra = extra;
    player_run(&rq);
    explicit_bzero(hdr, strlen(hdr));
    free(hdr);
    free(url);
}

static void view_go(View *w, int d) {
    if (w->n <= 0) return;
    w->prev = w->i;
    w->i = (w->i + d + w->n) % w->n;
    w->changed_at = w->shown_at = app_time();
    w->fade = 0;
}

static void view_update(Screen *s, float dt) {
    View *w = s->st;
    w->fade = anim_to(w->fade, 1, dt, 5);
    if (w->show && app_time() - w->shown_at > 7) {
        int d = 1;
        while (d < w->n && w->a[(w->i + d) % w->n].video) d++; /* the slideshow skips videos */
        view_go(w, d);
    }
}

static void draw_asset(const Asset *x, float a) {
    char *u = asset_img(x->id);
    img_draw(img_get(u), 0, 0, UI_W, UI_H, 0, a);
    free(u);
}

static void view_draw(Screen *s) {
    View *w = s->st;
    fill_rect(0, 0, UI_W, UI_H, RGB(0, 0, 0));
    if (w->n <= 0) return;
    if (w->fade < 1 && w->prev != w->i) draw_asset(&w->a[w->prev], 1 - w->fade);
    draw_asset(&w->a[w->i], w->fade);
    /* fetch the next one already, so it is ready when it is its turn */
    char *nx = asset_img(w->a[(w->i + 1) % w->n].id);
    img_get(nx);
    free(nx);
    const Asset *x = &w->a[w->i];
    if (x->video) draw_icon(ICON_PLAY, UI_W / 2, UI_H / 2, 96, RGBA(255, 255, 255, 220));
    if (app_time() - w->changed_at < 4 && x->label[0]) { /* the date shows briefly after each change */
        float a = (float)(4 - (app_time() - w->changed_at));
        if (a > 1) a = 1;
        fill_rrect(60, UI_H - 110, text_width(F_SEMI, 28, x->label) + 48, 60, 30, RGBA(0, 0, 0, (uint8_t)(140 * a)));
        text_draw(F_SEMI, 28, 84, UI_H - 98, RGBA(255, 255, 255, (uint8_t)(230 * a)), AL_LEFT, x->label);
    }
}

static bool view_input(Screen *s, const InputEvent *ev) {
    View *w = s->st;
    switch (ev->a) {
    case A_LEFT: case A_REW: w->show = false; view_go(w, -1); return true;
    case A_RIGHT: case A_FF: w->show = false; view_go(w, 1); return true;
    case A_PLAY: case A_PLAYPAUSE: case A_PAUSE:
        w->show = !w->show;
        w->shown_at = app_time();
        app_toast(w->show ? T("Slideshow") : T("Slideshow stopped"));
        return true;
    case A_OK:
        if (w->a[w->i].video) play_video(&w->a[w->i]);
        else { w->show = !w->show; w->shown_at = app_time(); app_toast(w->show ? T("Slideshow") : T("Slideshow stopped")); }
        return true;
    case A_BACK: app_pop(); return true;
    default: return false;
    }
}

static void view_destroy(Screen *s) {
    View *w = s->st;
    free(w->a);
    free(w);
    free(s);
}

static Screen *view_screen(const Asset *a, int n, int i, bool show) {
    Screen *s = xcalloc(1, sizeof *s);
    View *w = xcalloc(1, sizeof *w);
    w->a = xcalloc((size_t)n, sizeof *a);
    memcpy(w->a, a, sizeof *a * (size_t)n);
    w->n = n;
    w->i = w->prev = i;
    w->show = show;
    w->changed_at = w->shown_at = app_time();
    s->name = "immich-view";
    s->st = w;
    s->update = view_update;
    s->draw = view_draw;
    s->input = view_input;
    s->destroy = view_destroy;
    return s;
}

/* ---------- grid ---------- */
static const char *tab_name(int t) {
    static const char *n[] = {N_("On this day"), N_("Recent"), N_("Albums"), N_("People")};
    return T(n[t]);
}
static int count(const Im *v) { return v->na ? v->na : v->nc; }
static float grid_y(const Im *v) { return v->top ? 220 : 160; }

static void im_update(Screen *s, float dt) {
    Im *v = s->st;
    int row = v->sel / COLS;
    float target = row > 1 ? (row - 1) * PITCH : 0;
    v->scroll = anim_to(v->scroll, target, dt, 12);
}

static void draw_login(Im *v) {
    static const char *btn[1];
    btn[0] = T("Sign in");
    if (draw_empty_ex)
        draw_empty_ex(ICON_STAR, RGB(120, 140, 255), T("Sign in to Immich"),
                      T("You need your Immich server's address and an API key (Immich → Account settings → API keys). Both can be typed from your phone."), btn, 1, v->loading ? -1 : 0);
    else draw_empty(T("Sign in to Immich"), T("Press OK to type the server address and an API key."));
    if (v->loading) draw_spinner(UI_W / 2, UI_H - 160, 24, C_TEXT2);
}

static void draw_tile_at(const Im *v, int i, float x, float y, bool f) {
    char *u = NULL;
    const char *t = NULL;
    char sub[64] = "";
    const void *key;
    if (v->na) {
        const Asset *a = &v->a[i];
        u = asset_img(a->id);
        t = a->label;
        key = a;
    } else {
        const Coll *c = &v->c[i];
        u = c->person ? person_img(c->id) : c->thumb[0] ? asset_img(c->thumb) : NULL;
        t = c->name;
        if (!c->person) snprintf(sub, sizeof sub, T("%d items"), c->count);
        key = c;
    }
    float fa = ui_focus(key, f), sc = 1 + 0.07f * fa, w = TILE * sc, xx = x - (w - TILE) / 2, yy = y - (w - TILE) / 2;
    bool round = !v->na && v->c[i].person;
    float r = round ? w / 2 : 14;
    ui_focus_under(xx, yy, w, w, r, fa);
    fill_rrect(xx, yy, w, w, r, C_SURFACE);
    if (u) img_draw_cover(img_get(u), xx, yy, w, w, r, 1);
    if (v->na && v->a[i].video) {
        fill_rrect(xx + 12, yy + 12, 44, 44, 22, RGBA(0, 0, 0, 150));
        draw_icon(ICON_PLAY, xx + 34, yy + 34, 22, RGB(255, 255, 255));
    }
    ui_focus_over(xx, yy, w, w, r, fa);
    free(u);
    if (t && *t) text_draw_clip(F_SEMI, 24, x, y + TILE + 14, TILE, f ? C_TEXT : C_TEXT2, AL_LEFT, t);
    if (sub[0]) text_draw_clip(F_REG, 20, x, y + TILE + 42, TILE, C_TEXT3, AL_LEFT, sub);
}

static void im_draw(Screen *s) {
    Im *v = s->st;
    draw_background();
    draw_topbar(v->top ? "Immich" : v->title);
    if (!signed_in()) { draw_login(v); return; }
    if (v->top) {
        const char *chips[T_COUNT];
        for (int i = 0; i < T_COUNT; i++) chips[i] = tab_name(i);
        draw_chips(96, 128, chips, T_COUNT, v->on_chips ? v->tab : -1);
    }
    float y0 = grid_y(v);
    int n = count(v);
    if (n == 0) {
        if (v->loading) { draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT); return; }
        if (v->failed) draw_empty(T("Cannot reach the Immich server"), T("Check that the server is on, or sign in again under Settings → Apps → Immich."));
        else if (v->top && v->tab == T_MEMORIES) draw_empty(T("Nothing from this day"), T("Photos taken on this date in earlier years show up here."));
        else draw_empty(T("Nothing here yet"), "");
        return;
    }
    if (v->loading) draw_spinner(UI_W - 60, 140, 14, C_TEXT2);
    clip_push(0, y0 - 30, UI_W, UI_H - y0 + 30);
    for (int i = 0; i < n; i++) {
        float x = 96 + (i % COLS) * (TILE + GAP), y = y0 + (i / COLS) * PITCH - v->scroll;
        if (y + PITCH < y0 - 30 || y > UI_H) continue;
        draw_tile_at(v, i, x, y, !v->on_chips && i == v->sel);
    }
    clip_pop();
}

static Screen *im_at(int kind, const char *id, const char *title);

static bool im_input(Screen *s, const InputEvent *ev) {
    Im *v = s->st;
    if (!signed_in()) {
        if (ev->a == A_OK && !v->loading) { start_login(v); return true; }
        if (ev->a == A_BACK || ev->a == A_LEFT) { app_pop(); return true; }
        return true;
    }
    int n = count(v);
    if (v->on_chips) {
        int t = v->tab;
        if (ev->a == A_DOWN) { if (n) v->on_chips = false; return true; }
        if (ev->a == A_LEFT && t == 0) { app_pop(); return true; }
        if (buttons_nav(&t, T_COUNT, ev)) { if (t != v->tab) { v->tab = t; load(v); } return true; }
        if (ev->a == A_OK) return true;
        if (ev->a == A_BACK) { app_pop(); return true; }
        return false;
    }
    switch (ev->a) {
    case A_UP:
        if (v->sel >= COLS) v->sel -= COLS;
        else if (v->top) v->on_chips = true;
        return true;
    case A_DOWN: if (v->sel + COLS < n) v->sel += COLS; else if (v->sel / COLS < (n - 1) / COLS) v->sel = n - 1; return true;
    case A_LEFT: if (v->sel % COLS) v->sel--; else if (v->top) v->on_chips = true; else app_pop(); return true;
    case A_RIGHT: if (v->sel + 1 < n && (v->sel + 1) % COLS) v->sel++; return true;
    case A_CHUP: v->sel = v->sel >= COLS * 3 ? v->sel - COLS * 3 : v->sel % COLS; return true;
    case A_CHDOWN: v->sel = v->sel + COLS * 3 < n ? v->sel + COLS * 3 : n - 1; return true;
    case A_OK: case A_PLAY: case A_PLAYPAUSE:
        if (n == 0) return true;
        if (v->na) {
            if (ev->a == A_OK && v->a[v->sel].video) play_video(&v->a[v->sel]);
            else app_push(view_screen(v->a, v->na, v->sel, ev->a != A_OK));
        } else {
            const Coll *c = &v->c[v->sel];
            app_push(im_at(c->person ? K_PERSON : K_ALBUM, c->id, c->name));
        }
        return true;
    case A_BACK:
        if (v->top && v->sel) { v->sel = 0; return true; }
        app_pop();
        return true;
    default: return false;
    }
}

static void im_enter(Screen *s) { live = s->st; if (count(live) == 0) load(live); }
static void im_resume(Screen *s) {
    Im *v = s->st;
    if (live == v) return;
    live = v;
    if (v->loading || (!count(v) && !v->failed)) load(v);
}
static void im_destroy(Screen *s) {
    Im *v = s->st;
    if (live == v) { live = NULL; load_gen++; }
    free(v->a);
    free(v->c);
    free(v);
    free(s);
}

static Screen *im_at(int kind, const char *id, const char *title) {
    Screen *s = xcalloc(1, sizeof *s);
    Im *v = xcalloc(1, sizeof *v);
    v->top = kind < 0;
    v->kind = kind;
    if (id) snprintf(v->coll_id, sizeof v->coll_id, "%s", id);
    if (title) snprintf(v->title, sizeof v->title, "%s", title);
    v->tab = T_MEMORIES;
    s->name = "immich";
    s->st = v;
    s->enter = im_enter;
    s->resume = im_resume;
    s->update = im_update;
    s->draw = im_draw;
    s->input = im_input;
    s->destroy = im_destroy;
    return s;
}

static Screen *immich_screen(void) {
    if (!draw_chips) { app_toast(T("Update NotaTV to use this app")); return NULL; } /* an older NotaTV */
    return im_at(-1, NULL, NULL);
}

/* ---------- photos for the screensaver (ImmichIface) ---------- */
typedef struct {
    char base[512], key[160];
    int n;
    char **urls;
    int nu;
    void (*cb)(void *ud, const char *const *urls, int n_urls);
    void *ud;
} RandJob;

static void rand_work(void *arg) {
    RandJob *j = arg;
    char body[96];
    snprintf(body, sizeof body, "{\"size\":%d,\"type\":\"IMAGE\"}", j->n);
    cJSON *r = im_call(j->base, j->key, "POST", "/api/search/random", body, NULL);
    char *k = url_encode(j->key);
    const cJSON *it;
    cJSON_ArrayForEach(it, r) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
        if (!id || j->nu >= j->n) continue;
        j->urls = xrealloc(j->urls, sizeof *j->urls * (j->nu + 1));
        j->urls[j->nu++] = str_printf("%s/api/assets/%s/thumbnail?size=preview&apiKey=%s", j->base, id, k);
    }
    free(k);
    cJSON_Delete(r);
}
static void rand_done(void *arg) {
    RandJob *j = arg;
    j->cb(j->ud, (const char *const *)j->urls, j->nu);
    for (int i = 0; i < j->nu; i++) free(j->urls[i]);
    free(j->urls);
    explicit_bzero(j->key, sizeof j->key);
    free(j);
}
static void random_photos(int n, void (*cb)(void *ud, const char *const *urls, int n_urls), void *ud) {
    if (!signed_in() || n <= 0) { cb(ud, NULL, 0); return; }
    RandJob *j = xcalloc(1, sizeof *j);
    snprintf(j->base, sizeof j->base, "%s", im_url());
    snprintf(j->key, sizeof j->key, "%s", im_key());
    j->n = n > 100 ? 100 : n;
    j->cb = cb;
    j->ud = ud;
    job_submit_q(JQ_SLOW, rand_work, rand_done, j);
}
static const ImmichIface im_iface = {signed_in, random_photos};

/* ---------- the app ---------- */
static void im_forget(void) {
    cfg_set("im_url", "");
    cfg_set("im_key", "");
    pending_url[0] = 0;
    load_gen++;
    img_cache_clear(); /* the photos leave memory too */
    if (live) { free(live->a); live->a = NULL; live->na = 0; free(live->c); live->c = NULL; live->nc = 0; }
}
static void act_logout(void) { app_ask_signout("Immich", im_forget); }
static const char *signed_out(void) { return signed_in() ? NULL : ITEM_HIDDEN; }
static const char *server_value(void) { return im_url(); }
static const Item im_settings[] = {
    {N_("Server"), IT_INFO, .value = server_value, .locked = signed_out},
    {N_("Sign out of Immich"), IT_ACTION, .action = act_logout, .locked = signed_out},
};

const NotaApp nota_app = {
    NOTA_API, "immich", "Immich", N_("Your photos and videos from your own Immich server"), APP_VERSION, 88, 0,
    ICON_STAR, {66, 80, 175, 255}, {30, 38, 110, 255}, {255, 255, 255, 255},
    .open = immich_screen, .forget = im_forget,
    .settings = im_settings, .n_settings = sizeof im_settings / sizeof im_settings[0],
    .iface = &im_iface,
};
