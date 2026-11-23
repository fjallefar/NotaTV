/* NotaTV app package "homeassistant" - Home Assistant: see and control the home from the TV (Debat 9), and
 * notifications from Home Assistant on the TV (Debat 18).
 * Only the REST API with a long-lived access token (no open port on the box, no WebSocket):
 *   - GET /api/states every few seconds while the app is open; areas once via POST /api/template
 *   - POST /api/services/<domain>/<service> to switch things
 *   - notifications: the box looks at the helper input_text.notatv_notify every 5 s while installed; when an
 *     automation sets a new text, it is shown on the TV (app_notify)
 * Shown: lights, switches, fans, covers, climate, scenes, scripts, input_booleans (all can be used), and locks,
 * temperature/humidity sensors and door/window/motion sensors (only shown - a lock is never opened from the TV). */
#define _GNU_SOURCE
#include "tv.h"
#include "vendor/cJSON.h"
#include "json_util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const NotaApp nota_app;

#define MAXE 400
#define MAXA 40
#define COLS 4
#define CW 408
#define CH 150
#define GAP 32
#define PITCH (CH + GAP)

typedef enum { K_TOGGLE, K_SCENE, K_CLIMATE, K_SENSOR } Kind;
typedef struct {
    char id[96], name[96], state[48], unit[16], domain[24];
    Kind kind;
    int bright;            /* light: 0..255, -1 = not dimmable */
    double cur, target;    /* climate */
} Ent;
typedef struct { char name[64]; char (*ids)[96]; int n; } Area;

typedef struct {
    Ent *e;
    int ne;
    Area a[MAXA];
    int na;
    int chip, sel;         /* chip 0 = All, 1.. = areas */
    bool on_chips;
    float scroll;
    bool loading, failed, areas_done;
    double next_poll;
    int vis[MAXE], nvis;
} Ha;

static const char *ha_url(void) { return cfg_get("ha_url", ""); }
static const char *ha_token(void) { return cfg_get("ha_token", ""); }
static bool signed_in(void) { return *ha_url() && *ha_token(); }

/* ---------- REST ---------- */
static char *rest(const char *base, const char *token, const char *method, const char *path, const char *body, long *status) {
    char *url = str_printf("%s%s", base, path), *auth = str_printf("Authorization: Bearer %s", token);
    const char *hdr[] = {auth, "Content-Type: application/json", "Accept: application/json", NULL};
    HttpBuf b;
    memset(&b, 0, sizeof b);
    char *out = NULL;
    bool ok = http_request(method, url, hdr, body, &b, 15);
    if (status) *status = ok ? b.status : 0;
    if (ok && b.status >= 200 && b.status < 300 && b.data) { out = b.data; b.data = NULL; }
    http_free(&b);
    explicit_bzero(auth, strlen(auth));
    free(auth);
    free(url);
    return out;
}
static char *iface_call(const char *method, const char *path, const char *body, long *status) {
    if (!signed_in()) { if (status) *status = 0; return NULL; }
    return rest(ha_url(), ha_token(), method, path, body, status);
}

static bool shown_domain(const char *d) {
    static const char *ok[] = {"light", "switch", "fan", "cover", "climate", "scene", "script", "input_boolean", "lock",
                               "sensor", "binary_sensor", NULL};
    for (int i = 0; ok[i]; i++) if (!strcmp(d, ok[i])) return true;
    return false;
}

