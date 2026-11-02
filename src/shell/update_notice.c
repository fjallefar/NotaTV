/* NotaTV shell - update reminder: the box looks for a new version on GitHub by itself and reminds the user
 *
 * With the setting update_notify on
 * (default), the box asks GitHub ("update check" helper) about 90 s after start and then every 12 hours. When a newer
 * version exists, a card pops up on the home screen - at most once every other day (cfg update_notice_at). The card
 * offers Update now, Later and Don't remind me (turns update_notify off). With update_notify off the box never
 * contacts GitHub by itself; Settings → System → Update NotaTV still checks when asked.
 * Test without a Pi: TVSHELL_UPDATE_TEST="NY 9.9.9" pretends GitHub has that version (checked 2 s after start).
 *
 * Installing (update_install_open): a full card with a progress bar while the "update install" helper runs. The
 * helper writes its step to /tmp/notatv-update-state ("<percent>", or "dl <bytes total> <file>" while downloading,
 * then the bar follows the size of the file) and a line of text to /tmp/notatv-progress. When it is done the same
 * card asks Restart now / Restart later; on an error it shows what went wrong. */
#include "tv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
    char ver[32];
    snprintf(ver, sizeof ver, "%s", n->ver);
    app_pop(); /* frees n */
    if (b == 0) update_install_open(ver);
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

/* ---------- installing: progress bar, then restart now or later ---------- */
#define STATE_FILE "/tmp/notatv-update-state"
#define TEXT_FILE "/tmp/notatv-progress"
#define DL_SHARE 85.0 /* the download is this much of the bar; checking, unpacking and installing the rest */

enum { INS_RUNNING, INS_DONE, INS_FAILED };
typedef struct {
    char ver[32], step[200], err[300];
    int phase, btn;
    double target, shown; /* percent; target < 0 = not known yet */
    long long got, total; /* bytes downloaded / to download (total 0 = unknown) */
    double next_read;
} Install;
static Install *ins_cur; /* the open install card (the helper's result goes here) */

static void ins_read(Install *v) {
    FILE *f = fopen(STATE_FILE, "r");
    if (f) {
        char line[400] = "", path[300] = "";
        long long total = 0;
        if (fgets(line, sizeof line, f)) {
            if (sscanf(line, "dl %lld %299s", &total, path) == 2) {
                struct stat st;
                v->got = stat(path, &st) == 0 ? (long long)st.st_size : 0;
                v->total = total;
                v->target = total > 0 ? DL_SHARE * (double)(v->got < total ? v->got : total) / (double)total : -1;
            } else {
                v->target = atof(line);
                v->total = 0;
            }
        }
        fclose(f);
    }
    f = fopen(TEXT_FILE, "r");
    if (f) {
        char line[200] = "";
        if (fgets(line, sizeof line, f)) { str_trim(line); snprintf(v->step, sizeof v->step, "%s", tr_msg(line)); }
        fclose(f);
    }
}

static void ins_work(void *arg) {
    char **out = arg;
    char *cmd = str_printf("%s/update install 2>/dev/null", helper_path());
    *out = run_capture(cmd);
    free(cmd);
}
static void ins_done(void *arg) {
    char **out = arg;
    char *o = *out ? *out : str_dup("");
    str_trim(o);
    const char *last = strrchr(o, '\n');
    bool ok = !strcmp(last ? last + 1 : o, "OK");
    if (ins_cur) {
        ins_cur->phase = ok ? INS_DONE : INS_FAILED;
        ins_cur->btn = 0;
        ins_cur->target = ins_cur->shown = ok ? 100 : ins_cur->shown;
        snprintf(ins_cur->err, sizeof ins_cur->err, "%s", *o ? tr_msg(last ? last + 1 : o) : T("The update could not be installed"));
    }
    free(o);
    free(out);
}

static void ins_update(Screen *s, float dt) {
    Install *v = s->st;
    if (v->phase != INS_RUNNING) return;
    if (app_time() >= v->next_read) { v->next_read = app_time() + 0.25; ins_read(v); }
    if (v->target >= 0) {
        if (v->target < v->shown) v->shown = v->target; /* a download that starts over */
        double k = dt * 4;
        v->shown += (v->target - v->shown) * (k > 1 ? 1 : k);
    }
}

