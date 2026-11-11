/* NotaTV shell - the control center: slides down at the top right (the button by the clock on the home screen, Red, or the Power key).
   Sleep timer, shortcuts into Settings, and sleep / restart / power off. */
#include "tv.h"
#include "settings.h"
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void home_begin_edit(void); /* ui_home.c */

/* the sleep timer: the box goes to sleep at this time (0 = off). Checked by quick_tick every frame */
static double sleep_at;
static bool sleep_warned;
static const int sleep_mins[] = {0, 15, 30, 60, 90, 120};
#define N_SLEEP (int)(sizeof sleep_mins / sizeof sleep_mins[0])

/* soft sleep ("power sleep", /run/notatv/sleeping): the box stays on, the TV is off. A key press
 * wakes it: clear the flag and answer the TV as active source again (the flag itself is only an
 * agreement with cecd - the shell is what sees the key presses). Fires when the flag is set AND a
 * key has been pressed after the sleep began - never just because the flag exists. */
void soft_sleep_wake(void) {
    static double seen_at; /* when the flag was first seen set (0 = not sleeping) */
    static double last_activity;
    static double next_check;
    bool s = access("/run/notatv/sleeping", F_OK) == 0;
    double now = app_time(), act = input_last_activity();
    if (s) {
        if (!seen_at) {
            seen_at = now;
            last_activity = act; /* the key that may already have arrived counts as before the sleep */
        } else if (now - seen_at < 3) {
            /* "power sleep" itself sends Back through the inbox to stop playback, and that is a key press too
               (input_push): what happens in the first seconds is part of going to sleep, not a wake-up */
            last_activity = act;
        } else if (act > last_activity && now >= next_check) {
            seen_at = 0;
            next_check = now + 2; /* one power on per event, not one per key repeat */
            run_cmd("%s/power on &", helper_path());
        }
    } else {
        seen_at = 0;
        last_activity = act;
    }
}

void quick_tick(void) {
    if (sleep_at <= 0) return;
    double left = sleep_at - app_time();
    if (left <= 60 && !sleep_warned) { sleep_warned = true; app_toast("%s", T("The box goes to sleep in 1 minute")); }
    if (left <= 0) {
        sleep_at = 0;
        run_cmd("%s/power sleep &", helper_path());
    }
}

/* automatic sleep (cfg auto_sleep in hours, default 4, 0 = off): after that long without a key press the box warns,
   and goes to sleep a minute later unless a key is pressed. Not while music or radio plays: then the sound card is in
   use while no video plays (AirPlay, Bluetooth, Radio) - they have the sleep timer. Checked every 5 seconds. */
static bool sound_busy(void) {
    glob_t g;
    bool busy = false;
    if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && !busy; i++) {
            char *s = file_read_all(g.gl_pathv[i], 4096);
            if (s && strstr(s, "RUNNING")) busy = true;
            free(s);
        }
        globfree(&g);
    }
    return busy;
}
void idle_sleep_tick(bool playing) {
    static double next_check, slept_at;
    static bool warned;
    double now = app_time();
    if (now < next_check) return;
    next_check = now + 5;
    int hours = cfg_int("auto_sleep", 4);
    if (hours <= 0) { warned = false; return; }
    double idle = now - fmax(fmax(input_last_activity(), 0), slept_at), limit = hours * 3600.0;
    if (idle < limit - 60) { warned = false; return; }
    if (!playing && sound_busy()) { input_touch(); return; } /* music or radio: counts as being used */
    if (!warned) {
        warned = true;
        char m[240];
        snprintf(m, sizeof m, T("No key pressed for %d hours - the box goes to sleep in 1 minute. Press a key to keep watching."), hours);
        if (playing) player_osd(m, 20000);
        else app_toast("%s", m);
        return;
    }
    if (idle >= limit) {
        warned = false;
        slept_at = now; /* not again right after waking up */
        SDL_Log("automatisk dvale efter %d timer uden tastetryk", hours);
        run_cmd("%s/power sleep &", helper_path());
    }
}

typedef enum { Q_TIMER, Q_BT, Q_AV, Q_NET, Q_EDIT, Q_ALL, Q_SLEEP, Q_RESTART, Q_OFF, Q_N } QItem;
typedef struct { int sel; bool closing, from_home; float out; } Quick;

