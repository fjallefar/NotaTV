/* NotaTV app package "cameras" - Cameras: live pictures from your cameras (Debat 13).
 * Sources: the cameras in Home Assistant (needs the Home Assistant app, through HomeAssistantIface: snapshots from
 * /api/camera_proxy/<id>?token=<the entity's access_token>, live from /api/camera_proxy_stream/...), and your own
 * RTSP addresses (snapshots with ffmpeg, a few seconds apart, only while the grid is open).
 * OK = full screen live in mpv. The doorbell: choose a Home Assistant sensor (binary_sensor) and a camera; when it
 * turns on, a small live picture shows in the corner for 30 s (also over other apps; during video a message). */
#define _GNU_SOURCE
#include "tv.h"
#include "vendor/cJSON.h"
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern const NotaApp nota_app;

#define MAXC 24
#define COLS 3
#define TW 552
#define TH 310
#define GAP 36
#define PITCH (TH + 80)

typedef struct {
    char name[96], id[160];   /* HA: entity id; RTSP: the address */
    char token[96];           /* HA: access_token (changes now and then - refreshed with the states) */
    bool ha;
    char shown[600], next[600]; /* snapshot shown, and the one loading (no blank flash between them) */
    double next_at;
} Cam;

typedef struct {
    Cam c[MAXC];
    int n, sel, chip;
    bool on_chips, loading;
    float scroll;
} Cams;

static Cams *live;
static const HomeAssistantIface *ha(void) {
    const HomeAssistantIface *h = app_iface("homeassistant");
    return h && h->signed_in() ? h : NULL;
}

/* own cameras: cfg cam_list = "name\x1furl\x1ename\x1furl..." */
static int own_cams(Cam *out, int max) {
    char *s = str_dup(cfg_get("cam_list", "")), *save = NULL;
    int n = 0;
    for (char *rec = strtok_r(s, "\x1e", &save); rec && n < max; rec = strtok_r(NULL, "\x1e", &save)) {
        char *us = strchr(rec, '\x1f');
        if (!us) continue;
        *us = 0;
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].name, sizeof out[n].name, "%s", rec);
        snprintf(out[n].id, sizeof out[n].id, "%s", us + 1);
        n++;
    }
    free(s);
    return n;
}
static void own_add(const char *name, const char *url) {
    char *v = str_printf("%s%s%s\x1f%s", cfg_get("cam_list", ""), *cfg_get("cam_list", "") ? "\x1e" : "", name, url);
    cfg_set("cam_list", v);
    free(v);
}
static void own_remove(const char *url) {
    Cam c[MAXC];
    int n = own_cams(c, MAXC);
    cfg_set("cam_list", "");
    for (int i = 0; i < n; i++) if (strcmp(c[i].id, url)) own_add(c[i].name, c[i].id);
}

/* ---------- loading the list (HA states) ---------- */
typedef struct { Cam c[MAXC]; int n; bool done; } ListJob;
static void list_work(void *arg) {
    ListJob *j = arg;
    const HomeAssistantIface *h = ha();
    long st = 0;
    char *r = h ? h->call("GET", "/api/states", NULL, &st) : NULL;
    cJSON *arr = r ? cJSON_Parse(r) : NULL;
    free(r);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "entity_id"));
        if (!id || strncmp(id, "camera.", 7) || j->n >= MAXC) continue;
        const cJSON *at = cJSON_GetObjectItem(it, "attributes");
        const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(at, "access_token"));
        const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(at, "friendly_name"));
        if (!tok) continue;
        Cam *c = &j->c[j->n++];
        memset(c, 0, sizeof *c);
        c->ha = true;
        snprintf(c->id, sizeof c->id, "%s", id);
        snprintf(c->token, sizeof c->token, "%s", tok);
        snprintf(c->name, sizeof c->name, "%s", fn && *fn ? fn : id + 7);
    }
    cJSON_Delete(arr);
}
static void list_done(void *arg) {
    ListJob *j = arg;
    Cams *v = live;
    if (v) {
        Cam own[MAXC];
        int no = own_cams(own, MAXC);
        Cam all[MAXC];
        int n = 0;
        for (int i = 0; i < j->n && n < MAXC; i++) all[n++] = j->c[i];
        for (int i = 0; i < no && n < MAXC; i++) all[n++] = own[i];
        /* keep the snapshots that were already shown for the same camera */
        for (int i = 0; i < n; i++)
            for (int k = 0; k < v->n; k++)
                if (!strcmp(all[i].id, v->c[k].id)) { memcpy(all[i].shown, v->c[k].shown, sizeof all[i].shown); all[i].next_at = v->c[k].next_at; }
        memcpy(v->c, all, sizeof all[0] * n);
        v->n = n;
        if (v->sel >= n) v->sel = n ? n - 1 : 0;
        v->loading = false;
    }
    explicit_bzero(j, sizeof *j);
    free(j);
}
static void load_list(Cams *v) {
    v->loading = true;
    job_submit_q(JQ_SLOW, list_work, list_done, xcalloc(1, sizeof(ListJob)));
}

