/* NotaTV shell - settings */
#include "tv.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_ITEMS 32 /* the most items in one section (Picture and sound has 17) */
#include <time.h>
#include <unistd.h>


typedef struct Section { const char *name; IconId icon; const Item *items; int n; const struct Section *subs; } Section;

/* ---------- handlinger ---------- */
static void helper_bg(const char *args) { run_cmd("%s/%s >/dev/null 2>&1 &", helper_path(), args); }

/* busy overlay while a job runs */
typedef struct { char title[128]; bool done; double t0; } Busy;
static Screen *busy_screen;
static void busy_draw(Screen *s) {
    Busy *b = s->st;
    fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(190 * s->anim)));
    draw_panel(UI_W / 2 - 420, UI_H / 2 - 150, 840, 300, 32, 1);
    draw_spinner(UI_W / 2, UI_H / 2 - 40, 30, C_TEXT);
    text_draw(F_SEMI, 30, UI_W / 2, UI_H / 2 + 30, C_TEXT, AL_CENTER, b->title);
    FILE *f = fopen("/tmp/notatv-progress", "r");
    if (f) {
        char line[200] = "";
        if (fgets(line, sizeof line, f)) { str_trim(line); text_draw(F_REG, 22, UI_W / 2, UI_H / 2 + 80, C_TEXT2, AL_CENTER, tr_msg(line)); }
        fclose(f);
    }
    if (app_time() - b->t0 > 30) /* BUSY_BACK_AFTER */
        text_draw(F_REG, 22, UI_W / 2, UI_H / 2 + 112, C_TEXT3, AL_CENTER, T("This takes longer than expected - Back closes it"));
}
#define BUSY_BACK_AFTER 30 /* s: then Back closes the overlay, if a helper hangs */
static bool busy_input(Screen *s, const InputEvent *ev) {
    Busy *b = s->st;
    if (ev->a == A_BACK && app_time() - b->t0 > BUSY_BACK_AFTER) {
        if (busy_screen == s) busy_screen = NULL;
        app_close(s);
    }
    return true;
}
static void busy_destroy(Screen *s) {
    if (busy_screen == s) busy_screen = NULL;
    free(s->st);
    free(s);
}
static void busy_open(const char *title) {
    unlink("/tmp/notatv-progress");
    Screen *s = calloc(1, sizeof *s);
    Busy *b = calloc(1, sizeof *b);
    snprintf(b->title, sizeof b->title, "%s", title);
    b->t0 = app_time();
    s->name = "busy";
    s->st = b;
    s->draw = busy_draw;
    s->input = busy_input;
    s->destroy = busy_destroy;
    s->overlay = true;
    busy_screen = s;
    app_push(s);
}
static void busy_close(void) { /* also when something was opened on top of it meanwhile */
    Screen *s = busy_screen;
    busy_screen = NULL;
    app_close(s);
}

/* run a helper in a background thread and call done with the output */
typedef struct { char *cmd, *out; void (*done)(char *out, void *ud); void *ud; } CmdJob;
static void cmd_work(void *arg) { CmdJob *j = arg; j->out = run_capture(j->cmd); }
static void cmd_done(void *arg) {
    CmdJob *j = arg;
    if (j->done) j->done(j->out, j->ud);
    free(j->out);
    free(j->cmd);
    free(j);
}
static void run_async(void (*done)(char *, void *), void *ud, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *args = NULL;
    if (vasprintf(&args, fmt, ap) < 0) { va_end(ap); return; }
    va_end(ap);
    CmdJob *j = calloc(1, sizeof *j);
    j->cmd = str_printf("%s/%s 2>&1", helper_path(), args);
    free(args);
    j->done = done;
    j->ud = ud;
    job_submit(cmd_work, cmd_done, j);
}

static char *sq(const char *s) { /* shell-citering */
    size_t n = strlen(s);
    char *o = malloc(n * 4 + 3), *w = o;
    *w++ = '\'';
    for (; *s; s++) {
        if (*s == '\'') { memcpy(w, "'\\''", 4); w += 4; } else *w++ = *s;
    }
    *w++ = '\'';
    *w = 0;
    return o;
}

/* linjer -> array */
static int split_lines(char *out, char ***lines) {
    int n = 0;
    *lines = NULL;
    for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {
        if (!*l) continue;
        *lines = realloc(*lines, sizeof(char *) * (n + 1));
        (*lines)[n++] = l;
    }
    return n;
}

/* --- Wi-Fi --- */
static char wifi_ssid[128];
static void wifi_connected(char *out, void *ud) {
    (void)ud;
    busy_close();
    str_trim(out);
    app_toast("%s", *out ? tr_msg(out) : T("Done"));
}
static void wifi_pw(void *ud, const char *pw) {
    (void)ud;
    if (!pw) return;
    char *a = sq(wifi_ssid), *b = sq(pw);
    busy_open(T("Connecting to Wi-Fi…"));
    run_async(wifi_connected, NULL, "wifi connect %s %s", a, b);
    free(a); free(b);
}
static char **scan_ssids;
static bool *scan_open;
static int n_scan;
static void wifi_pick(void *ud, int idx) {
    (void)ud;
    if (idx < 0 || idx >= n_scan) return;
    snprintf(wifi_ssid, sizeof wifi_ssid, "%s", scan_ssids[idx]);
    if (scan_open[idx]) wifi_pw(NULL, "");
    else {
        char t[200];
        snprintf(t, sizeof t, T("Password for %s"), wifi_ssid);
        keyboard_open(t, "", true, wifi_pw, NULL);
    }
}
static void wifi_scanned(char *out, void *ud) {
    (void)ud;
    busy_close();
    for (int i = 0; i < n_scan; i++) free(scan_ssids[i]);
    free(scan_ssids);
    free(scan_open);
    char **lines;
    int n = split_lines(out, &lines);
    scan_ssids = calloc(n + 1, sizeof(char *));
    scan_open = calloc(n + 1, sizeof(bool));
    char **labels = calloc(n + 1, sizeof(char *));
    n_scan = 0;
    for (int i = 0; i < n; i++) {
        char *ssid = strtok(lines[i], "\t"), *sig = strtok(NULL, "\t"), *sec = strtok(NULL, "\t");
        if (!ssid || !*ssid) continue;
        scan_ssids[n_scan] = str_dup(ssid);
        scan_open[n_scan] = !sec || !strcmp(sec, "open");
        labels[n_scan] = str_printf("%s   %s%%%s", ssid, sig ? sig : "?", scan_open[n_scan] ? "" : T("  · secured"));
        n_scan++;
    }
    if (n_scan == 0) app_toast(T("No Wi-Fi networks found"));
    else choice_open(T("Choose Wi-Fi network"), (const char *const *)labels, n_scan, 0, wifi_pick, NULL);
    for (int i = 0; i < n_scan; i++) free(labels[i]);
    free(labels);
    free(lines);
}
static void act_wifi(void) {
    busy_open(T("Searching for Wi-Fi networks…"));
    run_async(wifi_scanned, NULL, "wifi scan");
}
void settings_wifi(void) { act_wifi(); }
static void forgot(char *out, void *ud) { (void)ud; str_trim(out); app_toast("%s", *out ? tr_msg(out) : T("Wi-Fi forgotten")); }
static void act_wifi_forget(void) { run_async(forgot, NULL, "wifi forget"); }
static void act_phone_pin(void) { phone_pin_change(NULL); }
static void act_netwatch(void) { app_push(netwatch_screen()); }
static void act_bt_pair(void) { app_push(bt_pair_screen()); }
/* Forget paired devices: choose one device or all of them, then confirm */
static BtDevice fg_dev[16];
static int fg_n, fg_sel; /* fg_sel = fg_n: all devices */
static void forget_cb(void *ud, bool yes) {
    (void)ud;
    if (!yes) return;
    if (fg_sel >= fg_n) { bt_forget_all(); return; }
    bt_forget(fg_dev[fg_sel].path);
    app_toast(T("%s forgotten"), fg_dev[fg_sel].name);
}
static void forget_pick(void *ud, int idx) {
    (void)ud;
    if (idx < 0 || idx > fg_n) return;
    fg_sel = idx;
    char title[192];
    if (idx == fg_n) snprintf(title, sizeof title, "%s", T("Forget all Bluetooth devices?"));
    else snprintf(title, sizeof title, T("Forget %s?"), fg_dev[idx].name);
    confirm_open(title, idx == fg_n ? T("The phones will need to be paired again afterwards.") : T("It will need to be paired again afterwards."),
                 T("Forget"), T("Cancel"), forget_cb, NULL);
}
static void act_bt_forget(void) {
    fg_n = bt_devices(fg_dev, 16);
    if (fg_n <= 0) { fg_n = 0; app_toast(T("No paired devices")); return; }
    const char *items[17];
    for (int i = 0; i < fg_n; i++) items[i] = fg_dev[i].name[0] ? fg_dev[i].name : fg_dev[i].path;
    items[fg_n] = T("All devices");
    choice_open(T("Forget which device?"), items, fg_n + 1, 0, forget_pick, NULL);
}