static const char *q_label(QItem i) {
    static const char *l[Q_N] = {N_("Sleep timer"), "Bluetooth", N_("Picture"), N_("Network"), N_("Edit home screen"),
                                 N_("All settings"), N_("Sleep"), N_("Restart"), N_("Power off")};
    return T(l[i]);
}
static IconId q_icon(QItem i) {
    static const IconId ic[Q_N] = {ICON_CLOCK, ICON_MUSIC, ICON_FILM, ICON_WIFI, ICON_TV, ICON_GEAR, ICON_MOON, ICON_RESTART, ICON_POWER};
    return ic[i];
}
#define PW 640.0f
#define PAD 28.0f
#define GAP 16.0f
#define TILE_TOP 136.0f
#define BIG_W ((PW - 2 * PAD - GAP) / 2)
#define BIG_H 112.0f
#define SMALL_W ((PW - 2 * PAD - 2 * GAP) / 3)
#define SMALL_H 96.0f
#define POWER_Y (TILE_TOP + 3 * (BIG_H + GAP) + 44)
#define PH (POWER_Y + SMALL_H + PAD)
typedef struct { float x, y, w, h; } QRect;
/* where tile i is, relative to the panel. "Edit home screen" exists only when the panel was opened from the home screen */
static bool q_visible(const Quick *q, int i) { return i != Q_EDIT || q->from_home; }
static QRect q_rect(const Quick *q, int i) {
    if (i >= Q_SLEEP) return (QRect){PAD + (i - Q_SLEEP) * (SMALL_W + GAP), POWER_Y, SMALL_W, SMALL_H};
    if (i == Q_ALL && !q->from_home) return (QRect){PAD, TILE_TOP + 2 * (BIG_H + GAP), PW - 2 * PAD, BIG_H};
    return (QRect){PAD + (i % 2) * (BIG_W + GAP), TILE_TOP + (i / 2) * (BIG_H + GAP), BIG_W, BIG_H};
}

static void quick_update(Screen *s, float dt) {
    Quick *q = s->st;
    if (q->closing) {
        q->out = anim_to(q->out, 1, dt, 14);
        if (q->out > 0.98f) app_close(s);
    }
}

static void quick_draw(Screen *s) {
    Quick *q = s->st;
    float e = s->anim * (1 - q->out);
    float k = 1 - (1 - e) * (1 - e); /* ease out */
    fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(130 * e)));
    float x = UI_W - PW - 24, y = 24 - (1 - k) * (PH + 40);
    draw_panel(x, y, PW, PH, 36, 1);
    /* the time, big, and what the box is called */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof clk, "%H:%M", &tm);
    text_draw(F_BOLD, 48, x + PAD + 8, y + 24, C_TEXT, AL_LEFT, clk);
    const char *ip = app_ip();
    char info[160];
    snprintf(info, sizeof info, "%s  ·  %s", cfg_get("device_name", "NotaTV"), ip[0] ? ip : T("No network connection"));
    text_draw_clip(F_REG, 24, x + PAD + 8, y + 90, PW - 2 * PAD - 16, C_TEXT2, AL_LEFT, info);
    text_draw(F_SEMI, 22, x + PAD + 8, y + POWER_Y - 36, C_TEXT3, AL_LEFT, T("Power"));

    for (int i = 0; i < Q_N; i++) {
        if (!q_visible(q, i)) continue;
        QRect r = q_rect(q, i);
        float f = focus_amt(q_label(i), i == q->sel), fc = f < 0 ? 0 : f > 1 ? 1 : f;
        float sc = 1 + 0.04f * f, w = r.w * sc, h = r.h * sc, tx = x + r.x - (w - r.w) / 2, ty = y + r.y - (h - r.h) / 2;
        if (fc > 0.01f) draw_shadow(tx, ty, w, h, 24, 18, 0.7f * fc);
        fill_rrect(tx, ty, w, h, 24, color_mix(RGBA(255, 255, 255, 20), C_FOCUS, fc));
        Color c = color_mix(C_TEXT, RGB(10, 10, 10), fc), c2 = color_mix(C_TEXT2, RGB(60, 60, 66), fc);
        bool small = i >= Q_SLEEP;
        float ix = small ? tx + w / 2 : tx + 40, iy = small ? ty + 34 : ty + 36;
        if (i == Q_SLEEP) { /* the moon: cut out with the color behind it */
            fill_circle(ix, iy, 12, c);
            fill_circle(ix + 6, iy - 5, 10, color_mix(RGB(42, 46, 56), C_FOCUS, fc));
        } else draw_icon(q_icon(i), ix, iy, 32, c);
        if (small) text_draw(F_SEMI, 22, tx + w / 2, ty + 62, c, AL_CENTER, q_label(i));
        else text_draw_clip(F_SEMI, 26, tx + 22, ty + 66, w - 44, c, AL_LEFT, q_label(i));
        if (i == Q_TIMER) {
            char v[48];
            if (sleep_at > 0) {
                int m = (int)ceil((sleep_at - app_time()) / 60);
                snprintf(v, sizeof v, T("%d min left"), m < 1 ? 1 : m);
            } else snprintf(v, sizeof v, "%s", T("Off"));
            text_draw(F_REG, 24, tx + w - 22, ty + 22, c2, AL_RIGHT, v);
        }
    }
}