static bool parse_ent(const cJSON *o, Ent *e) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(o, "entity_id"));
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(o, "state"));
    if (!id || !st || strlen(id) >= sizeof e->id) return false;
    const char *dot = strchr(id, '.');
    if (!dot || (size_t)(dot - id) >= sizeof e->domain) return false;
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    snprintf(e->domain, sizeof e->domain, "%.*s", (int)(dot - id), id);
    if (!shown_domain(e->domain)) return false;
    const cJSON *at = cJSON_GetObjectItem(o, "attributes");
    const char *dc = cJSON_GetStringValue(cJSON_GetObjectItem(at, "device_class"));
    if (!strcmp(e->domain, "sensor") && !(dc && (!strcmp(dc, "temperature") || !strcmp(dc, "humidity")))) return false;
    if (!strcmp(e->domain, "binary_sensor") &&
        !(dc && (!strcmp(dc, "door") || !strcmp(dc, "window") || !strcmp(dc, "motion") || !strcmp(dc, "occupancy"))))
        return false;
    if (!strcmp(st, "unavailable") && (!strcmp(e->domain, "sensor") || !strcmp(e->domain, "binary_sensor"))) return false;
    const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(at, "friendly_name"));
    snprintf(e->name, sizeof e->name, "%s", fn && *fn ? fn : dot + 1);
    snprintf(e->state, sizeof e->state, "%s", st);
    const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(at, "unit_of_measurement"));
    if (u) snprintf(e->unit, sizeof e->unit, "%s", u);
    e->bright = -1;
    if (!strcmp(e->domain, "scene") || !strcmp(e->domain, "script")) e->kind = K_SCENE;
    else if (!strcmp(e->domain, "climate")) {
        e->kind = K_CLIMATE;
        e->cur = json_num(at, "current_temperature");
        e->target = json_num(at, "temperature");
    } else if (!strcmp(e->domain, "sensor") || !strcmp(e->domain, "binary_sensor") || !strcmp(e->domain, "lock")) e->kind = K_SENSOR;
    else e->kind = K_TOGGLE;
    if (!strcmp(e->domain, "light")) {
        const cJSON *modes = cJSON_GetObjectItem(at, "supported_color_modes"), *m;
        bool dim = false;
        cJSON_ArrayForEach(m, modes) if (cJSON_IsString(m) && strcmp(m->valuestring, "onoff")) dim = true;
        if (dim) e->bright = (int)json_num(at, "brightness");
    }
    return true;
}

static int ent_cmp(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    if (x->kind != y->kind) return x->kind - y->kind; /* things you switch first, sensors last */
    return strcasecmp(x->name, y->name);
}

typedef struct {
    unsigned gen;
    char base[512], token[400];
    bool login, areas;
    Ent *e;
    int ne;
    Area a[MAXA];
    int na;
    bool failed;
    long status;
} Job;

static void job_work(void *arg) {
    Job *j = arg;
    if (j->login) {
        char *r = rest(j->base, j->token, "GET", "/api/", NULL, &j->status);
        j->failed = !r;
        free(r);
        return;
    }
    char *r = rest(j->base, j->token, "GET", "/api/states", NULL, &j->status);
    cJSON *arr = r ? cJSON_Parse(r) : NULL;
    free(r);
    j->failed = !arr;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (j->ne >= MAXE) break;
        Ent e;
        if (!parse_ent(it, &e)) continue;
        j->e = xrealloc(j->e, sizeof *j->e * (j->ne + 1));
        j->e[j->ne++] = e;
    }
    cJSON_Delete(arr);
    if (j->ne > 1) qsort(j->e, j->ne, sizeof *j->e, ent_cmp);
    if (!j->areas || j->failed) return;
    /* areas with their entities - also those that belong to the area through their device */
    const char *tpl = "{\"template\":\"{% for a in areas() %}{{ area_name(a) }}\\t{{ (area_entities(a) + "
                      "(area_devices(a) | map('device_entities') | sum(start=[]))) | unique | join(',') }}\\n{% endfor %}\"}";
    char *t = rest(j->base, j->token, "POST", "/api/template", tpl, NULL);
    for (char *line = t ? strtok(t, "\n") : NULL; line && j->na < MAXA; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        Area *a = &j->a[j->na];
        memset(a, 0, sizeof *a);
        snprintf(a->name, sizeof a->name, "%s", line);
        char *save = NULL;
        for (char *id = strtok_r(tab + 1, ",", &save); id; id = strtok_r(NULL, ",", &save)) {
            a->ids = xrealloc(a->ids, sizeof *a->ids * (a->n + 1));
            snprintf(a->ids[a->n++], sizeof a->ids[0], "%s", id);
        }
        if (a->n) j->na++;
        else free(a->ids);
    }
    free(t);
}