/* --- name / text fields --- */
static const Item *text_item;
static void text_done(void *ud, const char *t) {
    (void)ud;
    if (!t || !text_item) return;
    char *v = str_dup(t);
    str_trim(v);
    cfg_set(text_item->key, v);
    free(v);
    if (text_item->changed) text_item->changed();
}

static void apply_ssh(void) { helper_bg(cfg_int("ssh_enabled", 0) ? "ssh on" : "ssh off"); }
static const char *airplay_off(void) { return cfg_int("airplay_mirror", 1) ? NULL : ITEM_HIDDEN; }
static const char *ssh_off(void) { return cfg_int("ssh_enabled", 0) ? NULL : ITEM_HIDDEN; }
static void restart_music(void) { helper_bg("music restart"); }
static void apply_name(void) { helper_bg("music restart"); helper_bg("cec setup"); bt_apply(); dlna_apply(); }
void settings_apply_name(void) { apply_name(); }
static void apply_share(void) { helper_bg("automount"); app_toast(T("Mounting network drive…")); }
/* smb address with "user:password@" keeps the login in the address - otherwise ask and save it in the settings */
static bool share_login_in(const char *s) {
    const char *sch = strstr(s, "://");
    if (!sch) return false;
    const char *at = strchr(sch + 3, '@'), *sl = strchr(sch + 3, '/');
    return at && (!sl || at < sl) && memchr(sch + 3, ':', at - (sch + 3)) != NULL;
}
/* a login in the address would still win - remove it so the saved login is used */
static void share_strip_login(void) {
    const char *s = cfg_get("media_share", "");
    const char *sch = strstr(s, "://");
    const char *at = sch ? strchr(sch + 3, '@') : NULL, *sl = sch ? strchr(sch + 3, '/') : NULL;
    if (!at || (sl && at > sl)) return;
    char *v = str_dup(s);
    memmove(v + (sch - s) + 3, v + (at - s) + 1, strlen(at + 1) + 1);
    cfg_set("media_share", v);
    free(v);
}
static void share_pass_done(void *ud, const char *t) {
    (void)ud;
    if (!t) return;
    share_strip_login();
    char *v = str_dup(t);
    cfg_set("media_share_pass", v);
    free(v);
    apply_share();
}
static void share_user_done(void *ud, const char *t) {
    (void)ud;
    if (!t) return;
    char *v = str_dup(t);
    str_trim(v);
    cfg_set("media_share_user", v);
    free(v);
    keyboard_open(T("Password"), cfg_get("media_share_pass", ""), true, share_pass_done, NULL);
}
static void share_login_start(void) {
    keyboard_open(T("Username (leave empty for guest)"), cfg_get("media_share_user", ""), false, share_user_done, NULL);
}
static void share_changed(void) {
    const char *s = cfg_get("media_share", "");
    if (!strncmp(s, "smb://", 6) && !share_login_in(s)) share_login_start();
    else apply_share();
}
static const char *share_login_off(void) { return !strncmp(cfg_get("media_share", ""), "smb://", 6) ? NULL : ITEM_HIDDEN; }
static void act_share_login(void) { share_login_start(); }
static void apply_cec(void) { helper_bg("cec setup"); }

/* --- audio devices --- */
static char **adev_ids;
static int n_adev;
static void adev_pick(void *ud, int idx) {
    (void)ud;
    if (idx < 0) return;
    cfg_set("audio_device", idx == 0 ? "" : adev_ids[idx]);
    app_toast(T("Audio output saved"));
}
static void adev_listed(char *out, void *ud) {
    (void)ud;
    for (int i = 0; i < n_adev; i++) free(adev_ids[i]);
    free(adev_ids);
    char **lines;
    int n = split_lines(out, &lines);
    adev_ids = calloc(n + 2, sizeof(char *));
    char **labels = calloc(n + 2, sizeof(char *));
    adev_ids[0] = str_dup("");
    labels[0] = str_dup(T("Automatic"));
    n_adev = 1;
    int sel = 0;
    for (int i = 0; i < n; i++) {
        char *id = strtok(lines[i], "\t"), *name = strtok(NULL, "\t");
        if (!id) continue;
        adev_ids[n_adev] = str_dup(id);
        labels[n_adev] = str_dup(name ? name : id);
        if (!strcmp(id, cfg_get("audio_device", ""))) sel = n_adev;
        n_adev++;
    }
    choice_open(T("Audio output"), (const char *const *)labels, n_adev, sel, adev_pick, NULL);
    for (int i = 0; i < n_adev; i++) free(labels[i]);
    free(labels);
    free(lines);
}
static void act_audio_dev(void) { run_async(adev_listed, NULL, "audio list"); }
static void act_audio_test(void) { helper_bg("audio test"); app_toast(T("Playing test sound…")); }

/* --- CEC --- */
static void cec_done(char *out, void *ud) { (void)ud; str_trim(out); app_toast("%s", *out ? tr_msg(out) : T("Sent")); }
static void act_cec_on(void) { run_async(cec_done, NULL, "cec tv-on"); }
static void act_cec_off(void) { run_async(cec_done, NULL, "cec tv-off"); }