/* ---------- snapshots ---------- */
static char *ha_snap(const Cam *c) {
    const HomeAssistantIface *h = ha();
    if (!h) return NULL;
    char *tok = url_encode(c->token);
    char *u = str_printf("%s/api/camera_proxy/%s?token=%s&n=%ld", h->base(), c->id, tok, (long)time(NULL));
    free(tok);
    return u;
}

typedef struct { char url[160], out[96]; int idx; } SnapJob;
static unsigned snap_n;
static void snap_work(void *arg) { /* RTSP: one frame with ffmpeg, small (the grid shows it 552 wide) */
    SnapJob *j = arg;
    char *q = shell_quote(j->url);
    run_cmd("ffmpeg -nostdin -loglevel error -rtsp_transport tcp -timeout 5000000 -i %s -frames:v 1 -vf scale=640:-2 -y %s "
            ">/dev/null 2>&1 || ffmpeg -nostdin -loglevel error -i %s -frames:v 1 -vf scale=640:-2 -y %s >/dev/null 2>&1",
            q, j->out, q, j->out);
    free(q);
}
static void snap_done(void *arg) {
    SnapJob *j = arg;
    Cams *v = live;
    if (v && j->idx < v->n && !strcmp(v->c[j->idx].id, j->url) && access(j->out, R_OK) == 0)
        snprintf(v->c[j->idx].next, sizeof v->c[j->idx].next, "%s", j->out);
    free(j);
}
static bool rtsp_busy;
static void rtsp_snap_done(void *arg) { rtsp_busy = false; snap_done(arg); }

static void refresh_snaps(Cams *v) {
    double now = app_time();
    for (int i = 0; i < v->n; i++) {
        Cam *c = &v->c[i];
        /* the loading one replaces the shown one once it is ready (or failed) */
        if (c->next[0]) {
            ImgState st = img_state(img_get(c->next));
            if (st == IMG_READY) {
                if (!c->ha && c->shown[0]) unlink(c->shown);
                memcpy(c->shown, c->next, sizeof c->shown);
                c->next[0] = 0;
            } else if (st == IMG_FAILED) c->next[0] = 0;
            continue;
        }
        if (now < c->next_at) continue;
        if (c->ha) {
            char *u = ha_snap(c);
            if (u) { snprintf(c->next, sizeof c->next, "%s", u); free(u); }
            c->next_at = now + 3;
        } else if (!rtsp_busy) { /* one ffmpeg at a time */
            rtsp_busy = true;
            SnapJob *j = xcalloc(1, sizeof *j);
            snprintf(j->url, sizeof j->url, "%s", c->id);
            snprintf(j->out, sizeof j->out, "/tmp/notatv-cam-%u.jpg", ++snap_n);
            j->idx = i;
            job_submit_q(JQ_SLOW, snap_work, rtsp_snap_done, j);
            c->next_at = now + 6;
        }
    }
}

