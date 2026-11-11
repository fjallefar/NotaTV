/* NotaTV shell - anonymous usage count (STATUS 121).
 *
 * Once a day (UTC) the box sends only its version number to our Cloudflare Worker (server/stats):
 *   POST https://notatv-stats.notaapp.workers.dev/ping   {"v":"1.0.26"}
 * Nothing else: no box id, no settings, no account. The Worker adds one to the count for (day, version)
 * and stores no IP, so the count per day is roughly the number of boxes that were on that day.
 *
 * On by default, off under Settings -> System (cfg usage_ping). First try about 2 minutes after start
 * (network and clock up), then every hour, but only if today's ping has not been sent yet (cfg usage_ping_day).
 * A failed ping (no network) is simply tried again an hour later.
 * Never pings from a PC build, a "dev" build, uitest (TVSHELL_SCRIPT) or before the setup guide is done.
 * TVSHELL_USAGE_URL overrides the address (testing against "wrangler dev"; then the Pi and uitest checks are skipped). */
#include "tv.h"
#include <time.h>

#define USAGE_URL "https://notatv-stats.notaapp.workers.dev/ping"
#define FIRST_TRY 120
#define RETRY_EVERY 3600

static bool sending;
static double next_try = -1;
static char sending_day[16];

static void utc_day(char out[16]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 16, "%Y-%m-%d", &tm);
}

static bool version_ok(const char *v) {
    if (!v[0] || !strcmp(v, "dev") || strlen(v) > 24) return false;
    for (const char *p = v; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || *p == '.' || *p == '-')) return false;
    return true;
}

static void ping_work(void *arg) {
    char **out = arg;
    const char *url = getenv("TVSHELL_USAGE_URL");
    char body[64];
    snprintf(body, sizeof body, "{\"v\":\"%s\"}", TV_VERSION);
    /* -f: an HTTP error (4xx/5xx) counts as failed; the Worker answers 204 */
    const char *argv[] = {"curl", "-fsS", "-m", "15", "-o", "/dev/null", "-w", "%{http_code}",
                          "-H", "Content-Type: application/json", "--data-binary", body,
                          url ? url : USAGE_URL, NULL};
    *out = run_capture_argv(argv);
}

static void ping_done(void *arg) {
    char **out = arg;
    sending = false;
    if (*out) {
        str_trim(*out);
        if (!strcmp(*out, "204")) cfg_set("usage_ping_day", sending_day);
        else SDL_Log("usage ping: failed (%s), trying again in an hour", (*out)[0] ? *out : "no answer");
    }
    free(*out);
    free(out);
}

/* every frame from the main loop */
void usage_ping_tick(void) {
    if (sending || !cfg_int("usage_ping", 1) || !cfg_int("setup_done", 0)) return;
    bool test_url = getenv("TVSHELL_USAGE_URL") != NULL;
    if ((!test_url && (getenv("TVSHELL_SCRIPT") || !board_pi())) || !version_ok(TV_VERSION)) return;
    double now = app_time();
    if (next_try < 0) next_try = now + (test_url ? 2 : FIRST_TRY);
    if (now < next_try) return;
    next_try = now + RETRY_EVERY;
    if (time(NULL) < 1700000000) return; /* the clock is not set yet (no NTP) */
    utc_day(sending_day);
    if (!strcmp(sending_day, cfg_get("usage_ping_day", ""))) return;
    sending = true;
    job_submit_q(JQ_SLOW, ping_work, ping_done, xcalloc(1, sizeof(char *)));
}