static Ha *live;
static unsigned load_gen;
static void rebuild_visible(Ha *v);

static void free_areas(Area *a, int n) { for (int i = 0; i < n; i++) free(a[i].ids); }

static void job_done(void *arg) {
    Job *j = arg;
    Ha *v = live;
    if (v && j->gen == load_gen) {
        v->loading = false;
        if (j->login) {
            if (j->failed) app_toast(j->status == 401 ? T("The access token was not accepted") : T("Cannot reach Home Assistant"));
            else {
                cfg_set("ha_url", j->base);
                cfg_set("ha_token", j->token);
                v->next_poll = 0;
            }
        } else {
            v->failed = j->failed;
            if (!j->failed) {
                free(v->e);
                v->e = j->e; v->ne = j->ne; j->e = NULL;
                if (j->areas) {
                    free_areas(v->a, v->na);
                    memcpy(v->a, j->a, sizeof j->a);
                    v->na = j->na;
                    j->na = 0;
                    v->areas_done = true;
                }
                rebuild_visible(v);
            }
        }
    }
    free_areas(j->a, j->na);
    free(j->e);
    explicit_bzero(j->token, sizeof j->token);
    free(j);
}

static void poll_states(Ha *v) {
    Job *j = xcalloc(1, sizeof *j);
    j->gen = ++load_gen;
    snprintf(j->base, sizeof j->base, "%s", ha_url());
    snprintf(j->token, sizeof j->token, "%s", ha_token());
    j->areas = !v->areas_done;
    if (!v->ne) v->loading = true;
    job_submit_q(JQ_SLOW, job_work, job_done, j);
    v->next_poll = app_time() + 4;
}

static void rebuild_visible(Ha *v) {
    v->nvis = 0;
    const Area *a = v->chip > 0 && v->chip <= v->na ? &v->a[v->chip - 1] : NULL;
    for (int i = 0; i < v->ne && v->nvis < MAXE; i++) {
        if (a) {
            bool in = false;
            for (int k = 0; k < a->n && !in; k++) in = !strcmp(a->ids[k], v->e[i].id);
            if (!in) continue;
        }
        v->vis[v->nvis++] = i;
    }
    if (v->sel >= v->nvis) v->sel = v->nvis ? v->nvis - 1 : 0;
}

/* ---------- switching ---------- */
typedef struct { char base[512], token[400], path[128], body[256]; } Svc;
static void svc_work(void *arg) {
    Svc *s = arg;
    long st = 0;
    char *r = rest(s->base, s->token, "POST", s->path, s->body, &st);
    if (!r) SDL_Log("homeassistant: %s gav %ld", s->path, st);
    free(r);
}
static void svc_done(void *arg) {
    Svc *s = arg;
    explicit_bzero(s->token, sizeof s->token);
    free(s);
    if (live) live->next_poll = app_time() + 0.6; /* see the result soon */
}
static void call_service(const char *domain, const char *service, const char *entity, const char *extra) {
    Svc *s = xcalloc(1, sizeof *s);
    snprintf(s->base, sizeof s->base, "%s", ha_url());
    snprintf(s->token, sizeof s->token, "%s", ha_token());
    snprintf(s->path, sizeof s->path, "/api/services/%s/%s", domain, service);
    snprintf(s->body, sizeof s->body, "{\"entity_id\":\"%s\"%s%s}", entity, extra ? "," : "", extra ? extra : "");
    job_submit_q(JQ_FAST, svc_work, svc_done, s);
}

static bool is_on(const Ent *e) {
    return !strcmp(e->state, "on") || !strcmp(e->state, "open") || !strcmp(e->state, "unlocked") ||
           !strcmp(e->state, "heat") || !strcmp(e->state, "cool") || !strcmp(e->state, "heat_cool");
}

