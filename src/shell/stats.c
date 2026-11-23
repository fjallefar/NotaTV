/* Viewing statistics (Debat 20): only on this box, never sent anywhere. The system logs by itself - no app has to
 * do anything:
 *   - time spent in an app (opened from the home screen until back on the home screen; idle time under the
 *     screensaver does not count)
 *   - every playback (player_run): which app, the title, the series/channel (the subtitle before " · ") and the
 *     seconds it played
 * One line per event in <data>/stats.log: "<unix time>\t<kind a|p>\t<app id>\t<seconds>\t<group>\t<title>".
 * Lines older than two years are dropped at start. Signing out of an app or uninstalling it deletes its lines.
 * cfg stats_on=0: nothing is logged (and the log is deleted). */
#define _GNU_SOURCE
#include "tv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KEEP_S (2 * 366 * 86400L)

void stats_app_leave(void);

static char cur_app[40];
static double cur_since;

static void log_path(char *out, size_t n) { snprintf(out, n, "%s/stats.log", data_dir()); }
static bool stats_on(void) { return cfg_int("stats_on", 1) != 0; }

static void clean_field(char *s) {
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static void stats_write(char kind, const char *app, double secs, const char *group, const char *title) {
    if (!stats_on() || secs < 5) return; /* a few seconds is a mistake, not use */
    char path[512], g[160], t[200];
    log_path(path, sizeof path);
    snprintf(g, sizeof g, "%s", group ? group : "");
    snprintf(t, sizeof t, "%s", title ? title : "");
    clean_field(g);
    clean_field(t);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%ld\t%c\t%s\t%.0f\t%s\t%s\n", (long)time(NULL), kind, app && *app ? app : "other", secs, g, t);
    fclose(f);
}

/* keep the lines a filter likes (rewrite through a temp file) */
static void stats_rewrite(bool (*keep)(const char *line, const void *ud), const void *ud) {
    char path[512], tmp[540];
    log_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r");
    if (!in) return;
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); return; }
    char line[512];
    while (fgets(line, sizeof line, in))
        if (keep(line, ud)) fputs(line, out);
    fclose(in);
    fclose(out);
    rename(tmp, path);
}

static bool keep_recent(const char *line, const void *ud) {
    long now = *(const long *)ud;
    return now - atol(line) < KEEP_S;
}
static bool keep_other_apps(const char *line, const void *ud) {
    const char *id = ud;
    const char *p = strchr(line, '\t');
    if (!p || !(p = strchr(p + 1, '\t'))) return false;
    p++;
    size_t n = strlen(id);
    return !(strncmp(p, id, n) == 0 && p[n] == '\t');
}

void stats_init(void) {
    long now = (long)time(NULL);
    if (now > 1700000000L) stats_rewrite(keep_recent, &now); /* not before the clock is set */
}

/* the user opened an app (home screen, App Store, a row item) */
void stats_app_enter(const char *id) {
    if (cur_app[0] && strcmp(cur_app, id)) stats_app_leave();
    if (!cur_app[0]) cur_since = app_time();
    snprintf(cur_app, sizeof cur_app, "%s", id ? id : "");
}

/* back on the home screen, the screensaver started, or the box went to sleep */
void stats_app_leave(void) {
    if (!cur_app[0]) return;
    stats_write('a', cur_app, app_time() - cur_since, NULL, NULL);
    cur_app[0] = 0;
}

const char *stats_current_app(void) { return cur_app; }

void stats_pause(bool idle) {
    static bool paused;
    if (idle == paused || !cur_app[0]) { paused = idle && cur_app[0]; return; }
    paused = idle;
    if (idle) stats_write('a', cur_app, app_time() - cur_since, NULL, NULL);
    cur_since = app_time();
}

/* player.c: one playback ended */
void stats_played(const char *title, const char *subtitle, double secs) {
    char group[160] = "";
    if (subtitle) {
        snprintf(group, sizeof group, "%s", subtitle);
        char *dot = strstr(group, " · ");
        if (dot) *dot = 0;
    }
    stats_write('p', cur_app[0] ? cur_app : "other", secs, group, title);
}

/* sign-out or uninstall: everything about that app goes */
void stats_forget(const char *id) {
    if (!id || !*id) return;
    stats_rewrite(keep_other_apps, id);
}

void stats_forget_all(void) {
    char path[512];
    log_path(path, sizeof path);
    unlink(path);
}

/* ---------- reading (the statistics screen) ---------- */
int stats_read(long since, StatRow **apps, int *n_apps, StatRow **groups, int *n_groups, double *total_play) {
    *apps = *groups = NULL;
    *n_apps = *n_groups = 0;
    *total_play = 0;
    char path[512];
    log_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int lines = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *fld[6] = {0}, *p = line;
        int k = 0;
        for (; k < 6 && p; k++) { fld[k] = p; p = strchr(p, '\t'); if (p) *p++ = 0; }
        if (k < 6 || atol(fld[0]) < since) continue;
        double secs = atof(fld[3]);
        lines++;
        /* time per app: app time, and playback from outside an app (AirPlay, casting) */
        if (fld[1][0] == 'a' || (fld[1][0] == 'p' && !strcmp(fld[2], "other"))) {
            int i;
            for (i = 0; i < *n_apps && strcmp((*apps)[i].name, fld[2]); i++) {}
            if (i == *n_apps) {
                *apps = xrealloc(*apps, sizeof **apps * (*n_apps + 1));
                memset(&(*apps)[i], 0, sizeof **apps);
                snprintf((*apps)[i].name, sizeof (*apps)[i].name, "%s", fld[2]);
                (*n_apps)++;
            }
            (*apps)[i].secs += secs;
        }
        if (fld[1][0] == 'p') {
            *total_play += secs;
            const char *g = fld[4][0] ? fld[4] : fld[5];
            if (!*g) continue;
            int i;
            for (i = 0; i < *n_groups && strcmp((*groups)[i].name, g); i++) {}
            if (i == *n_groups) {
                if (*n_groups >= 2000) continue;
                *groups = xrealloc(*groups, sizeof **groups * (*n_groups + 1));
                memset(&(*groups)[i], 0, sizeof **groups);
                snprintf((*groups)[i].name, sizeof (*groups)[i].name, "%s", g);
                snprintf((*groups)[i].app, sizeof (*groups)[i].app, "%s", fld[2]);
                (*n_groups)++;
            }
            (*groups)[i].secs += secs;
            (*groups)[i].count++;
        }
    }
    fclose(f);
    return lines;
}