static void ins_draw(Screen *s) {
    Install *v = s->st;
    float a = s->anim;
    fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(200 * a)));
    float w = 1100, h = 470, x = UI_W / 2 - w / 2, y = UI_H / 2 - h / 2 + 30 * (1 - a);
    draw_shadow(x, y, w, h, 32, 40, a);
    fill_rrect(x, y, w, h, 32, RGB(24, 28, 37));
    char t[128];
    if (v->phase == INS_RUNNING) {
        fill_rrect_vgrad(x + 60, y + 60, 120, 120, 30, RGB(16, 150, 140), RGB(8, 92, 96));
        draw_icon(ICON_TV, x + 120, y + 120, 60, C_TEXT);
        text_draw(F_SEMI, 24, x + 220, y + 66, C_ACCENT, AL_LEFT, T("UPDATING"));
        snprintf(t, sizeof t, "NotaTV %s", v->ver);
        text_draw_clip(F_BOLD, 46, x + 220, y + 100, w - 280, C_TEXT, AL_LEFT, t);
        draw_spinner(x + 236, y + 184, 14, C_TEXT2);
        text_draw_clip(F_REG, 28, x + 264, y + 168, w - 480, C_TEXT2, AL_LEFT, v->step[0] ? v->step : T("Starting…"));
        /* the bar */
        float bx = x + 60, by = y + 250, bw = w - 120, bh = 18;
        fill_rrect(bx, by, bw, bh, bh / 2, RGBA(255, 255, 255, 30));
        if (v->target >= 0) {
            float fw = (float)(bw * v->shown / 100);
            if (fw > 0.5f) fill_rrect(bx, by, fw < bh ? bh : fw, bh, bh / 2, C_ACCENT);
            snprintf(t, sizeof t, "%d %%", (int)(v->shown + 0.5));
            text_draw(F_SEMI, 30, x + w - 60, y + 196, C_TEXT, AL_RIGHT, t);
        } else { /* size not known: a block that slides back and forth */
            double p = app_time() * 0.6;
            p -= (long)p;
            float bl = bw * 0.25f, ox = (float)((bw - bl) * (p < 0.5 ? p * 2 : 2 - p * 2));
            fill_rrect(bx + ox, by, bl, bh, bh / 2, C_ACCENT);
        }
        if (v->total > 0) {
            snprintf(t, sizeof t, T("%d MB of %d MB"), (int)(v->got / 1000000), (int)(v->total / 1000000));
            text_draw(F_REG, 24, bx, by + 34, C_TEXT3, AL_LEFT, t);
        }
        text_draw_wrap(F_REG, 26, x + 60, y + h - 110, w - 120, 2, 36, C_TEXT2,
                       T("Keep the box turned on until the update is done. Your settings, apps and logins are kept."));
        return;
    }
    bool ok = v->phase == INS_DONE;
    fill_rrect_vgrad(x + 60, y + 60, 120, 120, 30, ok ? RGB(40, 170, 90) : RGB(200, 70, 60), ok ? RGB(20, 110, 60) : RGB(130, 40, 35));
    draw_icon(ok ? ICON_CHECK : ICON_TV, x + 120, y + 120, 60, C_TEXT);
    text_draw(F_SEMI, 24, x + 220, y + 66, ok ? C_OK : C_DANGER, AL_LEFT, ok ? T("UPDATE INSTALLED") : T("UPDATE FAILED"));
    if (ok) snprintf(t, sizeof t, T("NotaTV %s is ready"), v->ver);
    else snprintf(t, sizeof t, "%s", T("The update did not finish"));
    text_draw_clip(F_BOLD, 46, x + 220, y + 100, w - 280, C_TEXT, AL_LEFT, t);
    text_draw_wrap(F_REG, 28, x + 60, y + 220, w - 120, 3, 40, C_TEXT2,
                   ok ? T("Restart the box to start using the new version. It takes about a minute.") : v->err);
    if (ok) {
        const char *b[2] = {T("Restart now"), T("Restart later")};
        draw_buttons(UI_W / 2, y + h - 110, b, 2, v->btn);
    } else {
        const char *b[1] = {"OK"};
        draw_buttons(UI_W / 2, y + h - 110, b, 1, 0);
    }
}

static bool ins_input(Screen *s, const InputEvent *ev) {
    Install *v = s->st;
    if (v->phase == INS_RUNNING) return true; /* nothing to cancel: the helper finishes or undoes it by itself */
    if (v->phase == INS_DONE && buttons_nav(&v->btn, 2, ev)) return true;
    if (ev->a == A_HOME) return true;
    if (ev->a == A_BACK) { if (v->phase == INS_DONE) v->btn = 1; else { app_pop(); return true; } }
    else if (ev->a != A_OK || ev->repeat) return true;
    if (v->phase == INS_DONE && v->btn == 0) {
        run_cmd("%s/power reboot >/dev/null 2>&1 &", helper_path());
        return true;
    }
    bool ok = v->phase == INS_DONE;
    char ver[32];
    snprintf(ver, sizeof ver, "%s", v->ver);
    app_pop();
    if (ok) app_toast(T("NotaTV %s starts the next time the box is restarted"), ver);
    return true;
}

static void ins_destroy(Screen *s) {
    if (ins_cur == s->st) ins_cur = NULL;
}

void update_install_open(const char *ver) {
    if (ins_cur) return; /* already installing */
    unlink(STATE_FILE);
    unlink(TEXT_FILE);
    Screen *s = calloc(1, sizeof *s);
    Install *v = calloc(1, sizeof *v);
    snprintf(v->ver, sizeof v->ver, "%s", ver && *ver ? ver : "");
    v->target = 0;
    s->name = "update-install";
    s->st = v;
    s->draw = ins_draw;
    s->input = ins_input;
    s->update = ins_update;
    s->destroy = ins_destroy;
    s->overlay = true;
    ins_cur = v;
    app_push(s);
    job_submit_q(JQ_SLOW, ins_work, ins_done, calloc(1, sizeof(char *)));
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