/* ---------- live ---------- */
static void play_live(const Cam *c) {
    PlayRequest rq = {0};
    char *url = NULL;
    static const char *rtsp_extra[] = {"--profile=low-latency", "--rtsp-transport=tcp", "--untimed", "--cache=no", NULL};
    static const char *mjpeg_extra[] = {"--profile=low-latency", "--untimed", "--cache=no", "--demuxer-lavf-format=mpjpeg", NULL};
    if (c->ha) {
        const HomeAssistantIface *h = ha();
        if (!h) return;
        char *tok = url_encode(c->token);
        url = str_printf("%s/api/camera_proxy_stream/%s?token=%s", h->base(), c->id, tok);
        free(tok);
        rq.extra = mjpeg_extra;
    } else {
        url = str_dup(c->id);
        rq.extra = rtsp_extra;
    }
    rq.url = url;
    rq.title = c->name;
    rq.is_live = true;
    player_run(&rq);
    free(url);
}

/* ---------- adding an RTSP camera ---------- */
static char new_name[96];
static void url_done(void *ud, const char *text) {
    if (!text || !*text) return;
    char *t = str_dup(text);
    str_trim(t);
    if (!strstr(t, "://")) { char *u = str_printf("rtsp://%s", t); free(t); t = u; }
    if (strchr(t, '\x1e') || strchr(t, '\x1f')) { free(t); return; }
    own_add(new_name, t);
    free(t);
    if (live) load_list(live);
    app_toast(T("Camera added"));
}
static void name_done(void *ud, const char *text) {
    if (!text || !*text) return;
    snprintf(new_name, sizeof new_name, "%s", text);
    for (char *p = new_name; *p; p++) if (*p == '\x1e' || *p == '\x1f') *p = ' ';
    keyboard_open(T("RTSP address (rtsp://user:password@192.168.1.20:554/stream1)"), "rtsp://", false, url_done, NULL);
}

/* ---------- the doorbell ---------- */
typedef struct { char id[96], name[96], token[96]; } Opt;
static Opt bell_opts[64], cam_opts[MAXC];
static int n_bell, n_cam;
typedef struct { Opt b[64], c[MAXC]; int nb, nc; } OptJob;
static void opt_work(void *arg) {
    OptJob *j = arg;
    const HomeAssistantIface *h = ha();
    char *r = h ? h->call("GET", "/api/states", NULL, NULL) : NULL;
    cJSON *arr = r ? cJSON_Parse(r) : NULL;
    free(r);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "entity_id"));
        const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(it, "attributes"), "friendly_name"));
        if (!id || strlen(id) >= 96) continue;
        Opt *o = NULL;
        if (!strncmp(id, "binary_sensor.", 14) && j->nb < 64) o = &j->b[j->nb++];
        else if (!strncmp(id, "camera.", 7) && j->nc < MAXC) o = &j->c[j->nc++];
        if (!o) continue;
        snprintf(o->id, sizeof o->id, "%s", id);
        snprintf(o->name, sizeof o->name, "%s", fn && *fn ? fn : id);
    }
    cJSON_Delete(arr);
}
static void cam_chosen(void *ud, int idx) {
    if (idx < 0 || idx >= n_cam) return;
    cfg_set("cam_bell_cam", cam_opts[idx].id);
    app_toast(T("Doorbell set up"));
}
static void bell_chosen(void *ud, int idx) {
    if (idx < 0) return;
    if (idx == 0) { cfg_set("cam_bell", ""); app_toast(T("Doorbell turned off")); return; }
    cfg_set("cam_bell", bell_opts[idx - 1].id);
    static const char *items[MAXC];
    for (int i = 0; i < n_cam; i++) items[i] = cam_opts[i].name;
    if (n_cam) choice_open(T("Which camera shows who is there?"), items, n_cam, 0, cam_chosen, NULL);
}
static void opt_done(void *arg) {
    OptJob *j = arg;
    memcpy(bell_opts, j->b, sizeof bell_opts);
    memcpy(cam_opts, j->c, sizeof cam_opts);
    n_bell = j->nb;
    n_cam = j->nc;
    free(j);
    if (!n_cam) { app_toast(T("Home Assistant has no cameras")); return; }
    static const char *items[65];
    items[0] = T("No doorbell");
    for (int i = 0; i < n_bell; i++) items[i + 1] = bell_opts[i].name;
    choice_open(T("Doorbell: which sensor turns on when someone rings?"), items, n_bell + 1, 0, bell_chosen, NULL);
}
static void setup_bell(void) {
    if (!ha()) { app_toast(T("The doorbell needs the Home Assistant app, signed in")); return; }
    job_submit_q(JQ_SLOW, opt_work, opt_done, xcalloc(1, sizeof(OptJob)));
}