static char pending_id[96];
static void bright_done(void *ud, int idx) {
    if (idx < 0 || !live) return;
    char extra[48];
    if (idx == 0) { call_service("light", "turn_off", pending_id, NULL); return; }
    snprintf(extra, sizeof extra, "\"brightness_pct\":%d", idx * 10);
    call_service("light", "turn_on", pending_id, extra);
}
static void ask_brightness(const Ent *e) {
    static char labels[11][16];
    static const char *items[11];
    for (int i = 0; i <= 10; i++) {
        if (i == 0) snprintf(labels[i], sizeof labels[i], "%s", T("Off"));
        else snprintf(labels[i], sizeof labels[i], "%d %%", i * 10);
        items[i] = labels[i];
    }
    snprintf(pending_id, sizeof pending_id, "%s", e->id);
    int cur = is_on(e) && e->bright >= 0 ? (e->bright * 10 + 127) / 255 : 0;
    choice_open(e->name, items, 11, cur, bright_done, NULL);
}

static double temp_opts[25];
static void temp_done(void *ud, int idx) {
    if (idx < 0) return;
    char extra[48];
    snprintf(extra, sizeof extra, "\"temperature\":%.1f", temp_opts[idx]);
    call_service("climate", "set_temperature", pending_id, extra);
}
static void ask_temp(const Ent *e) {
    static char labels[25][16];
    static const char *items[25];
    int cur = 0;
    for (int i = 0; i < 25; i++) {
        temp_opts[i] = 15 + i * 0.5;
        snprintf(labels[i], sizeof labels[i], "%.1f °", temp_opts[i]);
        items[i] = labels[i];
        if (e->target > 0 && temp_opts[i] <= e->target + 0.01) cur = i;
    }
    snprintf(pending_id, sizeof pending_id, "%s", e->id);
    choice_open(e->name, items, 25, cur, temp_done, NULL);
}

static void activate(Ha *v, Ent *e) {
    switch (e->kind) {
    case K_TOGGLE:
        call_service(!strcmp(e->domain, "cover") ? "cover" : e->domain, "toggle", e->id, NULL);
        /* show the change at once; the next poll says what really happened */
        snprintf(e->state, sizeof e->state, "%s", is_on(e) ? (!strcmp(e->domain, "cover") ? "closed" : "off") :
                                                              (!strcmp(e->domain, "cover") ? "open" : "on"));
        break;
    case K_SCENE:
        call_service(e->domain, "turn_on", e->id, NULL);
        app_toast(T("%s started"), e->name);
        break;
    case K_CLIMATE: ask_temp(e); break;
    case K_SENSOR: break;
    }
}

/* ---------- sign in ---------- */
static char pending_url[512];
static void token_done(void *ud, const char *text) {
    Ha *v = ud;
    if (!text || !*text || v != live) return;
    Job *j = xcalloc(1, sizeof *j);
    j->gen = ++load_gen;
    j->login = true;
    snprintf(j->base, sizeof j->base, "%s", pending_url);
    snprintf(j->token, sizeof j->token, "%s", text);
    str_trim(j->token);
    v->loading = true;
    job_submit_q(JQ_SLOW, job_work, job_done, j);
}
static void url_done(void *ud, const char *text) {
    Ha *v = ud;
    if (!text || !*text || v != live) return;
    char *t = str_dup(text);
    str_trim(t);
    size_t n = strlen(t);
    while (n && t[n - 1] == '/') t[--n] = 0;
    if (strstr(t, "://")) snprintf(pending_url, sizeof pending_url, "%s", t);
    else snprintf(pending_url, sizeof pending_url, "http://%s%s", t, strchr(t, ':') ? "" : ":8123");
    free(t);
    keyboard_open(T("Long-lived access token (your profile in Home Assistant → Security)"), "", true, token_done, v);
}
static void start_login(Ha *v) {
    keyboard_open(T("Home Assistant address (e.g. homeassistant.local:8123)"), *ha_url() ? ha_url() : "", false, url_done, v);
}

