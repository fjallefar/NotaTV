/* The screensaver (Debat 17): after screensaver_min minutes without a key press. With Immich signed in (and
 * cfg screensaver_photos on, the default) it shows random photos full screen that glide slowly (Ken Burns) and
 * fade over every PHOTO_S seconds, with the clock in a corner that wanders (burn-in). Otherwise the clock and the
 * date glide over black, as before. Only ever 2 photos in use; the next one is fetched while the current shows. */
#include "tv.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PHOTO_S 20.0
#define FADE_S 2.0
#define BATCH 30

static char *urls[BATCH];
static int n_urls, cur = -1, prev = -1;
static double cur_at;        /* when cur became the shown photo */
static bool fetching;
static unsigned gen;         /* a new screensaver round: answers for an older one are dropped */

static void drop_urls(void) {
    for (int i = 0; i < n_urls; i++) { free(urls[i]); urls[i] = NULL; }
    n_urls = 0;
    cur = prev = -1;
}

static void got_photos(void *ud, const char *const *u, int n) {
    fetching = false;
    if ((unsigned)(uintptr_t)ud != gen) return;
    drop_urls();
    for (int i = 0; i < n && i < BATCH; i++) urls[n_urls++] = str_dup(u[i]);
}

static const ImmichIface *photo_source(void) {
    if (!cfg_int("screensaver_photos", 1)) return NULL;
    const ImmichIface *im = app_iface("immich");
    return im && im->signed_in() ? im : NULL;
}

/* main.c: the screensaver starts (fetch a fresh batch) */
void screensaver_begin(void) {
    gen++;
    drop_urls();
    const ImmichIface *im = photo_source();
    if (im && !fetching) {
        fetching = true;
        im->random_photos(BATCH, got_photos, (void *)(uintptr_t)gen);
    }
}

/* main.c: the screensaver ended - nothing of it stays in memory */
void screensaver_end(void) {
    gen++;
    drop_urls();
}

/* true while photos are shown: they glide, so the main loop must not slow down to 20 fps */
bool screensaver_animated(void) { return cur >= 0; }

static void draw_photo(int i, double t, float a) {
    Image *im = img_get(urls[i]);
    if (img_state(im) != IMG_READY) return;
    /* Ken Burns: zoom from 1.0 to 1.08 over the photo's time, drifting one way per photo */
    float p = (float)(t / (PHOTO_S + FADE_S));
    if (p > 1) p = 1;
    float sc = 1.0f + 0.08f * p, w = UI_W * sc, h = UI_H * sc;
    float dx = (i & 1) ? 1.0f : -1.0f, dy = (i & 2) ? 1.0f : -1.0f;
    float x = -(w - UI_W) / 2 + dx * (w - UI_W) / 2 * p, y = -(h - UI_H) / 2 + dy * (h - UI_H) / 2 * p;
    img_draw_cover(im, x, y, w, h, 0, a);
}

static void draw_clock(float x, float y, float a, Align al, float size) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16], date[64];
    strftime(clk, sizeof clk, "%H:%M", &tm);
    static const char *days[] = {N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"), N_("Thursday"), N_("Friday"), N_("Saturday")};
    static const char *mon[] = {N_("January"), N_("February"), N_("March"), N_("April"), N_("May"), N_("June"), N_("July"),
                                N_("August"), N_("September"), N_("October"), N_("November"), N_("December")};
    snprintf(date, sizeof date, T("%1$s, %3$s %2$d"), T(days[tm.tm_wday]), tm.tm_mday, T(mon[tm.tm_mon]));
    text_draw(F_SEMI, size, x, y - size * 0.75f, RGBA(255, 255, 255, (uint8_t)fminf(255, 150 * a)), al, clk);
    text_draw(F_REG, size * 0.3f, x, y + size * 0.42f, RGBA(255, 255, 255, (uint8_t)fminf(255, 110 * a)), al, date);
}

/* main.c: draw it with alpha a (0..1 while it fades in or out); t = seconds since it started */
void screensaver_draw(float a, double t) {
    fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(255 * a)));
    if (n_urls > 0) {
        double now = app_time();
        if (cur < 0) { /* the first photo shows as soon as it is ready */
            if (img_state(img_get(urls[0])) == IMG_READY) { cur = 0; cur_at = now; }
            else if (img_state(img_get(urls[0])) == IMG_FAILED) { free(urls[0]); urls[0] = urls[--n_urls]; urls[n_urls] = NULL; }
        } else {
            int next = (cur + 1) % n_urls;
            Image *nx = img_get(urls[next]); /* fetched in advance */
            if (now - cur_at > PHOTO_S && next != cur && img_state(nx) == IMG_READY) {
                prev = cur;
                cur = next;
                cur_at = now; /* the batch loops (30 x 20 s = 10 min); the next screensaver gets a fresh one */
            }
        }
        if (cur >= 0) {
            double since = now - cur_at;
            float f = since < FADE_S ? (float)(since / FADE_S) : 1;
            if (f < 1 && prev >= 0 && prev < n_urls) draw_photo(prev, since + PHOTO_S, a);
            draw_photo(cur, since, a * f);
            /* the clock in a corner over a soft shade, wandering between the four corners every 10 minutes */
            int corner = (int)(t / 600) % 4;
            float x = (corner & 1) ? UI_W - 110 : 110, y = (corner & 2) ? 190 : UI_H - 150;
            Align al = (corner & 1) ? AL_RIGHT : AL_LEFT;
            Color dark = RGBA(0, 0, 0, (uint8_t)(150 * a)), clear = RGBA(0, 0, 0, 0);
            if (corner & 2) fill_rrect_vgrad(0, 0, UI_W, 380, 0, dark, clear);
            else fill_rrect_vgrad(0, UI_H - 380, UI_W, 380, 0, clear, dark);
            draw_clock(x, y, a * 1.6f > 1.6f ? 1.6f : a * 1.6f, al, 96); /* brighter than on black: photos are light */
            return;
        }
    }
    float x = UI_W / 2 + sinf((float)t * 0.05f) * 600, y = UI_H / 2 + sinf((float)t * 0.037f + 1) * 300;
    draw_clock(x, y + 30, a, AL_CENTER, 120);
}