/* the corner picture: 30 s from the ring, a new snapshot every second */
static double rang_at = -100;
static char bell_shown[600], bell_next[600];
static double bell_next_at;
static void bell_overlay(void) {
    double age = app_time() - rang_at;
    if (age > 30) { app_set_overlay("cameras", NULL); bell_shown[0] = bell_next[0] = 0; return; }
    const HomeAssistantIface *h = ha();
    if (h && app_time() > bell_next_at && !bell_next[0]) {
        Cam c = {0};
        snprintf(c.id, sizeof c.id, "%s", cfg_get("cam_bell_cam", ""));
        for (int i = 0; i < n_cam; i++) if (!strcmp(cam_opts[i].id, c.id)) snprintf(c.token, sizeof c.token, "%s", cam_opts[i].token[0] ? cam_opts[i].token : "");
        char *u = ha_snap(&c);
        if (u) { snprintf(bell_next, sizeof bell_next, "%s", u); free(u); }
        bell_next_at = app_time() + 1;
    }
    if (bell_next[0]) {
        ImgState st = img_state(img_get(bell_next));
        if (st == IMG_READY) { memcpy(bell_shown, bell_next, sizeof bell_shown); bell_next[0] = 0; }
        else if (st == IMG_FAILED) bell_next[0] = 0;
    }
    float a = age < 0.4 ? (float)(age / 0.4) : age > 29.5 ? (float)((30 - age) / 0.5) : 1;
    float w = 640, hh = 360, x = UI_W - w - 60, y = UI_H - hh - 90 + (1 - a) * 40;
    draw_shadow(x, y, w, hh, 20, 30, a);
    fill_rrect(x, y, w, hh, 20, RGBA(20, 20, 24, (uint8_t)(255 * a)));
    if (bell_shown[0]) img_draw_cover(img_get(bell_shown), x, y, w, hh, 20, a);
    fill_rrect(x + 20, y + 20, text_width(F_SEMI, 24, T("Someone is at the door")) + 40, 48, 24, RGBA(0, 0, 0, (uint8_t)(160 * a)));
    text_draw(F_SEMI, 24, x + 40, y + 30, RGBA(255, 255, 255, (uint8_t)(255 * a)), AL_LEFT, T("Someone is at the door"));
}

/* the access token of the bell camera comes with the states; fetched again when it rings */
typedef struct { char bell[96], cam[96], state[16], token[96]; } BellJob;
static bool bell_polling;
static char bell_last[16];
static void bell_work(void *arg) {
    BellJob *j = arg;
    const HomeAssistantIface *h = ha();
    if (!h) return;
    char path[160];
    snprintf(path, sizeof path, "/api/states/%s", j->bell);
    char *r = h->call("GET", path, NULL, NULL);
    cJSON *o = r ? cJSON_Parse(r) : NULL;
    free(r);
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(o, "state"));
    if (st) snprintf(j->state, sizeof j->state, "%s", st);
    cJSON_Delete(o);
    if (!strcmp(j->state, "on")) { /* (st belonged to o, which is freed now) */
        snprintf(path, sizeof path, "/api/states/%s", j->cam);
        r = h->call("GET", path, NULL, NULL);
        o = r ? cJSON_Parse(r) : NULL;
        free(r);
        const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "attributes"), "access_token"));
        if (tok) snprintf(j->token, sizeof j->token, "%s", tok);
        cJSON_Delete(o);
    }
}
static void bell_done(void *arg) {
    BellJob *j = arg;
    bell_polling = false;
    bool rang = !strcmp(j->state, "on") && bell_last[0] && strcmp(bell_last, "on");
    if (j->state[0]) snprintf(bell_last, sizeof bell_last, "%s", j->state);
    if (rang) {
        SDL_Log("cameras: dørklokken ringede");
        n_cam = 1; /* the bell camera with its fresh token, for bell_overlay */
        snprintf(cam_opts[0].id, sizeof cam_opts[0].id, "%s", j->cam);
        snprintf(cam_opts[0].token, sizeof cam_opts[0].token, "%s", j->token);
        double pos, dur;
        bool paused;
        if (player_state(&pos, &dur, &paused)) { if (app_notify) app_notify(NULL, T("Someone is at the door")); }
        else { rang_at = app_time(); bell_next_at = 0; app_set_overlay("cameras", bell_overlay); }
    }
    free(j);
}
static void cam_tick(void) {
    static double next;
    if (!app_set_overlay) return; /* an older NotaTV: no doorbell picture */
    const char *bell = cfg_get("cam_bell", "");
    if (!*bell || app_time() < next || bell_polling || !ha()) return;
    next = app_time() + 2;
    bell_polling = true;
    BellJob *j = xcalloc(1, sizeof *j);
    snprintf(j->bell, sizeof j->bell, "%s", bell);
    snprintf(j->cam, sizeof j->cam, "%s", cfg_get("cam_bell_cam", ""));
    job_submit_q(JQ_SLOW, bell_work, bell_done, j);
}