/* ---------- drawing ---------- */
static void state_text(const Ent *e, char *out, size_t n) {
    if (e->kind == K_CLIMATE) {
        if (e->target > 0) snprintf(out, n, T("%.1f° now · set to %.1f°"), e->cur, e->target);
        else snprintf(out, n, T("%.1f° now"), e->cur);
    } else if (!strcmp(e->domain, "sensor")) snprintf(out, n, "%s %s", e->state, e->unit);
    else if (!strcmp(e->domain, "binary_sensor")) snprintf(out, n, "%s", !strcmp(e->state, "on") ? T("Open / active") : T("Closed / quiet"));
    else if (!strcmp(e->domain, "lock")) snprintf(out, n, "%s", !strcmp(e->state, "locked") ? T("Locked") : T("Unlocked"));
    else if (e->kind == K_SCENE) snprintf(out, n, "%s", T("Press OK to start"));
    else if (!strcmp(e->state, "unavailable")) snprintf(out, n, "%s", T("Not available"));
    else if (!strcmp(e->domain, "light") && is_on(e) && e->bright > 0) snprintf(out, n, T("On · %d %%"), (e->bright * 100 + 127) / 255);
    else if (!strcmp(e->domain, "cover")) snprintf(out, n, "%s", is_on(e) ? T("Open") : T("Closed"));
    else snprintf(out, n, "%s", is_on(e) ? T("On") : T("Off"));
}

static void draw_card(const Ent *e, float x, float y, bool f) {
    float fa = ui_focus(e, f), sc = 1 + 0.05f * fa, w = CW * sc, h = CH * sc, xx = x - (w - CW) / 2, yy = y - (h - CH) / 2;
    bool on = e->kind == K_TOGGLE && is_on(e);
    ui_focus_under(xx, yy, w, h, 18, fa);
    Color bg = on ? (!strcmp(e->domain, "light") ? RGB(250, 200, 90) : RGB(90, 160, 255)) : C_SURFACE;
    fill_rrect(xx, yy, w, h, 18, bg);
    Color tc = on ? RGB(20, 20, 20) : C_TEXT, sc2 = on ? RGB(50, 50, 50) : C_TEXT3;
    text_draw_clip(F_SEMI, 26, xx + 24, yy + 28, w - 48 - (e->kind == K_TOGGLE ? 70 : 0), tc, AL_LEFT, e->name);
    char st[96];
    state_text(e, st, sizeof st);
    text_draw_clip(F_REG, 22, xx + 24, yy + h - 52, w - 48, sc2, AL_LEFT, st);
    if (e->kind == K_TOGGLE) { /* a small switch at the top right */
        float sx = xx + w - 84, sy = yy + 30;
        fill_rrect(sx, sy, 56, 30, 15, on ? RGBA(0, 0, 0, 60) : C_SURFACE2);
        fill_rrect(on ? sx + 28 : sx + 2, sy + 2, 26, 26, 13, on ? RGB(255, 255, 255) : RGB(150, 150, 155));
    }
    ui_focus_over(xx, yy, w, h, 18, fa);
}

