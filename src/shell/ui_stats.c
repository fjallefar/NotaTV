/* "Your viewing" (Settings → System): what stats.c has logged on this box. Chips choose the period
 * (week, month, year, all); below: hours watched, time per app as bars, and the most watched series/channels. */
#include "tv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int period, chip;
    bool on_list;
    StatRow *apps, *groups;
    int n_apps, n_groups, lines;
    double total;
    float scroll;
    int sel;
} Stats;

static const char *period_name(int i) {
    static const char *n[] = {N_("This week"), N_("This month"), N_("This year"), N_("All")};
    return T(n[i]);
}

static int by_secs(const void *a, const void *b) {
    double x = ((const StatRow *)a)->secs, y = ((const StatRow *)b)->secs;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void reload(Stats *v) {
    free(v->apps);
    free(v->groups);
    long now = (long)time(NULL), since = 0;
    if (v->period == 0) since = now - 7 * 86400L;
    else if (v->period == 1) since = now - 30 * 86400L;
    else if (v->period == 2) since = now - 365 * 86400L;
    v->lines = stats_read(since, &v->apps, &v->n_apps, &v->groups, &v->n_groups, &v->total);
    if (v->n_apps > 1) qsort(v->apps, v->n_apps, sizeof *v->apps, by_secs);
    if (v->n_groups > 1) qsort(v->groups, v->n_groups, sizeof *v->groups, by_secs);
    v->sel = 0;
    v->scroll = 0;
}

static const char *app_label(const char *id) {
    if (!strcmp(id, "other")) return T("Casting and other");
    const NotaApp *a = app_find(id);
    return a ? T(a->name) : id;
}

static void fmt_hours(double secs, char *out, size_t n) {
    if (secs >= 3600) snprintf(out, n, T("%.1f h"), secs / 3600);
    else snprintf(out, n, T("%d min"), (int)(secs / 60 + 0.5));
}

static void stats_draw(Screen *s) {
    Stats *v = s->st;
    draw_background();
    draw_topbar(T("Your viewing"));
    const char *chips[4];
    for (int i = 0; i < 4; i++) chips[i] = period_name(i);
    draw_chips(96, 128, chips, 4, v->on_list ? -1 : v->period);
    if (!cfg_int("stats_on", 1)) {
        draw_empty(T("Viewing statistics are off"), T("Turn them on under Settings → System → Keep viewing statistics."));
        return;
    }
    if (v->lines == 0) {
        draw_empty(T("Nothing yet"), T("What you watch and which apps you use show up here. It is only saved on this box and never sent anywhere."));
        return;
    }
    char buf[64];
    /* the big number */
    fmt_hours(v->total, buf, sizeof buf);
    text_draw(F_BOLD, 72, 96, 210, C_TEXT, AL_LEFT, buf);
    text_draw(F_REG, 26, 96, 300, C_TEXT3, AL_LEFT, T("watched"));

    /* time per app: bars */
    float x = 96, y = 380, bw = 760;
    text_draw(F_SEMI, 30, x, y, C_TEXT, AL_LEFT, T("Time per app"));
    double max = v->n_apps ? v->apps[0].secs : 1;
    for (int i = 0; i < v->n_apps && i < 8; i++) {
        float yy = y + 60 + i * 72, w = (float)(bw * v->apps[i].secs / (max > 0 ? max : 1));
        if (w < 8) w = 8;
        text_draw_clip(F_REG, 24, x, yy, 260, C_TEXT2, AL_LEFT, app_label(v->apps[i].name));
        fill_rrect(x + 280, yy + 4, bw - 280, 26, 13, C_SURFACE);
        fill_rrect(x + 280, yy + 4, (w * (bw - 280)) / bw, 26, 13, C_ACCENT);
        fmt_hours(v->apps[i].secs, buf, sizeof buf);
        text_draw(F_SEMI, 22, x + bw + 20, yy + 2, C_TEXT2, AL_LEFT, buf);
    }

    /* most watched: series, channels, titles */
    float gx = 1040, gy = 380;
    text_draw(F_SEMI, 30, gx, gy, C_TEXT, AL_LEFT, T("Most watched"));
    clip_push(gx - 20, gy + 50, UI_W - gx, UI_H - gy - 80);
    for (int i = 0; i < v->n_groups && i < 50; i++) {
        float yy = gy + 60 + i * 72 - v->scroll;
        if (yy < gy || yy > UI_H) continue;
        bool f = v->on_list && i == v->sel;
        if (f) fill_rrect(gx - 16, yy - 10, UI_W - gx - 80, 64, 14, C_FOCUS);
        Color tc = f ? RGB(10, 10, 10) : C_TEXT;
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        text_draw(F_SEMI, 24, gx + 10, yy + 4, f ? tc : C_TEXT3, AL_CENTER, num);
        text_draw_clip(F_SEMI, 24, gx + 44, yy - 2, 520, tc, AL_LEFT, v->groups[i].name);
        text_draw_clip(F_REG, 20, gx + 44, yy + 26, 520, f ? RGB(60, 60, 60) : C_TEXT3, AL_LEFT, app_label(v->groups[i].app));
        fmt_hours(v->groups[i].secs, buf, sizeof buf);
        text_draw(F_SEMI, 22, UI_W - 120, yy + 8, f ? tc : C_TEXT2, AL_RIGHT, buf);
    }
    clip_pop();
}

static void stats_update(Screen *s, float dt) {
    Stats *v = s->st;
    float target = v->sel > 5 ? (v->sel - 5) * 72 : 0;
    v->scroll = anim_to(v->scroll, target, dt, 12);
}

static bool stats_input(Screen *s, const InputEvent *ev) {
    Stats *v = s->st;
    int n = v->n_groups < 50 ? v->n_groups : 50;
    if (!v->on_list) {
        int p = v->period;
        if (buttons_nav(&p, 4, ev)) { if (p != v->period) { v->period = p; reload(v); } return true; }
        if (ev->a == A_DOWN && n) { v->on_list = true; return true; }
        if (ev->a == A_BACK || ev->a == A_LEFT) { app_pop(); return true; }
        return true;
    }
    switch (ev->a) {
    case A_UP: if (v->sel) v->sel--; else v->on_list = false; return true;
    case A_DOWN: if (v->sel + 1 < n) v->sel++; return true;
    case A_BACK: v->on_list = false; return true;
    default: return true;
    }
}

static void stats_enter(Screen *s) { reload(s->st); }
static void stats_destroy(Screen *s) {
    Stats *v = s->st;
    free(v->apps);
    free(v->groups);
    free(v);
    free(s);
}

Screen *stats_screen(void) {
    Screen *s = xcalloc(1, sizeof *s);
    Stats *v = xcalloc(1, sizeof *v);
    s->name = "stats";
    s->st = v;
    s->enter = stats_enter;
    s->update = stats_update;
    s->draw = stats_draw;
    s->input = stats_input;
    s->destroy = stats_destroy;
    return s;
}