/* ---------- the grid ---------- */
static const char *chip_label(int i) {
    static char bell[160];
    if (i == 0) return T("Add camera");
    const char *b = cfg_get("cam_bell", "");
    snprintf(bell, sizeof bell, "%s", *b ? T("Doorbell: on") : T("Doorbell: off"));
    return bell;
}

static void cams_draw(Screen *s) {
    Cams *v = s->st;
    draw_background();
    draw_topbar(T("Cameras"));
    const char *chips[2] = {chip_label(0), chip_label(1)};
    draw_chips(96, 128, chips, 2, v->on_chips ? v->chip : -1);
    if (!v->n) {
        if (v->loading) { draw_spinner(UI_W / 2, UI_H / 2, 30, C_TEXT); return; }
        draw_empty(T("No cameras yet"), T("Cameras from Home Assistant show up here by themselves, or add an RTSP camera with Add camera above."));
        return;
    }
    float y0 = 230;
    clip_push(0, y0 - 30, UI_W, UI_H - y0 + 30);
    for (int i = 0; i < v->n; i++) {
        const Cam *c = &v->c[i];
        float x = 96 + (i % COLS) * (TW + GAP), y = y0 + (i / COLS) * PITCH - v->scroll;
        if (y + PITCH < y0 - 40 || y > UI_H) continue;
        bool f = !v->on_chips && i == v->sel;
        float fa = ui_focus(c, f), sc = 1 + 0.05f * fa, w = TW * sc, h = TH * sc, xx = x - (w - TW) / 2, yy = y - (h - TH) / 2;
        ui_focus_under(xx, yy, w, h, 16, fa);
        fill_rrect(xx, yy, w, h, 16, C_SURFACE);
        if (c->shown[0]) img_draw_cover(img_get(c->shown), xx, yy, w, h, 16, 1);
        else draw_spinner(xx + w / 2, yy + h / 2, 18, C_TEXT3);
        fill_rrect(xx + 16, yy + 16, 70, 32, 16, RGBA(200, 30, 40, 220));
        text_draw(F_BOLD, 18, xx + 51, yy + 22, RGB(255, 255, 255), AL_CENTER, T("LIVE"));
        ui_focus_over(xx, yy, w, h, 16, fa);
        text_draw_clip(F_SEMI, 26, x, y + TH + 16, TW, f ? C_TEXT : C_TEXT2, AL_LEFT, c->name);
        text_draw_clip(F_REG, 20, x, y + TH + 48, TW, C_TEXT3, AL_LEFT, c->ha ? "Home Assistant" : "RTSP");
    }
    clip_pop();
    if (!v->on_chips && v->sel < v->n && !v->c[v->sel].ha) draw_keybar(96, UI_H - 62, NULL, NULL, NULL, NULL, T("Remove camera"));
}