static void ha_draw(Screen *s) {
    Ha *v = s->st;
    draw_background();
    draw_topbar("Home Assistant");
    if (!signed_in()) {
        static const char *btn[1];
        btn[0] = T("Sign in");
        if (draw_empty_ex)
            draw_empty_ex(ICON_TV, RGB(65, 189, 245), T("Connect Home Assistant"),
                          T("You need the address of your Home Assistant and a long-lived access token (your profile → Security). Both can be typed from your phone."),
                          btn, 1, v->loading ? -1 : 0);
        else draw_empty(T("Connect Home Assistant"), T("Press OK to type the address and an access token."));
        if (v->loading) draw_spinner(UI_W / 2, UI_H - 160, 24, C_TEXT2);
        return;
    }
    const char *chips[MAXA + 1];
    chips[0] = T("All");
    for (int i = 0; i < v->na; i++) chips[i + 1] = v->a[i].name;
    draw_chips(96, 128, chips, v->na + 1, v->on_chips ? v->chip : -1);
    if (!v->nvis) {
        if (v->loading || (!v->ne && !v->failed)) draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT);
        else if (v->failed) draw_empty(T("Cannot reach Home Assistant"), T("Check that it is on, or sign in again under Settings → Apps → Home Assistant."));
        else draw_empty(T("Nothing to show here"), T("Lights, switches, climate, scenes and a few sensors show up here."));
        return;
    }
    float y0 = 230;
    clip_push(0, y0 - 30, UI_W, UI_H - y0 - 70);
    for (int i = 0; i < v->nvis; i++) {
        float x = 96 + (i % COLS) * (CW + GAP), y = y0 + (i / COLS) * PITCH - v->scroll;
        if (y + PITCH < y0 - 40 || y > UI_H) continue;
        draw_card(&v->e[v->vis[i]], x, y, !v->on_chips && i == v->sel && !keybar_focused());
    }
    clip_pop();
    const Ent *fe = !v->on_chips && v->sel < v->nvis ? &v->e[v->vis[v->sel]] : NULL;
    bool dim = fe && !strcmp(fe->domain, "light") && fe->bright >= 0;
    draw_keybar(96, UI_H - 62, NULL, NULL, NULL, NULL, dim ? T("Brightness") : NULL);
}

static void ha_update(Screen *s, float dt) {
    Ha *v = s->st;
    if (live == v && signed_in() && app_time() >= v->next_poll) poll_states(v);
    int row = v->sel / COLS;
    float target = row > 2 ? (row - 2) * PITCH : 0;
    v->scroll = anim_to(v->scroll, target, dt, 12);
}

static bool ha_input(Screen *s, const InputEvent *ev) {
    Ha *v = s->st;
    if (!signed_in()) {
        if (ev->a == A_OK && !v->loading) start_login(v);
        else if (ev->a == A_BACK || ev->a == A_LEFT) app_pop();
        return true;
    }
    if (v->on_chips) {
        int c = v->chip;
        if (ev->a == A_DOWN) { if (v->nvis) v->on_chips = false; return true; }
        if (ev->a == A_LEFT && c == 0) { app_pop(); return true; }
        if (buttons_nav(&c, v->na + 1, ev)) { if (c != v->chip) { v->chip = c; v->sel = 0; rebuild_visible(v); } return true; }
        if (ev->a == A_BACK) { app_pop(); return true; }
        return true;
    }
    int n = v->nvis;
    Ent *e = n ? &v->e[v->vis[v->sel]] : NULL;
    switch (ev->a) {
    case A_UP: if (v->sel >= COLS) v->sel -= COLS; else v->on_chips = true; return true;
    case A_DOWN:
        if (v->sel + COLS < n) { v->sel += COLS; return true; }
        if (v->sel / COLS < (n - 1) / COLS) { v->sel = n - 1; return true; }
        return false; /* below the last row: the key bar */
    case A_LEFT: if (v->sel % COLS) v->sel--; else v->on_chips = true; return true;
    case A_RIGHT: if (v->sel + 1 < n && (v->sel + 1) % COLS) v->sel++; return true;
    case A_OK: if (e) activate(v, e); return true;
    case A_MENU: if (e && !strcmp(e->domain, "light") && e->bright >= 0) ask_brightness(e); return true;
    case A_BACK: app_pop(); return true;
    default: return false;
    }
}

static void ha_enter(Screen *s) { live = s->st; live->next_poll = 0; }
static void ha_resume(Screen *s) { live = s->st; live->next_poll = 0; }
static void ha_destroy(Screen *s) {
    Ha *v = s->st;
    if (live == v) { live = NULL; load_gen++; }
    free(v->e);
    free_areas(v->a, v->na);
    free(v);
    free(s);
}

static Screen *ha_screen(void) {
    if (!draw_chips) { app_toast(T("Update NotaTV to use this app")); return NULL; } /* an older NotaTV */
    Screen *s = xcalloc(1, sizeof *s);
    Ha *v = xcalloc(1, sizeof *v);
    s->name = "homeassistant";
    s->st = v;
    s->enter = ha_enter;
    s->resume = ha_resume;
    s->update = ha_update;
    s->draw = ha_draw;
    s->input = ha_input;
    s->destroy = ha_destroy;
    return s;
}

