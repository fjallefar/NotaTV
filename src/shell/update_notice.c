/* NotaTV shell - update reminder: the box looks for a new version on GitHub by itself and reminds the user
 *
 * With the setting update_notify on
 * (default), the box asks GitHub ("update check" helper) about 90 s after start and then every 12 hours. When a newer
 * version exists, a card pops up on the home screen - at most once every other day (cfg update_notice_at). The card
 * offers Update now, Later and Don't remind me (turns update_notify off). With update_notify off the box never
 * contacts GitHub by itself; Settings → System → Update NotaTV still checks when asked.
 * Test without a Pi: TVSHELL_UPDATE_TEST="NY 9.9.9" pretends GitHub has that version (checked 2 s after start). */
#include "tv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK_EVERY (12 * 3600.0)
#define NOTICE_EVERY (2 * 86400)

static char found[32];    /* the newer version GitHub has ("" = none known) */
static bool checking;
static double next_check = -1;

static void check_work(void *arg) {
    char **out = arg;
    const char *test = getenv("TVSHELL_UPDATE_TEST");
    if (test) { *out = str_dup(test); return; }
    char *cmd = str_printf("%s/update check 2>/dev/null", helper_path());
    *out = run_capture(cmd);
    free(cmd);
}
static void check_done(void *arg) {
    char **out = arg;
    checking = false;
    if (*out) {
        str_trim(*out);
        if (!strncmp(*out, "NY ", 3)) snprintf(found, sizeof found, "%s", *out + 3);
        else if (!strncmp(*out, "NYESTE ", 7)) found[0] = 0; /* up to date (e.g. updated meanwhile) */
        /* errors (no network, GitHub down): keep what we knew and try again at the next check */
    }
    free(*out);
    free(out);
}

/* ---------- the card ---------- */
typedef struct { char ver[32]; int btn; } Notice;

static void notice_draw(Screen *s) {
    Notice *n = s->st;
    float a = s->anim;
    fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(170 * a)));
    float w = 980, h = 420, x = UI_W / 2 - w / 2, y = UI_H / 2 - h / 2 + 30 * (1 - a);
    draw_shadow(x, y, w, h, 32, 40, a);
    fill_rrect(x, y, w, h, 32, RGB(24, 28, 37));
    fill_rrect_vgrad(x + 60, y + 60, 120, 120, 30, RGB(16, 150, 140), RGB(8, 92, 96));
    draw_icon(ICON_TV, x + 120, y + 120, 60, C_TEXT);
    text_draw(F_SEMI, 24, x + 220, y + 66, C_ACCENT, AL_LEFT, T("UPDATE AVAILABLE"));
    char t[96];
    snprintf(t, sizeof t, T("NotaTV %s is ready"), n->ver);
    text_draw_clip(F_BOLD, 46, x + 220, y + 100, w - 280, C_TEXT, AL_LEFT, t);
    char cur[96];
    snprintf(cur, sizeof cur, T("You have version %s"), TV_VERSION);
    text_draw(F_REG, 26, x + 220, y + 162, C_TEXT2, AL_LEFT, cur);
    text_draw_wrap(F_REG, 26, x + 60, y + 220, w - 120, 2, 36, C_TEXT2,
                   T("The update takes a few minutes. Then restart the box to use it. Your settings, apps and logins are kept."));
    const char *b[3] = {T("Update now"), T("Later"), T("Don't remind me")};
    draw_buttons(UI_W / 2, y + h - 110, b, 3, n->btn);
}

static bool notice_input(Screen *s, const InputEvent *ev) {
    Notice *n = s->st;
    if (buttons_nav(&n->btn, 3, ev)) return true;
    if (ev->a == A_BACK || ev->a == A_HOME) { app_pop(); return true; }
    if (ev->a != A_OK || ev->repeat) return true;
    int b = n->btn;
    app_pop();
    if (b == 0) settings_update_install();
    else if (b == 2) {
        cfg_set_int("update_notify", 0);
        app_toast(T("No more update reminders - turn them on again under Settings → System"));
    }
    return true;
}

static void notice_open(const char *ver) {
    Screen *s = calloc(1, sizeof *s);
    Notice *n = calloc(1, sizeof *n);
    snprintf(n->ver, sizeof n->ver, "%s", ver);
    s->name = "update-notice";
    s->st = n;
    s->draw = notice_draw;
    s->input = notice_input;
    s->overlay = true;
    app_push(s);
}

/* every frame from the main loop */
void update_tick(void) {
    bool test = getenv("TVSHELL_UPDATE_TEST") != NULL;
    if ((!test && getenv("TVSHELL_SCRIPT")) || !cfg_int("update_notify", 1) || !cfg_int("setup_done", 0)) return;
    double now = app_time();
    if (next_check < 0) next_check = now + (test ? 2 : 90); /* give the network time to come up after boot */
    if (!checking && now >= next_check) {
        checking = true;
        next_check = now + CHECK_EVERY;
        job_submit_q(JQ_SLOW, check_work, check_done, calloc(1, sizeof(char *)));
    }
    if (!found[0]) return;
    /* only on the home screen (never over an app, playback or the setup guide), at most every other day */
    Screen *top = app_top();
    if (!top || !top->name || strcmp(top->name, "home")) return;
    time_t wall = time(NULL);
    if (wall < 1700000000) return; /* the clock is not set yet (no NTP) */
    long last = atol(cfg_get("update_notice_at", "0"));
    if (last > wall) last = 0; /* the clock was wrong when it was saved */
    if (wall - last < NOTICE_EVERY) return;
    char v[32];
    snprintf(v, sizeof v, "%ld", (long)wall);
    cfg_set("update_notice_at", v);
    notice_open(found);
}