static void cams_update(Screen *s, float dt) {
    Cams *v = s->st;
    if (live == v) refresh_snaps(v);
    int row = v->sel / COLS;
    v->scroll = anim_to(v->scroll, row > 1 ? (row - 1) * PITCH : 0, dt, 12);
}

static void remove_cb(void *ud, bool yes) {
    if (!yes || !live || live->sel >= live->n) return;
    own_remove(live->c[live->sel].id);
    load_list(live);
}

static bool cams_input(Screen *s, const InputEvent *ev) {
    Cams *v = s->st;
    if (v->on_chips) {
        int c = v->chip;
        if (buttons_nav(&c, 2, ev)) { v->chip = c; return true; }
        if (ev->a == A_LEFT && c == 0) { app_pop(); return true; }
        if (ev->a == A_DOWN) { if (v->n) v->on_chips = false; return true; }
        if (ev->a == A_OK) {
            if (v->chip == 0) keyboard_open(T("Camera name"), "", false, name_done, NULL);
            else setup_bell();
            return true;
        }
        if (ev->a == A_BACK) { app_pop(); return true; }
        return true;
    }
    int n = v->n;
    switch (ev->a) {
    case A_UP: if (v->sel >= COLS) v->sel -= COLS; else v->on_chips = true; return true;
    case A_DOWN:
        if (v->sel + COLS < n) { v->sel += COLS; return true; }
        if (v->sel / COLS < (n - 1) / COLS) { v->sel = n - 1; return true; }
        return false;
    case A_LEFT: if (v->sel % COLS) v->sel--; else v->on_chips = true; return true;
    case A_RIGHT: if (v->sel + 1 < n && (v->sel + 1) % COLS) v->sel++; return true;
    case A_OK: case A_PLAY: case A_PLAYPAUSE: if (n) play_live(&v->c[v->sel]); return true;
    case A_MENU:
        if (n && !v->c[v->sel].ha) {
            char t[160];
            snprintf(t, sizeof t, T("Remove %s?"), v->c[v->sel].name);
            confirm_open(t, T("The camera is removed from NotaTV (not from the camera itself)."), T("Remove"), T("Cancel"), remove_cb, NULL);
        }
        return true;
    case A_BACK: app_pop(); return true;
    default: return false;
    }
}

static void cams_enter(Screen *s) { live = s->st; load_list(live); }
static void cams_resume(Screen *s) { live = s->st; }
static void cams_destroy(Screen *s) {
    Cams *v = s->st;
    for (int i = 0; i < v->n; i++) { /* the RTSP snapshots in /tmp go too */
        if (!v->c[i].ha && v->c[i].shown[0]) unlink(v->c[i].shown);
        if (!v->c[i].ha && v->c[i].next[0]) unlink(v->c[i].next);
    }
    if (live == v) live = NULL;
    free(v);
    free(s);
}

static Screen *cams_screen(void) {
    if (!draw_chips || !app_set_overlay) { app_toast(T("Update NotaTV to use this app")); return NULL; } /* an older NotaTV */
    Screen *s = xcalloc(1, sizeof *s);
    s->name = "cameras";
    s->st = xcalloc(1, sizeof(Cams));
    s->enter = cams_enter;
    s->resume = cams_resume;
    s->update = cams_update;
    s->draw = cams_draw;
    s->input = cams_input;
    s->destroy = cams_destroy;
    return s;
}

static void cams_forget(void) {
    cfg_set("cam_list", "");
    cfg_set("cam_bell", "");
    cfg_set("cam_bell_cam", "");
    memset(cam_opts, 0, sizeof cam_opts);
    n_cam = 0;
    run_cmd("rm -f /tmp/notatv-cam-*.jpg");
}

const NotaApp nota_app = {
    NOTA_API, "cameras", N_("Cameras"), N_("Live pictures from your cameras and who is at the door"), APP_VERSION, 87, 0,
    ICON_PLAY, {70, 70, 80, 255}, {30, 30, 36, 255}, {255, 255, 255, 255},
    .open = cams_screen, .tick = cam_tick, .forget = cams_forget,
};