/* ---------- notifications: input_text.notatv_notify ---------- */
static pthread_mutex_t nmu = PTHREAD_MUTEX_INITIALIZER;
static bool npolling;
static char seen_changed[64];
static double next_note;
typedef struct { char base[512], token[400], text[256], changed[64]; bool ok; } NoteJob;
static void note_work(void *arg) {
    NoteJob *j = arg;
    char *r = rest(j->base, j->token, "GET", "/api/states/input_text.notatv_notify", NULL, NULL);
    cJSON *o = r ? cJSON_Parse(r) : NULL;
    free(r);
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(o, "state"));
    const char *ch = cJSON_GetStringValue(cJSON_GetObjectItem(o, "last_changed"));
    if (st && ch) {
        j->ok = true;
        snprintf(j->text, sizeof j->text, "%s", st);
        snprintf(j->changed, sizeof j->changed, "%s", ch);
    }
    cJSON_Delete(o);
}
static void note_done(void *arg) {
    NoteJob *j = arg;
    pthread_mutex_lock(&nmu);
    npolling = false;
    pthread_mutex_unlock(&nmu);
    if (j->ok && strcmp(j->changed, seen_changed)) {
        bool first = !seen_changed[0]; /* what was there when the box started is old news */
        snprintf(seen_changed, sizeof seen_changed, "%s", j->changed);
        if (!first && j->text[0] && strcmp(j->text, "unknown") && app_notify) app_notify("Home Assistant", j->text);
    }
    explicit_bzero(j->token, sizeof j->token);
    free(j);
}
static void ha_tick(void) {
    if (!signed_in() || !cfg_int("ha_notify", 1) || app_time() < next_note) return;
    next_note = app_time() + 5;
    if (npolling) return;
    npolling = true;
    NoteJob *j = xcalloc(1, sizeof *j);
    snprintf(j->base, sizeof j->base, "%s", ha_url());
    snprintf(j->token, sizeof j->token, "%s", ha_token());
    job_submit_q(JQ_SLOW, note_work, note_done, j);
}

/* ---------- the app ---------- */
static const HomeAssistantIface ha_iface = {signed_in, iface_call, ha_url};

static void ha_forget(void) {
    cfg_set("ha_url", "");
    cfg_set("ha_token", "");
    pending_url[0] = 0;
    seen_changed[0] = 0;
    load_gen++;
    if (live) {
        free(live->e); live->e = NULL; live->ne = 0; live->nvis = 0;
        free_areas(live->a, live->na); live->na = 0; live->areas_done = false;
    }
}
static void act_logout(void) { app_ask_signout("Home Assistant", ha_forget); }
static const char *signed_out(void) { return signed_in() ? NULL : ITEM_HIDDEN; }
static const char *server_value(void) { return ha_url(); }
static const Item ha_settings[] = {
    {N_("Server"), IT_INFO, .value = server_value, .locked = signed_out},
    {N_("Notifications on the TV"), IT_TOGGLE, "ha_notify", "1", set_onoff, .locked = signed_out,
     .help = N_("An automation sets the helper input_text.notatv_notify to a text, and the TV shows it")},
    {N_("Sign out of Home Assistant"), IT_ACTION, .action = act_logout, .locked = signed_out},
};

const NotaApp nota_app = {
    NOTA_API, "homeassistant", "Home Assistant", N_("Lights, heat and scenes in your home - and its messages on the TV"), APP_VERSION, 86, 0,
    ICON_TV, {65, 189, 245, 255}, {20, 110, 180, 255}, {255, 255, 255, 255},
    .open = ha_screen, .tick = ha_tick, .forget = ha_forget,
    .settings = ha_settings, .n_settings = sizeof ha_settings / sizeof ha_settings[0],
    .iface = &ha_iface,
};
