/* NotaTV shell - warning about a power supply that is too weak (STATUS 125).
 *
 * The Pi's firmware sets bits in its "throttled" value when the voltage drops below about 4.63 V:
 *   0x1     under-voltage right now
 *   0x10000 under-voltage has happened since the start (sticky)
 * The kernel shows the value in sysfs (raspberrypi firmware driver, get_throttled). Every 10 s we read it, and the
 * first time either bit is set a notification is shown in the corner (or on the player's message line during
 * video) - once per start, so it does not nag. The Overclock line under Advanced and logkeep's log line stay.
 * On a PC the file does not exist and nothing happens. TVSHELL_THROTTLED points at another file (testing). */
#include "tv.h"
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_EVERY 10.0

static char path[256];
static bool searched, warned;
static double next_check;

static void find_path(void) {
    searched = true;
    const char *env = getenv("TVSHELL_THROTTLED");
    if (env && *env) { snprintf(path, sizeof path, "%s", env); return; }
    static const char *const pats[] = {"/sys/devices/platform/soc/soc:firmware/get_throttled",
                                       "/sys/devices/platform/*/*firmware*/get_throttled"};
    for (size_t i = 0; i < sizeof pats / sizeof pats[0] && !*path; i++) {
        glob_t g;
        if (glob(pats[i], 0, NULL, &g) == 0 && g.gl_pathc) snprintf(path, sizeof path, "%s", g.gl_pathv[0]);
        globfree(&g);
    }
}

void power_warn_tick(void) {
    if (warned) return;
    double now = app_time();
    if (now < next_check) return;
    next_check = now + CHECK_EVERY;
    if (!searched) find_path();
    if (!*path) { warned = true; return; } /* not a Pi: stop looking */
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[32] = "";
    bool ok = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (!ok) return;
    char *end;
    unsigned long v = strtoul(buf, &end, 16); /* sysfs gives hex without 0x ("50005"); strtoul accepts both */
    if (end == buf || !(v & 0x10001UL)) return;
    warned = true;
    SDL_Log("power: under-voltage (throttled=%lx) - notification shown", v);
    app_notify(T("The power supply is too weak"),
               board_pi() == 5 ? T("The box gets too little power. Use the official 5 V/5 A power supply.")
                               : T("The box gets too little power. Use the official 5.1 V/3 A power supply."));
}