/* sign out of an app: confirm first, then everything saved for the account is deleted (see the *_forget functions) */
static void (*signout_fn)(void);
static const char *signout_app;
static void signout_confirmed(void *ud, bool yes) {
    (void)ud;
    if (!yes || !signout_fn) return;
    signout_fn();
    app_toast(T("Signed out of %s"), signout_app);
}
void app_ask_signout(const char *app, void (*fn)(void)) {
    char t[120];
    snprintf(t, sizeof t, T("Sign out of %s?"), app);
    signout_fn = fn;
    signout_app = app;
    confirm_open(t, T("Everything saved for the account on this box is deleted."), T("Sign out"), T("Cancel"), signout_confirmed, NULL);
}
/* the logs are downloaded from the phone page (with the code), so they never leave the box by themselves */
static void act_logs(void) {
    const char *ip = app_ip();
    char msg[400];
    if (!cfg_int("phone_enabled", 1)) snprintf(msg, sizeof msg, "%s", T("Turn on the phone page first (Settings → Network → Phone page)."));
    else if (!ip[0]) snprintf(msg, sizeof msg, "%s", T("The box is not on a network."));
    else snprintf(msg, sizeof msg, T("Open http://%s on a phone or computer on the same network, enter the code %s and press Download logs at the bottom. Passwords and logins are removed from the files."), ip, phone_pin());
    confirm_open(T("Get the logs"), msg, "OK", "", NULL, NULL);
}
static void act_os_logout(void) { app_ask_signout("OpenSubtitles", subs_forget); }
static const char *os_signed_out(void) {
    return *cfg_get("os_apikey", "") || *cfg_get("os_user", "") || *cfg_get("os_pass", "") ? NULL : ITEM_HIDDEN;
}

/* --- system --- */
static void reboot_cb(void *ud, bool yes) { (void)ud; if (yes) helper_bg("power reboot"); }

/* update from GitHub releases: check -> ask -> install with a progress bar -> restart now or later (update_notice.c) */
static char gh_ver[32];
static void gh_install_cb(void *ud, bool yes) {
    (void)ud;
    if (yes) update_install_open(gh_ver);
}
static void gh_checked(char *out, void *ud) {
    (void)ud;
    busy_close();
    str_trim(out);
    if (!strncmp(out, "NY ", 3)) {
        char t[96];
        snprintf(gh_ver, sizeof gh_ver, "%s", out + 3);
        snprintf(t, sizeof t, T("NotaTV %s is available"), gh_ver);
        UpdateNews news;
        if (update_news(gh_ver, &news)) { /* what is new, from the release */
            const char *pts[UPDATE_NEWS_MAX];
            for (int i = 0; i < news.n; i++) pts[i] = news.points[i];
            notes_open(T("WHAT'S NEW"), t, news.summary, pts, news.n, T("Update"), T("Later"), gh_install_cb, NULL);
        } else
            confirm_open(t, T("The update will be downloaded and installed. Everything you have set up stays as it is."), T("Update"), T("Later"), gh_install_cb, NULL);
    } else if (!strncmp(out, "NYESTE ", 7)) app_toast(T("You have the latest version (%s)"), out + 7);
    else app_toast("%s", *out ? tr_msg(out) : T("Could not check for updates"));
}
static void act_update_gh(void) {
    busy_open(T("Checking for updates…"));
    run_async(gh_checked, NULL, "update check");
}
/* undo the last GitHub update (Pi): the files it replaced are kept in /data/notatv/update-backup */
static void rollback_done(char *out, void *ud) {
    (void)ud;
    busy_close();
    str_trim(out);
    if (!strcmp(out, "OK") || (strlen(out) > 3 && !strcmp(out + strlen(out) - 3, "\nOK")))
        confirm_open(T("Previous version restored"), T("Restart to use it."), T("Restart now"), T("Later"), reboot_cb, NULL);
    else app_toast("%s", *out ? tr_msg(out) : T("Could not restore the previous version"));
}
static void rollback_cb(void *ud, bool yes) {
    (void)ud;
    if (!yes) return;
    busy_open(T("Restoring the previous version…"));
    run_async(rollback_done, NULL, "update rollback");
}
static void act_rollback(void) {
    confirm_open(T("Undo last update?"), T("NotaTV goes back to the version from before the last update. Everything you have set up stays as it is."),
                 T("Undo"), T("Cancel"), rollback_cb, NULL);
}
static void reset_cb(void *ud, bool yes) {
    (void)ud;
    if (yes) helper_bg("factory-reset");
}
static void act_reset(void) {
    confirm_open(T("Reset to factory settings?"), T("All settings, logins and playlists will be deleted, and the box will restart."), T("Reset"), T("Cancel"), reset_cb, NULL);
}
/* overclock (Pi): the helper tries the new frequency once via tryboot and only makes it
 * permanent once the box has run for a minute - if it does not start, a power cycle restores the old one */
static void oc_done(char *out, void *ud) { (void)ud; str_trim(out); if (*out) app_toast("%s", tr_msg(out)); }
static void oc_cb(void *ud, bool yes) { (void)ud; if (yes) run_async(oc_done, NULL, "oc apply"); }
/* the setting per board (an SD card can move between a Pi 4 and a Pi 5): Pi 4 cpu_mhz (1500), Pi 5 cpu_mhz5 (2400) */
static const char *oc_key(void) { return board_pi() == 5 ? "cpu_mhz5" : "cpu_mhz"; }
static void act_oc_apply(void) {
    char title[96];
    snprintf(title, sizeof title, T("Restart at %s MHz?"), cfg_get(oc_key(), board_pi() == 5 ? "2400" : "1500"));
    confirm_open(title, T("If the box does not start (black screen for more than 2 minutes): turn it off and on again, and it will continue at the old frequency."), T("Restart"), T("Cancel"), oc_cb, NULL);
}

static void act_sleep(void) { helper_bg("power sleep"); }
static void act_reboot(void) { helper_bg("power reboot"); }
static void act_poweroff(void) { helper_bg("power poweroff"); }

/* ---------- sektioner ---------- */
const char *const set_onoff[] = {"0", "1", NULL};
#define onoff set_onoff
/* live TV: 50 Hz (Europe), 60 Hz (USA) or "Do not change" (mixed channel lists: the screen keeps its rate) */
static const char *const rate_o[] = {"50", "60", "0", NULL}, *const rate_l[] = {"50 Hz", "60 Hz", N_("Do not change"), NULL};
static const char *const lang_o[] = {"da,dan,dk,en,eng", "en,eng,da,dan", "sv,swe,en,eng", "no,nor,nob,en,eng", "de,deu,ger,en,eng", "no-subs", NULL};
static const char *const lang_l[] = {N_("Danish"), N_("English"), N_("Swedish"), N_("Norwegian"), N_("German"), N_("None"), NULL};
static const char *const alang_o[] = {"da,dan,en,eng", "en,eng", "sv,swe,en,eng", "no,nor,en,eng", "de,deu,ger,en,eng", NULL};
static const char *const alang_l[] = {N_("Danish"), N_("English"), N_("Swedish"), N_("Norwegian"), N_("German"), NULL};
static const char *const ss_o[] = {"0", "5", "10", "30", NULL}, *const ss_l[] = {N_("Off"), "5 min", "10 min", "30 min", NULL};
static const char *const delay_o[] = {"-300", "-200", "-150", "-100", "-50", "0", "50", "100", "150", "200", "300", NULL};
static const char *const delay_l[] = {"-300 ms", "-200 ms", "-150 ms", "-100 ms", "-50 ms", "0 ms", "+50 ms", "+100 ms", "+150 ms", "+200 ms", "+300 ms", NULL};

