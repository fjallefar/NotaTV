/* Notifications on the TV (Debat 18): a small card that slides in at the top right for 8 seconds, also over the
 * screensaver ("The washing machine is done"). During video the player's OSD shows the text instead (the UI is
 * not drawn then). Up to 3 wait their turn; Home Assistant is the first user (app_notify from any app). */
#include "tv.h"
#include <stdio.h>
#include <string.h>

#define SHOW_S 8.0
#define QMAX 3

typedef struct { char title[96], text[240]; } Note;
static Note q[QMAX];
static int qn;
static double shown_at = -100;

void app_notify(const char *title, const char *text) {
    if (!text || !*text) return;
    double pos, dur;
    bool paused;
    if (player_state(&pos, &dur, &paused)) { /* video: the player's own message line */
        char buf[340];
        snprintf(buf, sizeof buf, "%s%s%s", title && *title ? title : "", title && *title ? ": " : "", text);
        player_osd(buf, (int)(SHOW_S * 1000));
        return;
    }
    if (qn == QMAX) { memmove(q, q + 1, sizeof q[0] * (QMAX - 1)); qn--; } /* the oldest waiting one goes */
    snprintf(q[qn].title, sizeof q[qn].title, "%s", title ? title : "");
    snprintf(q[qn].text, sizeof q[qn].text, "%s", text);
    SDL_Log("notify: besked i kø (%d)", qn + 1); /* never the text: it can be private */
    if (qn == 0) shown_at = app_time();
    qn++;
    app_request_redraw();
}

bool notify_active(void) { return qn > 0; }

void notify_draw(void) {
    if (!qn) return;
    double age = app_time() - shown_at;
    if (age > SHOW_S) {
        memmove(q, q + 1, sizeof q[0] * (QMAX - 1));
        qn--;
        shown_at = app_time();
        if (!qn) return;
        age = 0;
    }
    float a = age < 0.35 ? (float)(age / 0.35) : age > SHOW_S - 0.5 ? (float)((SHOW_S - age) / 0.5) : 1.0f;
    a = a * a * (3 - 2 * a);
    const Note *n = &q[0];
    float w = 560, x = UI_W - w - 60 + (1 - a) * 80, y = 60;
    bool two = text_width(F_REG, 24, n->text) > w - 64; /* the text wraps to a second line */
    float h = 28 + (n->title[0] ? 44 : 0) + (two ? 64 : 32) + 28;
    draw_panel(x, y, w, h, 24, a);
    float ty = y + 28;
    if (n->title[0]) {
        text_draw_clip(F_BOLD, 28, x + 32, ty, w - 64, color_alpha(C_TEXT, a), AL_LEFT, n->title);
        ty += 44;
    }
    text_draw_wrap(F_REG, 24, x + 32, ty, w - 64, 2, 32, color_alpha(C_TEXT2, a), n->text);
}