/* the timer steps through Off, 15, 30, 60, 90, 120 minutes */
static void timer_step(int dir) {
    int cur = 0;
    if (sleep_at > 0) { /* the step nearest to what is left */
        double left = (sleep_at - app_time()) / 60;
        cur = 1;
        for (int i = 1; i < N_SLEEP; i++) if (fabs(sleep_mins[i] - left) < fabs(sleep_mins[cur] - left)) cur = i;
    }
    int n = (cur + dir + N_SLEEP) % N_SLEEP;
    sleep_at = sleep_mins[n] ? app_time() + sleep_mins[n] * 60.0 : 0;
    sleep_warned = false;
}

static void quick_close(Screen *s) { ((Quick *)s->st)->closing = true; }

/* the nearest tile in the direction of the arrow; -1 = none */
static int q_step(const Quick *q, Action a) {
    QRect c = q_rect(q, q->sel);
    float cx = c.x + c.w / 2, cy = c.y + c.h / 2, best = 1e9f;
    int found = -1;
    for (int i = 0; i < Q_N; i++) {
        if (i == q->sel || !q_visible(q, i)) continue;
        QRect r = q_rect(q, i);
        float dx = r.x + r.w / 2 - cx, dy = r.y + r.h / 2 - cy, score;
        if (a == A_UP || a == A_DOWN) {
            if (a == A_UP ? dy >= -1 : dy <= 1) continue;
            score = fabsf(dy) * 1000 + fabsf(dx);
        } else {
            if (fabsf(dy) > 1 || (a == A_LEFT ? dx >= 0 : dx <= 0)) continue;
            score = fabsf(dx);
        }
        if (score < best) { best = score; found = i; }
    }
    return found;
}

static bool quick_input(Screen *s, const InputEvent *ev) {
    Quick *q = s->st;
    if (q->closing) return true;
    switch (ev->a) {
    case A_UP: case A_DOWN: case A_LEFT: case A_RIGHT: {
        int n = q_step(q, ev->a);
        if (n >= 0) q->sel = n;
        return true;
    }
    case A_BACK: case A_MENU: case A_HOME: case A_POWER: quick_close(s); return true;
    case A_OK: {
        if (ev->repeat) return true;
        int sec = -1;
        switch ((QItem)q->sel) {
        case Q_TIMER: timer_step(1); return true;
        case Q_EDIT: app_close(s); home_begin_edit(); return true; /* after the close: going back to home resets it */
        case Q_BT: sec = SET_DEVICES; break;
        case Q_AV: sec = SET_PICTURE; break;
        case Q_NET: sec = SET_NET; break;
        case Q_ALL: sec = 99; break;
        case Q_SLEEP: run_cmd("%s/power sleep &", helper_path()); break;
        case Q_RESTART: run_cmd("%s/power reboot &", helper_path()); break;
        case Q_OFF: run_cmd("%s/power poweroff &", helper_path()); break;
        default: break;
        }
        app_close(s);
        if (sec >= 0) app_push(settings_screen_at(sec));
        return true;
    }
    default: return true; /* the panel has the focus: nothing goes through to the screen behind it */
    }
}

static Screen *quick_open_now;
static void quick_destroy(Screen *s) {
    if (quick_open_now == s) quick_open_now = NULL;
    free(s->st);
    free(s);
}

void quick_open(void) {
    if (quick_open_now) return;
    Screen *s = xcalloc(1, sizeof *s);
    s->name = "quick";
    s->overlay = true;
    Quick *q = xcalloc(1, sizeof(Quick));
    Screen *under = app_top();
    q->from_home = under && under->name && !strcmp(under->name, "home");
    s->st = q;
    s->update = quick_update;
    s->draw = quick_draw;
    s->input = quick_input;
    s->destroy = quick_destroy;
    quick_open_now = s;
    app_push(s);
}