static const Item net_items[] = {
    {N_("Connection"), IT_INFO, .info_cmd = "net status"},
    {N_("IP address"), IT_INFO, .info_cmd = "net ip"},
    {N_("Wi-Fi network"), IT_ACTION, .action = act_wifi, .help = N_("Search for and connect to a wireless network")},
    {N_("Forget Wi-Fi"), IT_ACTION, .action = act_wifi_forget},
    {N_("Phone page"), IT_TOGGLE, "phone_enabled", "1", onoff, .help = N_("Control the box and type text from your phone's browser (http://<box IP>). Locked with the code below")},
    {N_("Phone page code"), IT_ACTION, .action = act_phone_pin, .value = phone_pin, .help = N_("Logins and server addresses sent from the phone must still be approved on the TV")},
    {N_("Network monitor"), IT_ACTION, .action = act_netwatch, .help = N_("See live which programs on the box talk to the internet, and to whom")},
    {N_("Device name"), IT_TEXT, "device_name", "NotaTV", .changed = apply_name, .help = N_("The name shown in AirPlay, Plex and on the TV")},
};
static void apply_hdr_always(void) { if (app_video_suspend()) app_video_resume(); }
const char *set_hdr_locked(void) { return display_hdr_ok() ? NULL : T("Not supported by the display"); }
#define hdr_locked set_hdr_locked
/* audio: the TV only lists the formats it accepts right now - often only after "Bitstream"/"Pass through" is
   chosen in its own sound settings (also for a soundbar on ARC) */
