/* NotaTV shell - the quick panel: slides in from the right (Red on the home screen, or the Power key).
   Sleep timer, shortcuts into Settings, and sleep / restart / power off. */
#include "tv.h"
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

Screen *settings_screen_at(int sec);

/* the sleep timer: the box goes to sleep at this time (0 = off). Checked by quick_tick every frame */
static double sleep_at;
static bool sleep_warned;
static const int sleep_mins[] = {0, 15, 30, 60, 90, 120};
#define N_SLEEP (int)(sizeof sleep_mins / sizeof sleep_mins[0])

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

typedef enum { Q_TIMER, Q_BT, Q_AV, Q_NET, Q_ALL, Q_SLEEP, Q_RESTART, Q_OFF, Q_N } QItem;
typedef struct { int sel; bool closing; float out; } Quick;

static const char *q_label(QItem i) {
    static const char *l[Q_N] = {N_("Sleep timer"), "Bluetooth", N_("Picture and sound"), N_("Network"), N_("All settings"),
                                 N_("Sleep (fast start)"), N_("Restart"), N_("Power off")};
    return T(l[i]);
}
static IconId q_icon(QItem i) {
    static const IconId ic[Q_N] = {ICON_CLOCK, ICON_MUSIC, ICON_FILM, ICON_WIFI, ICON_GEAR, ICON_MOON, ICON_RESTART, ICON_POWER};
    return ic[i];
}
#define PW 620.0f
#define ROW_H 76.0f
static float q_row_y(int i) { return 250 + i * ROW_H + (i >= Q_SLEEP ? 60 : 0); }

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
    float x = UI_W - PW * k - 24 * k, y = 24, h = UI_H - 48;
    draw_panel(x, y, PW, h, 36, 1);
    float ix = x + 44;
    /* the time, big, and what the box is called */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof clk, "%H:%M", &tm);
    text_draw(F_BOLD, 64, ix, y + 44, C_TEXT, AL_LEFT, clk);
    const char *ip = app_ip();
    char info[160];
    snprintf(info, sizeof info, "%s  ·  %s", cfg_get("device_name", "NotaTV"), ip[0] ? ip : T("No network connection"));
    text_draw_clip(F_REG, 24, ix, y + 136, PW - 88, C_TEXT2, AL_LEFT, info);

    float rw = PW - 48, rx = x + 24;
    float hy = slide_to(q, q_row_y(q->sel));
    draw_shadow(rx, hy, rw, ROW_H - 10, 33, 18, 0.6f);
    fill_rrect(rx, hy, rw, ROW_H - 10, 33, C_FOCUS);
    for (int i = 0; i < Q_N; i++) {
        float ry = q_row_y(i);
        float ov = 1 - fabsf(ry - hy) / ROW_H;
        if (ov < 0) ov = 0;
        Color c = color_mix(C_TEXT, RGB(10, 10, 10), ov), c2 = color_mix(C_TEXT2, RGB(60, 60, 66), ov);
        if (i == Q_SLEEP) { /* the moon: cut out with the color behind it (the pill or the panel) */
            fill_circle(rx + 46, ry + 33, 12, c);
            fill_circle(rx + 52, ry + 28, 10, color_mix(RGB(26, 30, 40), C_FOCUS, ov));
        } else draw_icon(q_icon(i), rx + 46, ry + 33, 30, c);
        text_draw(F_SEMI, 28, rx + 90, ry + 16, c, AL_LEFT, q_label(i));
        char v[48] = "";
        if (i == Q_TIMER) {
            if (sleep_at > 0) {
                int m = (int)ceil((sleep_at - app_time()) / 60);
                snprintf(v, sizeof v, T("%d min left"), m < 1 ? 1 : m);
            } else snprintf(v, sizeof v, "%s", T("Off"));
            float vw = text_width(F_REG, 26, v);
            text_draw(F_REG, 26, rx + rw - 40, ry + 18, c2, AL_RIGHT, v);
            if (ov > 0.5f) {
                fill_tri(rx + rw - vw - 64, ry + 33, rx + rw - vw - 54, ry + 25, rx + rw - vw - 54, ry + 41, c2);
                fill_tri(rx + rw - 20, ry + 33, rx + rw - 30, ry + 25, rx + rw - 30, ry + 41, c2);
            }
        } else if (i <= Q_ALL) {
            fill_tri(rx + rw - 40, ry + 23, rx + rw - 28, ry + 33, rx + rw - 40, ry + 43, c2);
        }
    }
    text_draw(F_SEMI, 22, ix, q_row_y(Q_SLEEP) - 44, C_TEXT3, AL_LEFT, T("Power"));
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

static bool quick_input(Screen *s, const InputEvent *ev) {
    Quick *q = s->st;
    if (q->closing) return true;
    switch (ev->a) {
    case A_UP: if (q->sel > 0) q->sel--; return true;
    case A_DOWN: if (q->sel < Q_N - 1) q->sel++; return true;
    case A_LEFT: case A_RIGHT:
        if (q->sel == Q_TIMER) timer_step(ev->a == A_RIGHT ? 1 : -1);
        else if (ev->a == A_RIGHT) return true;
        else quick_close(s);
        return true;
    case A_BACK: case A_MENU: case A_HOME: case A_POWER: quick_close(s); return true;
    case A_OK: {
        if (ev->repeat) return true;
        int sec = -1;
        switch ((QItem)q->sel) {
        case Q_TIMER: timer_step(1); return true;
        case Q_BT: sec = 3; break;
        case Q_AV: sec = 1; break;
        case Q_NET: sec = 0; break;
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
    Screen *s = calloc(1, sizeof *s);
    s->name = "quick";
    s->overlay = true;
    s->st = calloc(1, sizeof(Quick));
    s->update = quick_update;
    s->draw = quick_draw;
    s->input = quick_input;
    s->destroy = quick_destroy;
    quick_open_now = s;
    app_push(s);
}