static const char *ac3_locked(void) { return display_audio_ok(DA_AC3) ? NULL : T("The TV does not accept it (check its sound output, e.g. Bitstream)"); }
static const char *dts_locked(void) { return display_audio_ok(DA_DTS) ? NULL : T("The TV does not accept it (check its sound output, e.g. Bitstream)"); }
static const char *hd_locked(void) { return display_audio_ok(DA_HD) ? NULL : T("The TV does not accept it (check its sound output, e.g. Bitstream)"); }
static const char *const hdrm_o[] = {"apple", "film", NULL}, *const hdrm_l[] = {N_("Like Apple TV"), N_("From the movie"), NULL};
static const Item av_items[] = {
    {N_("Display"), IT_INFO, .info_cmd = "display info"},
    {"HDR", IT_TOGGLE, "hdr", "1", onoff, .help = N_("Send an HDR signal to the TV for HDR content"), .locked = hdr_locked},
    {N_("Match frame rate"), IT_TOGGLE, "match_rate", "1", onoff, .help = N_("Switch the TV to the movie's frame rate (e.g. 23.976 Hz) - avoids judder")},
    {N_("Frame rate for live TV"), IT_CYCLE, "iptv_rate", "50", rate_o, rate_l},
    {N_("Always HDR"), IT_TOGGLE, "hdr_always", "0", onoff, .changed = apply_hdr_always,
     .help = N_("The TV stays in HDR all the time - menus too - so it never switches. Menus are converted to HDR"), .locked = hdr_locked},
    {N_("HDR brightness metadata"), IT_CYCLE, "hdr_meta", "apple", hdrm_o, hdrm_l,
     .help = N_("Like Apple TV: fixed values (MaxCLL 4000) - brighter on many TVs. From the movie: exactly as the movie was mastered"),
     .locked = hdr_locked},
    {N_("Audio output"), IT_ACTION, .action = act_audio_dev},
    {N_("Surround passthrough"), IT_TOGGLE, "audio_passthrough", "0", onoff, .help = N_("Send Dolby/DTS unchanged to a receiver or soundbar"), .locked = ac3_locked},
    {N_("DTS passthrough"), IT_TOGGLE, "pt_dts", "1", onoff, .locked = dts_locked},
    {"TrueHD / DTS-HD (Atmos)", IT_TOGGLE, "pt_hd", "0", onoff, .help = N_("Requires a receiver or soundbar that supports HD audio"), .locked = hd_locked},
    {N_("Audio delay (lip sync)"), IT_CYCLE, "audio_delay_ms", "0", delay_o, delay_l},
    {N_("Subtitle language"), IT_CYCLE, "sub_lang", "da,dan,dk,en,eng", lang_o, lang_l},
    {N_("Audio language"), IT_CYCLE, "audio_lang", "da,dan,en,eng", alang_o, alang_l},
    {N_("Play test sound"), IT_ACTION, .action = act_audio_test},
    {N_("OpenSubtitles API key"), IT_TEXT, "os_apikey", "", .help = N_("Free: create an account on opensubtitles.com → Consumers → new API key")},
    {N_("OpenSubtitles username"), IT_TEXT, "os_user", ""},
    {N_("OpenSubtitles password"), IT_TEXT, "os_pass", ""},
    {N_("Sign out of OpenSubtitles"), IT_ACTION, .action = act_os_logout, .locked = os_signed_out},
};
static const Item bt_items[] = {
    {"Bluetooth", IT_TOGGLE, "bt_enabled", "1", onoff, .changed = bt_apply, .help = N_("Use the TV as a Bluetooth speaker for phones")},
    {N_("Only visible while Music is open"), IT_TOGGLE, "bt_ondemand", "1", onoff, .changed = bt_apply,
     .help = N_("Phones can only find and connect to the TV while Music or \"Pair a phone\" is open (and for 2 min. after). Paired controllers and keyboards can always connect. Turn off if phones should be able to connect on their own")},
    {N_("Pair a phone"), IT_ACTION, .action = act_bt_pair, .help = N_("Makes the TV visible for 3 minutes. The code is confirmed here on the TV")},
    {N_("Forget paired devices"), IT_ACTION, .action = act_bt_forget, .help = N_("Choose a phone, controller or keyboard to forget - or all of them")},
    {N_("Cover art for Bluetooth music"), IT_TOGGLE, "music_cover_lookup", "1", onoff, .help = N_("Looks up cover art at Apple (iTunes) and in MusicBrainz (open music database). Sends artist and album there")},
    {N_("AirPlay (music from phone)"), IT_TOGGLE, "airplay_enabled", "1", onoff, .changed = restart_music, .help = N_("iPhone, iPad and Mac can play music on the TV. Local network only")},
};
static const Item cec_items[] = {
    {"Status", IT_INFO, .info_cmd = "cec status"},
    {N_("Turn on TV when the box turns on"), IT_TOGGLE, "cec_wake_tv", "1", onoff, .changed = apply_cec},
    {N_("Switch TV to the box's input"), IT_TOGGLE, "cec_active_source", "1", onoff, .changed = apply_cec},
    {N_("Sleep when the TV turns off"), IT_TOGGLE, "cec_standby", "1", onoff, .changed = apply_cec},
    {N_("Turn off TV when the box goes to sleep"), IT_TOGGLE, "cec_tv_off", "1", onoff},
    {N_("Turn on TV now (test)"), IT_ACTION, .action = act_cec_on},
    {N_("Turn off TV now (test)"), IT_ACTION, .action = act_cec_off},
};
static const Item cast_items[] = {
    {N_("DLNA (send from apps)"), IT_TOGGLE, "dlna_enabled", "1", onoff, .changed = dlna_apply, .help = N_("Apps like BubbleUPnP and \"Cast to Device\" in Windows can send video, music and pictures. Local network only")},
    {N_("AirPlay screen mirroring"), IT_TOGGLE, "airplay_mirror", "1", onoff, .changed = mirror_apply, .help = N_("iPhone/iPad/Mac can mirror the screen to the TV (shown as \"<name> (screen)\"). The first time, the phone asks for the code below")},
    {N_("AirPlay code"), IT_INFO, .value = airplay_pin, .locked = airplay_off},
    {N_("Network drive"), IT_TEXT, "media_share", "", .changed = share_changed, .help = N_("smb://user:password@server/share or nfs://server/path")},
    {N_("Network drive login"), IT_ACTION, .action = act_share_login, .locked = share_login_off},
};
static const char *time_now(void) {
    static char b[96];
    static const char *const dag[] = {N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"), N_("Thursday"), N_("Friday"), N_("Saturday")};
    static const char *const mdr[] = {N_("January"), N_("February"), N_("March"), N_("April"), N_("May"), N_("June"), N_("July"), N_("August"), N_("September"), N_("October"), N_("November"), N_("December")};
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char off[8];
    strftime(off, sizeof off, "%z", &tm);
    char d[64];
    /* TRANSLATORS: date: 1 = weekday, 2 = day of the month, 3 = month, 4 = year */
    snprintf(d, sizeof d, T("%1$s, %3$s %2$d, %4$d"), T(dag[tm.tm_wday]), tm.tm_mday, T(mdr[tm.tm_mon]), tm.tm_year + 1900);
    snprintf(b, sizeof b, "%02d:%02d   %s   (UTC%.3s:%.2s)", tm.tm_hour, tm.tm_min, d, off, off + 3);
    return b;
}
static const char *dst_note(void) {
    const char *z = cfg_get("tz_zone", "");
    return *z && !tz_zone_has_dst(z) ? T("This region has no daylight saving time") : "";
}
static void act_tz(void) { tz_pick(NULL, NULL); }
static void act_setup(void) { app_push(setup_screen()); }
/* language: saved and the user interface is restarted (init starts it again), so all texts change */
static const char *lang_value(void) {
    for (const LangInfo *l = i18n_langs; l->code; l++) if (!strcmp(l->code, i18n_lang())) return l->name;
    return "English";
}
static void lang_pick(void *ud, int idx) {
    (void)ud;
    if (idx < 0 || !i18n_langs[idx].code || !strcmp(i18n_langs[idx].code, i18n_lang())) return;
    cfg_set("language", i18n_langs[idx].code);
    app_quit();
}
static void act_language(void) {
    static const char *names[8];
    int n = 0, sel = 0;
    for (const LangInfo *l = i18n_langs; l->code && n < 8; l++, n++) {
        names[n] = l->name;
        if (!strcmp(l->code, i18n_lang())) sel = n;
    }
    choice_open(T("Language"), names, n, sel, lang_pick, NULL);
}
static const Item time_items[] = {
    {N_("Current time"), IT_INFO, .value = time_now},
    {N_("Country and region"), IT_ACTION, .action = act_tz, .value = tz_label, .help = N_("Chosen here on the box - nothing is looked up online")},
    {N_("Daylight saving time"), IT_TOGGLE, "tz_dst", "1", onoff, .changed = tz_apply,
     .help = N_("On: the clock switches between summer and winter time by the region's rules. Off: standard time all year")},
    {"", IT_INFO, .value = dst_note},
};
/* NotaTV is updated from GitHub releases */
static const Item sys_items[] = {
    {N_("Language"), IT_ACTION, .action = act_language, .value = lang_value, .help = N_("The language of the menus. The user interface restarts when you change it")},
    {N_("Version"), IT_INFO, .info_cmd = "sys version"},
    {N_("Boot time"), IT_INFO, .info_cmd = "sys boottime"},
    {N_("Uptime"), IT_INFO, .info_cmd = "sys uptime"},
    {N_("Hardware"), IT_INFO, .info_cmd = "sys hw"},
    {N_("Run the setup guide again"), IT_ACTION, .action = act_setup, .help = N_("Language, country, time, network, name and apps - like the first start")},
    {N_("Screensaver"), IT_CYCLE, "screensaver_min", "10", ss_o, ss_l},
    {N_("Show FPS"), IT_TOGGLE, "show_fps", "0", onoff, .help = N_("Frames per second and slowest frame (ms) in the top right - for troubleshooting")},
    {N_("Troubleshooting log"), IT_TOGGLE, "debug_log", "0", onoff, .changed = debug_log_apply,
     .help = N_("Saves more details about playback and the network, also across restarts. Turn it off again when the problem is found")},
    {N_("Get the logs"), IT_ACTION, .action = act_logs, .help = N_("Download them on a phone or computer to send to whoever helps you")},
    {N_("Developer mode (SSH)"), IT_TOGGLE, "ssh_enabled", "0", onoff, .changed = apply_ssh,
     .help = N_("SSH login as root with the password shown below. Every box has its own password")},
    {N_("SSH password"), IT_INFO, .info_cmd = "ssh password", .locked = ssh_off, .help = N_("Log in with ssh root@<the box's address>")},
    {N_("Update NotaTV"), IT_ACTION, .action = act_update_gh, .help = N_("Looks for a new version on GitHub and installs it")},
    {N_("Update reminders"), IT_TOGGLE, "update_notify", "1", onoff,
     .help = N_("Looks for a new version on GitHub twice a day and reminds you every other day. Off: NotaTV never checks by itself")},
    {N_("Undo last update"), IT_ACTION, .action = act_rollback, .help = N_("Goes back to the version from before the last update")},
    {N_("Sleep"), IT_ACTION, .action = act_sleep},
    {N_("Restart"), IT_ACTION, .action = act_reboot},
    {N_("Power off"), IT_ACTION, .action = act_poweroff},
    {N_("Reset to factory settings"), IT_ACTION, .action = act_reset},
};
static const char *const cpu_o[] = {"1500", "1600", "1700", "1800", "1900", "2000", "2100", "2147", NULL};
static const char *const cpu_l[] = {N_("1500 MHz (default)"), "1600 MHz", "1700 MHz", "1800 MHz", "1900 MHz", "2000 MHz", "2100 MHz", N_("2147 MHz (max - often unstable)"), NULL};
static const Item oc_items[] = {
    {N_("Now"), IT_INFO, .info_cmd = "oc status"},
    {N_("CPU frequency"), IT_CYCLE, "cpu_mhz", "1500", cpu_o, cpu_l,
     .help = N_("Above 1500 MHz the voltage is raised (over_voltage=6). Requires good cooling and a 5.1 V/3 A power supply")},
    {N_("Apply and restart"), IT_ACTION, .action = act_oc_apply,
     .help = N_("The new frequency is tried once first and saved after the box has run fine for a minute")},
};
/* Pi 5: 2400 MHz as standard; above that over_voltage_delta raises the voltage (see libexec/notatv/oc) */
static const char *const cpu5_o[] = {"2400", "2600", "2800", "3000", NULL};
static const char *const cpu5_l[] = {N_("2400 MHz (default)"), "2600 MHz", "2800 MHz", N_("3000 MHz (max - often unstable)"), NULL};
static const Item oc5_items[] = {
    {N_("Now"), IT_INFO, .info_cmd = "oc status"},
    {N_("CPU frequency"), IT_CYCLE, "cpu_mhz5", "2400", cpu5_o, cpu5_l,
     .help = N_("Above 2400 MHz the voltage is raised a little. Requires the active cooler and a 5 V/5 A power supply")},
    {N_("Apply and restart"), IT_ACTION, .action = act_oc_apply,
     .help = N_("The new frequency is tried once first and saved after the box has run fine for a minute")},
};
#define NI(a) (int)(sizeof(a) / sizeof(a[0]))
/* Settings → Apps: the installed apps that have settings (from the app register), and Cast and sharing.
   Built when Settings opens (apps can be installed and uninstalled meanwhile). */
#define MAX_APP_SECS 31
static Item apps_links[MAX_APP_SECS + 1];
static Section app_subs[MAX_APP_SECS + 1];
static int n_app_links;
static bool item_hidden(const Item *it) {
    const char *l = it->locked ? it->locked() : NULL;
    return l && !*l;
}
static bool any_shown(const Item *items, int n) {
    for (int i = 0; i < n; i++)
        if (!item_hidden(&items[i])) return true;
    return false;
}
static void build_app_sections(void) {
    int n = 0;
    for (int i = 0; i < app_count() && n < MAX_APP_SECS; i++) {
        const NotaApp *a = app_at(i);
        /* an app whose items are all hidden right now (e.g. only "Sign out", and nobody is signed in) is left out */
        if (!app_installed(a) || !a->settings || a->n_settings <= 0 || !any_shown(a->settings, a->n_settings)) continue;
        apps_links[n] = (Item){a->name, IT_SUB, .sub = n};
        /* the list has room for MAX_ITEMS (an app with more would be read past the end): the rest is left out */
        int ni = a->n_settings > MAX_ITEMS ? MAX_ITEMS : a->n_settings;
        if (ni < a->n_settings) SDL_Log("app %s: %d indstillinger, kun de første %d vises", a->id, a->n_settings, ni);
        app_subs[n] = (Section){a->name, a->icon, a->settings, ni};
        n++;
    }
    apps_links[n] = (Item){N_("Cast and sharing"), IT_SUB, .sub = n};
    app_subs[n] = (Section){N_("Cast and sharing"), ICON_PLAY, cast_items, NI(cast_items)};
    n_app_links = n + 1;
}
static Section sections[] = { /* not const: the number of Apps items is set when Settings opens */
    {N_("Network"), ICON_WIFI, net_items, NI(net_items)},
    {N_("Picture and sound"), ICON_FILM, av_items, NI(av_items)},
    {"HDMI-CEC", ICON_TV, cec_items, NI(cec_items)},
    {"Bluetooth", ICON_MUSIC, bt_items, NI(bt_items)},
    {N_("Apps"), ICON_PLAY, apps_links, 1, app_subs}, /* n is set by build_app_sections */
    {N_("Date and time"), ICON_CLOCK, time_items, NI(time_items)},
    {N_("System"), ICON_GEAR, sys_items, NI(sys_items)},
    {"Overclock", ICON_POWER, oc_items, NI(oc_items)},
};
#define N_SEC NI(sections)
static void rebuild_apps(void) {
    build_app_sections();
    for (int i = 0; i < N_SEC; i++) /* Overclock: the limits of the board we run on */
        if (sections[i].items == oc_items || sections[i].items == oc5_items) {
            sections[i].items = board_pi() == 5 ? oc5_items : oc_items;
            sections[i].n = board_pi() == 5 ? NI(oc5_items) : NI(oc_items);
        }
    for (int i = 0; i < N_SEC; i++)
        if (sections[i].items == apps_links) sections[i].n = n_app_links;
}

_Static_assert(NI(net_items) <= MAX_ITEMS && NI(av_items) <= MAX_ITEMS && NI(cec_items) <= MAX_ITEMS && NI(bt_items) <= MAX_ITEMS &&
               NI(cast_items) <= MAX_ITEMS && NI(sys_items) <= MAX_ITEMS && NI(oc_items) <= MAX_ITEMS && NI(oc5_items) <= MAX_ITEMS, "øg MAX_ITEMS");

/* ---------- screen ---------- */
typedef struct {
    int col, sec, item;
    int sub;     /* open app section under Apps, -1 = the list */
    float scroll;
    char *info[MAX_ITEMS];
    int info_sec;  /* which list the info values belong to (see sec_id) */
    double info_t;
    bool info_loading;
    Spring hl_sec, hl_item; /* the focus pills slide between rows */
    float col_f;            /* 0 = the menu on the left has focus, 1 = the list */
    int hl_sec_id;          /* the list the item pill belongs to (it jumps, not slides, to a new list) */
} Settings;

typedef struct { Settings *s; int id; const Section *cs; char *vals[MAX_ITEMS]; int token; } InfoJob;

/* the list of items on the right: the section, or an app's section under Apps */
static const Section *cur_sec(const Settings *s) {
    const Section *sec = &sections[s->sec];
    return sec->subs && s->sub >= 0 ? &sec->subs[s->sub] : sec;
}
static int sec_id(const Settings *s) { return s->sec * 16 + s->sub + 1; }
static int settings_token;

static void info_work(void *arg) {
    InfoJob *j = arg;
    const Section *sc = j->cs;
    for (int i = 0; i < sc->n && i < MAX_ITEMS; i++) {
        if (sc->items[i].type != IT_INFO || !sc->items[i].info_cmd) continue;
        char *cmd = str_printf("%s/%s 2>/dev/null", helper_path(), sc->items[i].info_cmd);
        j->vals[i] = run_capture(cmd);
        str_trim(j->vals[i]);
        free(cmd);
    }
}

static void info_done(void *arg) {
    InfoJob *j = arg;
    if (j->token == settings_token && j->s->info_sec == j->id) {
        for (int i = 0; i < MAX_ITEMS; i++) { free(j->s->info[i]); j->s->info[i] = j->vals[i]; }
        j->s->info_loading = false;
    } else {
        for (int i = 0; i < MAX_ITEMS; i++) free(j->vals[i]);
    }
    free(j);
}

static void info_refresh(Settings *s) {
    if (s->info_loading) return;
    s->info_loading = true;
    s->info_t = app_time();
    InfoJob *j = calloc(1, sizeof *j);
    j->s = s;
    j->id = s->info_sec = sec_id(s);
    j->cs = cur_sec(s);
    j->token = settings_token;
    job_submit(info_work, info_done, j);
}

static void settings_enter(Screen *sc) {
    Settings *s = sc->st;
    settings_token++;
    if (s->sub < 0) { /* back from an app (e.g. signed in): the list of apps with settings may have changed */
        rebuild_apps();
        if (sections[s->sec].items == apps_links && s->item >= n_app_links) s->item = n_app_links - 1;
    }
    s->info_sec = -1;
    info_refresh(s);
}

static void settings_destroy(Screen *sc) {
    Settings *s = sc->st;
    settings_token++;
    for (int i = 0; i < MAX_ITEMS; i++) free(s->info[i]);
    free(s);
    free(sc);
}

/* the row an item is drawn in (hidden items take no row) */
static int item_row(const Section *sec, int idx) {
    int r = 0;
    for (int i = 0; i < idx && i < sec->n; i++)
        if (!item_hidden(&sec->items[i])) r++;
    return r;
}
static bool selectable(const Section *sec, int i) {
    return i >= 0 && i < sec->n && sec->items[i].type != IT_INFO && !item_hidden(&sec->items[i]);
}
static int next_selectable(const Section *sec, int from, int dir) {
    int i = from + dir;
    while (i >= 0 && i < sec->n && !selectable(sec, i)) i += dir;
    return i >= 0 && i < sec->n ? i : from;
}
static int first_selectable(const Section *sec) {
    int i = next_selectable(sec, -1, 1);
    return i < 0 ? 0 : i;
}

/* Back/Left: out of an app's settings to the list of apps, otherwise to the side menu */
static bool leave_item(Settings *s) {
    if (s->sub >= 0) {
        s->item = s->sub;
        s->sub = -1;
        rebuild_apps(); /* an app may have nothing left to show (signed out) */
        if (s->item >= n_app_links) s->item = n_app_links - 1;
        return true;
    }
    s->col = 0;
    return true;
}

static void settings_update(Screen *sc, float dt) {
    Settings *s = sc->st;
    /* the focused item disappeared (e.g. signed out): move to the nearest one still there */
    if (s->col == 1) {
        const Section *sec = cur_sec(s);
        /* an app's page with nothing left on it (signed out of an app whose only item was "Sign out"): back to the list */
        if (s->sub >= 0 && !any_shown(sec->items, sec->n) && app_top() == sc) { leave_item(s); sec = cur_sec(s); }
        if (!selectable(sec, s->item)) {
            int i = next_selectable(sec, s->item, 1);
            if (i == s->item) i = next_selectable(sec, s->item, -1);
            s->item = i;
        }
    }
    if (s->info_sec != sec_id(s) || app_time() - s->info_t > 4) {
        if (s->info_sec != sec_id(s)) {
            for (int i = 0; i < MAX_ITEMS; i++) { free(s->info[i]); s->info[i] = NULL; }
            s->info_loading = false;
        }
        info_refresh(s);
    }
    float target = item_row(cur_sec(s), s->item) * 88 - 88 * 5;
    if (target < 0) target = 0;
    s->scroll = anim_to(s->scroll, target, dt, 14);
    spring_to(&s->hl_sec, s->sec * 76.0f, dt, SPRING_SOFT);
    float it_y = item_row(cur_sec(s), s->item) * 88.0f;
    if (s->hl_sec_id != sec_id(s)) { s->hl_sec_id = sec_id(s); s->hl_item = (Spring){it_y, 0}; }
    spring_to(&s->hl_item, it_y, dt, SPRING_SOFT);
    s->col_f = anim_to(s->col_f, (float)s->col, dt, 14);
}

static const char *item_def(const Item *it) {
    const char *d = it->key ? lang_media_default(it->key) : NULL;
    return d ? d : it->def;
}
static int opt_index(const Item *it) {
    const char *v = cfg_get(it->key, item_def(it));
    for (int i = 0; it->opts[i]; i++)
        if (!strcmp(it->opts[i], v)) return i;
    return 0;
}

static void settings_draw(Screen *sc) {
    Settings *s = sc->st;
    draw_background();
    draw_topbar(T("Settings"));
    { /* the menu's pill: white while the menu has focus, a quiet marker while the list has it */
        float y = 170 + s->hl_sec.x, k = 1 - s->col_f;
        if (k > 0.01f) draw_shadow(80, y, 440, 64, 32, 20, 0.6f * k);
        fill_rrect(80, y, 440, 64, 32, color_mix(color_alpha(C_SURFACE2, 0.8f), C_FOCUS, k));
    }
    for (int i = 0; i < N_SEC; i++) {
        float y = 170 + i * 76;
        bool sel = i == s->sec;
        /* how much of the white pill is under this row right now: the text turns dark as the pill slides in */
        float ov = 1 - fabsf(y - (170 + s->hl_sec.x)) / 76.0f;
        ov = (ov < 0 ? 0 : ov) * (1 - s->col_f);
        Color c = color_mix(sel ? C_TEXT : C_TEXT2, RGB(10, 10, 10), ov);
        draw_icon(sections[i].icon, 122, y + 32, 30, c);
        text_draw(F_SEMI, 28, 160, y + 15, c, AL_LEFT, T(sections[i].name));
    }
    const Section *sec = cur_sec(s);
    float x = 580, w = UI_W - x - 80;
    if (s->sub >= 0) {
        char crumb[160];
        snprintf(crumb, sizeof crumb, "%s - %s", T(sections[s->sec].name), T(sec->name));
        text_draw(F_SEMI, 26, x + 4, 112, C_TEXT2, AL_LEFT, crumb);
    }
    clip_push(x - 30, 140, w + 60, UI_H - 220);
    if (s->col_f > 0.01f) { /* the list's pill slides to the focused row */
        float y = 170 + s->hl_item.x - s->scroll, a = s->col_f;
        draw_shadow(x, y, w, 76, 38, 24, 0.7f * a);
        fill_rrect(x, y, w, 76, 38, color_alpha(C_FOCUS, a));
    }
    int row = 0;
    for (int i = 0; i < sec->n; i++) {
        const Item *it = &sec->items[i];
        if (item_hidden(it)) continue;
        float y = 170 + row++ * 88 - s->scroll;
        if (y < 80 || y > UI_H) continue;
        bool f = s->col == 1 && s->item == i;
        bool info = it->type == IT_INFO;
        /* rows without focus have no box of their own - only text. The text turns dark as the pill slides under it */
        float ov = 1 - fabsf(y - (170 + s->hl_item.x - s->scroll)) / 88.0f;
        ov = (ov < 0 ? 0 : ov) * s->col_f;
        Color lc = color_mix(info ? C_TEXT2 : C_TEXT, RGB(10, 10, 10), ov);
        Color vc = color_mix(C_TEXT2, RGB(50, 50, 55), ov);
        text_draw(F_SEMI, 28, x + 32, y + 21, lc, AL_LEFT, T(it->label));
        float vx = x + w - 32;
        const char *lock = it->locked ? it->locked() : NULL;
        if (lock) { text_draw_clip(F_REG, 26, vx, y + 23, w * 0.6f, vc, AL_RIGHT, lock); continue; }
        switch (it->type) {
        case IT_INFO: {
            const char *v = it->value ? it->value() : s->info[i] && *s->info[i] ? tr_msg(s->info[i]) : (s->info_loading ? "…" : "-");
            text_draw_clip(F_REG, 26, vx, y + 23, w * 0.6f, info ? C_TEXT : vc, AL_RIGHT, v);
            break;
        }
        case IT_TOGGLE: {
            bool on = cfg_int(it->key, atoi(it->def)) != 0;
            float tw = 76, th = 40, tx = vx - tw, ty = y + 18;
            fill_rrect(tx, ty, tw, th, th / 2, on ? C_OK : (f ? RGB(180, 180, 185) : C_SURFACE2));
            fill_circle(on ? tx + tw - th / 2 : tx + th / 2, ty + th / 2, th / 2 - 4, RGB(255, 255, 255));
            break;
        }
        case IT_CYCLE: {
            int k = opt_index(it);
            const char *lbl = it->labels ? T(it->labels[k]) : it->opts[k];
            float lw = text_width(F_REG, 26, lbl);
            text_draw(F_REG, 26, vx - 24, y + 23, vc, AL_RIGHT, lbl);
            if (f) {
                fill_tri(vx - lw - 48, y + 38, vx - lw - 38, y + 30, vx - lw - 38, y + 46, vc);
                fill_tri(vx - 4, y + 38, vx - 14, y + 30, vx - 14, y + 46, vc);
            }
            break;
        }
        case IT_TEXT: {
            const char *v = cfg_get(it->key, it->def);
            bool secret = strstr(it->key, "pass") != NULL;
            /* an address with user:password@ (e.g. the network drive) is shown without them */
            char shown[300];
            snprintf(shown, sizeof shown, "%s", v);
            char *sch = strstr(shown, "://"), *at = sch ? strchr(sch + 3, '@') : NULL, *sl = sch ? strchr(sch + 3, '/') : NULL;
            if (at && (!sl || at < sl)) memmove(sch + 3, at + 1, strlen(at + 1) + 1);
            text_draw_clip(F_REG, 24, vx, y + 24, w * 0.5f, vc, AL_RIGHT, !*v ? T("Not set") : secret ? "••••••••" : shown);
            break;
        }
        case IT_SUB:
        case IT_ACTION:
            fill_tri(vx - 14, y + 28, vx - 2, y + 38, vx - 14, y + 48, vc);
            if (it->value) text_draw_clip(F_REG, 26, vx - 30, y + 23, w * 0.5f, vc, AL_RIGHT, it->value());
            break;
        }
    }
    clip_pop();
    if (s->col == 1 && selectable(sec, s->item) && sec->items[s->item].help) {
        /* one line as before; a longer text wraps to two lines instead of being cut off at the screen edge */
        const char *h = T(sec->items[s->item].help);
        if (text_width(F_REG, 24, h) <= w - 32) text_draw(F_REG, 24, x + 32, UI_H - 70, C_TEXT3, AL_LEFT, h);
        else text_draw_wrap(F_REG, 24, x + 32, UI_H - 84, w - 32, 2, 30, C_TEXT3, h);
    }
}

static bool settings_input(Screen *sc, const InputEvent *ev) {
    Settings *s = sc->st;
    const Section *sec = cur_sec(s);
    if (s->col == 0) {
        switch (ev->a) {
        case A_UP: if (s->sec > 0) s->sec--; s->item = 0; s->sub = -1; return true;
        case A_DOWN: if (s->sec < N_SEC - 1) s->sec++; s->item = 0; s->sub = -1; return true;
        case A_RIGHT: case A_OK: {
            s->col = 1;
            s->item = first_selectable(sec);
            return true;
        }
        default: return false;
        }
    }
    if (!selectable(sec, s->item)) { /* nothing to focus in this list */
        if (ev->a == A_BACK || ev->a == A_LEFT) return leave_item(s);
        return ev->a != A_HOME;
    }
    const Item *it = &sec->items[s->item];
    if (it->locked && it->locked()) { /* locked item: cannot be changed; Left/Back still leave it */
        if (ev->a == A_LEFT) return leave_item(s);
        if (ev->a == A_OK || ev->a == A_RIGHT) return true;
    }
    switch (ev->a) {
    case A_UP: s->item = next_selectable(sec, s->item, -1); return true;
    case A_DOWN: s->item = next_selectable(sec, s->item, 1); return true;
    case A_BACK: return leave_item(s);
    case A_LEFT: case A_RIGHT:
        if (it->type == IT_CYCLE) {
            int k = opt_index(it), n = 0;
            while (it->opts[n]) n++;
            k = (k + (ev->a == A_RIGHT ? 1 : n - 1)) % n;
            cfg_set(it->key, it->opts[k]);
            if (it->changed) it->changed();
            return true;
        }
        if (it->type == IT_TOGGLE) {
            bool on = ev->a == A_RIGHT;
            if ((cfg_int(it->key, atoi(it->def)) != 0) == on) return true; /* already so: nothing to apply (would e.g. redo the video setup) */
            cfg_set_int(it->key, on);
            if (it->changed) it->changed();
            return true;
        }
        if (ev->a == A_LEFT) return leave_item(s);
        return true;
    case A_OK:
        switch (it->type) {
        case IT_TOGGLE:
            cfg_set_int(it->key, !cfg_int(it->key, atoi(it->def)));
            if (it->changed) it->changed();
            break;
        case IT_CYCLE: {
            int k = opt_index(it), n = 0;
            while (it->opts[n]) n++;
            cfg_set(it->key, it->opts[(k + 1) % n]);
            if (it->changed) it->changed();
            break;
        }
        case IT_TEXT:
            text_item = it;
            keyboard_open(T(it->label), cfg_get(it->key, it->def), strstr(it->key, "pass") != NULL, text_done, NULL);
            break;
        case IT_SUB:
            s->sub = it->sub;
            s->item = 0;
            s->scroll = 0;
            s->item = first_selectable(cur_sec(s));
            break;
        case IT_ACTION:
            if (it->action) it->action();
            break;
        default: break;
        }
        return true;
    default: return false;
    }
}

Screen *settings_screen(void) {
    rebuild_apps();
    Screen *s = calloc(1, sizeof *s);
    s->name = "settings";
    Settings *st = calloc(1, sizeof(Settings));
    st->sub = -1;
    st->hl_sec_id = -999; /* the item pill starts on the right row (no slide from the top) */
    s->st = st;
    s->enter = settings_enter;
    s->resume = settings_enter;
    s->update = settings_update;
    s->draw = settings_draw;
    s->input = settings_input;
    s->destroy = settings_destroy;
    return s;
}

/* Settings opened on one section (0 = Network, 1 = Picture and sound, 3 = Bluetooth ...) with the list in focus */
Screen *settings_screen_at(int sec) {
    Screen *s = settings_screen();
    Settings *st = s->st;
    if (sec >= 0 && sec < N_SEC) {
        st->sec = sec;
        st->col = 1;
        st->col_f = 1;
        st->hl_sec = (Spring){sec * 76.0f, 0};
        st->item = first_selectable(cur_sec(st));
    }
    return s;
}
