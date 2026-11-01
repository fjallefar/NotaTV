/* NotaTV shell - settings */
#include "tv.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum { IT_INFO, IT_TOGGLE, IT_CYCLE, IT_ACTION, IT_TEXT } ItemType;

typedef struct {
    const char *label;
    ItemType type;
    const char *key, *def;
    const char *const *opts, *const *labels;
    void (*action)(void);
    const char *info_cmd; /* helper command that gives the value (IT_INFO) */
    void (*changed)(void);
    const char *help;
    const char *(*value)(void); /* value computed when drawing (IT_INFO/IT_ACTION) */
} Item;

typedef struct { const char *name; IconId icon; const Item *items; int n; } Section;

/* ---------- handlinger ---------- */
static void helper_bg(const char *args) { run_cmd("%s/%s >/dev/null 2>&1 &", helper_path(), args); }

/* busy overlay while a job runs */
typedef struct { char title[128]; bool done; } Busy;
static Screen *busy_screen;
static void busy_draw(Screen *s) {
    Busy *b = s->st;
    fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(190 * s->anim)));
    fill_rrect(UI_W / 2 - 420, UI_H / 2 - 150, 840, 300, 28, RGB(24, 28, 37));
    draw_spinner(UI_W / 2, UI_H / 2 - 40, 30, C_TEXT);
    text_draw(F_SEMI, 30, UI_W / 2, UI_H / 2 + 30, C_TEXT, AL_CENTER, b->title);
    FILE *f = fopen("/tmp/notatv-progress", "r");
    if (f) {
        char line[200] = "";
        if (fgets(line, sizeof line, f)) { str_trim(line); text_draw(F_REG, 22, UI_W / 2, UI_H / 2 + 80, C_TEXT2, AL_CENTER, tr_msg(line)); }
        fclose(f);
    }
}
static bool busy_input(Screen *s, const InputEvent *ev) { (void)s; (void)ev; return true; }
static void busy_open(const char *title) {
    unlink("/tmp/notatv-progress");
    Screen *s = calloc(1, sizeof *s);
    Busy *b = calloc(1, sizeof *b);
    snprintf(b->title, sizeof b->title, "%s", title);
    s->name = "busy";
    s->st = b;
    s->draw = busy_draw;
    s->input = busy_input;
    s->overlay = true;
    busy_screen = s;
    app_push(s);
}
static void busy_close(void) {
    if (busy_screen && app_top() == busy_screen) app_pop();
    busy_screen = NULL;
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
static void forget_cb(void *ud, bool yes) { (void)ud; if (yes) bt_forget_all(); }
/* YouTube: "Link with TV code" (the code is fetched from YouTube in the background) */
typedef struct { char *code; } YtPair;
static void ytpair_work(void *a) { ((YtPair *)a)->code = ytcast_pairing_code(); }
static void ytpair_ok(void *ud, bool yes) { (void)ud; (void)yes; }
static void ytpair_done(void *a) {
    YtPair *p = a;
    if (!p->code) app_toast(T("Could not get a code from YouTube - try again"));
    else {
        char *msg = str_printf(T("Open YouTube on your phone: Settings → Watch on TV → Link with TV code, and enter\n\n%s\n\nThe code works for about 5 minutes."), p->code);
        confirm_open(T("YouTube: link with TV code"), msg, "OK", T("Close"), ytpair_ok, NULL);
        free(msg);
    }
    free(p->code);
    free(p);
}
static void act_ytpair(void) {
    app_toast(T("Getting code from YouTube…"));
    job_submit(ytpair_work, ytpair_done, calloc(1, sizeof(YtPair)));
}
static void act_bt_forget(void) { confirm_open(T("Forget all Bluetooth devices?"), T("The phones will need to be paired again afterwards."), T("Forget"), T("Cancel"), forget_cb, NULL); }

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
static void restart_music(void) { helper_bg("music restart"); }
static void apply_name(void) { helper_bg("music restart"); helper_bg("cec setup"); bt_apply(); dlna_apply(); }
void settings_apply_name(void) { apply_name(); }
static void apply_share(void) { helper_bg("automount"); app_toast(T("Mounting network drive…")); }
static void apply_iptv(void) { app_toast(T("Playlist saved - open Live TV")); }
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

/* --- Plex --- */
static void act_xtream(void) { iptv_xtream_login(); }

static void act_plex_logout(void) {
    cfg_set("plex_token", "");
    cfg_set("plex_server_uri", "");
    cfg_set("plex_server_token", "");
    app_toast(T("Signed out of Plex"));
}

/* --- system --- */
static char **inst_devs;
static int n_inst;
static int inst_sel;
static void install_finished(char *out, void *ud);
static void install_confirm2(void *ud, bool yes) {
    (void)ud;
    if (!yes) return;
    busy_open(T("Installing NotaTV…"));
    run_async(install_finished, NULL, "install run %s", inst_devs[inst_sel]);
}
static void install_confirm1(void *ud, bool yes) {
    (void)ud;
    if (!yes) return;
    char msg[300];
    snprintf(msg, sizeof msg, T("EVERYTHING on %s will be permanently erased. Are you absolutely sure?"), inst_devs[inst_sel]);
    confirm_open(T("Last chance"), msg, T("Erase and install"), T("Cancel"), install_confirm2, NULL);
}
static void install_pick(void *ud, int idx) {
    (void)ud;
    if (idx < 0) return;
    inst_sel = idx;
    char msg[300];
    snprintf(msg, sizeof msg, T("NotaTV will be installed on %s. The disk will be erased."), inst_devs[idx]);
    confirm_open(T("Install on internal disk?"), msg, T("Continue"), T("Cancel"), install_confirm1, NULL);
}
static void reboot_cb(void *ud, bool yes) { (void)ud; if (yes) helper_bg("power reboot"); }
static void install_finished(char *out, void *ud) {
    (void)ud;
    busy_close();
    if (strstr(out, "OK")) confirm_open(T("Installation complete"), T("Remove the USB stick and restart to boot from the internal disk."), T("Restart now"), T("Later"), reboot_cb, NULL);
    else {
        char *last = strrchr(out, '\n');
        while (last && last > out && !last[1]) { *last = 0; last = strrchr(out, '\n'); }
        app_toast(T("Installation failed: %s"), tr_msg(last ? last + 1 : out));
    }
}
static void install_listed(char *out, void *ud) {
    (void)ud;
    for (int i = 0; i < n_inst; i++) free(inst_devs[i]);
    free(inst_devs);
    char **lines;
    int n = split_lines(out, &lines);
    inst_devs = calloc(n + 1, sizeof(char *));
    char **labels = calloc(n + 1, sizeof(char *));
    n_inst = 0;
    for (int i = 0; i < n; i++) {
        char *dev = strtok(lines[i], "\t"), *size = strtok(NULL, "\t"), *model = strtok(NULL, "\t");
        if (!dev || strncmp(dev, "/dev/", 5)) continue;
        inst_devs[n_inst] = str_dup(dev);
        labels[n_inst] = str_printf("%s  ·  %s  ·  %s", dev, size ? size : "?", model ? model : "");
        n_inst++;
    }
    if (n_inst == 0) app_toast(T("No suitable disks found"));
    else choice_open(T("Choose disk"), (const char *const *)labels, n_inst, 0, install_pick, NULL);
    for (int i = 0; i < n_inst; i++) free(labels[i]);
    free(labels);
    free(lines);
}
static void act_install(void) { run_async(install_listed, NULL, "install list"); }

static void update_done(char *out, void *ud) {
    (void)ud;
    busy_close();
    str_trim(out);
    const char *last = strrchr(out, '\n');
    if (!strcmp(last ? last + 1 : out, "OK")) confirm_open(T("Update installed"), T("Restart to use the new version."), T("Restart now"), T("Later"), reboot_cb, NULL);
    else app_toast("%s", *out ? tr_msg(out) : T("No update found"));
}
static void act_update_usb(void) {
    busy_open(T("Looking for an update on USB…"));
    run_async(update_done, NULL, "update usb");
}
/* GitHub update (Pi): check -> ask -> install -> reboot */
static void gh_installed(char *out, void *ud) { update_done(out, ud); }
static void gh_install_cb(void *ud, bool yes) {
    (void)ud;
    if (!yes) return;
    busy_open(T("Downloading and installing update…"));
    run_async(gh_installed, NULL, "update install");
}
static void gh_checked(char *out, void *ud) {
    (void)ud;
    busy_close();
    str_trim(out);
    if (!strncmp(out, "NY ", 3)) {
        char t[96];
        snprintf(t, sizeof t, T("NotaTV %s is available"), out + 3);
        confirm_open(t, T("The update will be downloaded and installed. Your settings and logins are kept."), T("Update"), T("Later"), gh_install_cb, NULL);
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
    confirm_open(T("Undo last update?"), T("NotaTV goes back to the version from before the last update. Your settings and logins are kept."),
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
static void act_oc_apply(void) {
    char title[96];
    snprintf(title, sizeof title, T("Restart at %s MHz?"), cfg_get("cpu_mhz", "1500"));
    confirm_open(title, T("If the box does not start (black screen for more than 2 minutes): turn it off and on again, and it will continue at the old frequency."), T("Restart"), T("Cancel"), oc_cb, NULL);
}

static void act_sleep(void) { helper_bg("power sleep"); }
static void act_reboot(void) { helper_bg("power reboot"); }
static void act_poweroff(void) { helper_bg("power poweroff"); }

/* ---------- sektioner ---------- */
static const char *const onoff[] = {"0", "1", NULL};
static const char *const rate_o[] = {"50", "60", NULL}, *const rate_l[] = {"50 Hz", "60 Hz", NULL};
static const char *const lang_o[] = {"da,dan,dk,en,eng", "en,eng,da,dan", "sv,swe,en,eng", "no,nor,nob,en,eng", "de,deu,ger,en,eng", "no-subs", NULL};
static const char *const lang_l[] = {N_("Danish"), N_("English"), N_("Swedish"), N_("Norwegian"), N_("German"), N_("None"), NULL};
static const char *const alang_o[] = {"da,dan,en,eng", "en,eng", "sv,swe,en,eng", "no,nor,en,eng", "de,deu,ger,en,eng", NULL};
static const char *const alang_l[] = {N_("Danish"), N_("English"), N_("Swedish"), N_("Norwegian"), N_("German"), NULL};
static const char *const ytq_o[] = {"2160", "1440", "1080", "720", NULL}, *const ytq_l[] = {"4K", "1440p", "1080p", "720p", NULL};
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
static const char *const hdrm_o[] = {"apple", "film", NULL}, *const hdrm_l[] = {N_("Like Apple TV"), N_("From the movie"), NULL};
static const Item av_items[] = {
    {N_("Display"), IT_INFO, .info_cmd = "display info"},
    {"HDR", IT_TOGGLE, "hdr", "1", onoff, .help = N_("Send an HDR signal to the TV for HDR content")},
    {N_("Match frame rate"), IT_TOGGLE, "match_rate", "1", onoff, .help = N_("Switch the TV to the movie's frame rate (e.g. 23.976 Hz) - avoids judder")},
    {N_("Frame rate for live TV"), IT_CYCLE, "iptv_rate", "50", rate_o, rate_l},
    {N_("Always HDR"), IT_TOGGLE, "hdr_always", "0", onoff, .changed = apply_hdr_always,
     .help = N_("The TV stays in HDR all the time - menus too - so it never switches. Menus are converted to HDR")},
    {N_("HDR brightness metadata"), IT_CYCLE, "hdr_meta", "apple", hdrm_o, hdrm_l,
     .help = N_("Like Apple TV: fixed values (MaxCLL 4000) - brighter on many TVs. From the movie: exactly as the movie was mastered")},
    {N_("Audio output"), IT_ACTION, .action = act_audio_dev},
    {N_("Surround passthrough"), IT_TOGGLE, "audio_passthrough", "0", onoff, .help = N_("Send Dolby/DTS unchanged to a receiver or soundbar")},
    {N_("DTS passthrough"), IT_TOGGLE, "pt_dts", "1", onoff},
    {"TrueHD / DTS-HD (Atmos)", IT_TOGGLE, "pt_hd", "0", onoff, .help = N_("Requires a receiver or soundbar that supports HD audio")},
    {N_("Audio delay (lip sync)"), IT_CYCLE, "audio_delay_ms", "0", delay_o, delay_l},
    {N_("Subtitle language"), IT_CYCLE, "sub_lang", "da,dan,dk,en,eng", lang_o, lang_l},
    {N_("Audio language"), IT_CYCLE, "audio_lang", "da,dan,en,eng", alang_o, alang_l},
    {N_("Play test sound"), IT_ACTION, .action = act_audio_test},
};
static const Item bt_items[] = {
    {"Bluetooth", IT_TOGGLE, "bt_enabled", "1", onoff, .changed = bt_apply, .help = N_("Use the TV as a Bluetooth speaker for phones")},
    {N_("Only visible while Music is open"), IT_TOGGLE, "bt_ondemand", "1", onoff, .changed = bt_apply,
     .help = N_("Phones can only find and connect to the TV while Music or \"Pair a phone\" is open (and for 2 min. after). Paired controllers and keyboards can always connect. Turn off if phones should be able to connect on their own")},
    {N_("Pair a phone"), IT_ACTION, .action = act_bt_pair, .help = N_("Makes the TV visible for 3 minutes. The code is confirmed here on the TV")},
    {N_("Forget paired devices"), IT_ACTION, .action = act_bt_forget},
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
static const char *const plexst_o[] = {"hero", "focus", NULL}, *const plexst_l[] = {"Hero", N_("Focus"), NULL};
static const Item app_items[] = {
    {N_("Jellyfin: play from phone"), IT_TOGGLE, "jf_cast", "1", onoff, .help = N_("NotaTV appears under \"Play On\" in the Jellyfin app when you are signed in to Jellyfin here")},
    {N_("DLNA (send from apps)"), IT_TOGGLE, "dlna_enabled", "1", onoff, .changed = dlna_apply, .help = N_("Apps like BubbleUPnP and \"Cast to Device\" in Windows can send video, music and pictures. Local network only")},
    {N_("AirPlay screen mirroring"), IT_TOGGLE, "airplay_mirror", "1", onoff, .changed = mirror_apply, .help = N_("iPhone/iPad/Mac can mirror the screen to the TV (shown as \"<name> (screen)\"). Local network only")},
    {N_("YouTube cast"), IT_TOGGLE, "ytcast_enabled", "1", onoff, .changed = ytcast_apply, .help = N_("The cast button in the YouTube app can play on NotaTV. Control goes through YouTube (Google)")},
    {N_("YouTube: link with TV code"), IT_ACTION, .action = act_ytpair, .help = N_("If NotaTV does not show up under Cast in the YouTube app: link with a code")},
    {N_("Plex cast"), IT_TOGGLE, "plexcast_enabled", "1", onoff, .changed = plexcast_apply, .help = N_("The Plex app on your phone can play on NotaTV (cast button) and control playback. Local network only")},
    {N_("Cover art for Bluetooth music"), IT_TOGGLE, "music_cover_lookup", "1", onoff, .help = N_("Looks up cover art in MusicBrainz (open music database). Sends artist and album there")},
    {N_("AirPlay (music from phone)"), IT_TOGGLE, "airplay_enabled", "1", onoff, .changed = restart_music, .help = N_("iPhone, iPad and Mac can play music on the TV. Local network only")},
    {N_("YouTube max quality"), IT_CYCLE, "yt_quality", "2160", ytq_o, ytq_l},
    {N_("YouTube subtitles"), IT_TOGGLE, "yt_subs", "0", onoff, .help = N_("Downloads English subtitles - makes start about 2 sec. slower")},
    {N_("IPTV: sign in with Xtream Codes"), IT_ACTION, .action = act_xtream, .help = N_("Server, username and password from your IPTV provider")},
    {N_("IPTV playlist (M3U link)"), IT_TEXT, "iptv_url", "", .changed = apply_iptv},
    {N_("IPTV program guide (XMLTV)"), IT_TEXT, "iptv_epg", ""},
    {N_("Network drive"), IT_TEXT, "media_share", "", .changed = apply_share, .help = N_("smb://user:password@server/share or nfs://server/path")},
    {N_("OpenSubtitles API key"), IT_TEXT, "os_apikey", "", .help = N_("Free: create an account on opensubtitles.com → Consumers → new API key")},
    {N_("OpenSubtitles username"), IT_TEXT, "os_user", ""},
    {N_("OpenSubtitles password"), IT_TEXT, "os_pass", ""},
    {N_("Home screen (Plex and Jellyfin)"), IT_CYCLE, "plex_home_style", "hero", plexst_o, plexst_l, .help = N_("Hero: a featured title at the top. Focus: background and info follow the selected title")},
    {N_("Plex server"), IT_INFO, .info_cmd = "plex-name"},
    {N_("Sign out of Plex"), IT_ACTION, .action = act_plex_logout},
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
static const Item sys_items[] = {
    {N_("Language"), IT_ACTION, .action = act_language, .value = lang_value, .help = N_("The language of the menus. The user interface restarts when you change it")},
    {N_("Version"), IT_INFO, .info_cmd = "sys version"},
    {N_("Boot time"), IT_INFO, .info_cmd = "sys boottime"},
    {N_("Uptime"), IT_INFO, .info_cmd = "sys uptime"},
    {N_("Hardware"), IT_INFO, .info_cmd = "sys hw"},
    {N_("Running from"), IT_INFO, .info_cmd = "sys mode"},
    {N_("Run the setup guide again"), IT_ACTION, .action = act_setup, .help = N_("Language, country, time, network, name and apps - like the first start")},
    {N_("Screensaver"), IT_CYCLE, "screensaver_min", "10", ss_o, ss_l},
    {N_("Show FPS"), IT_TOGGLE, "show_fps", "0", onoff, .help = N_("Frames per second and slowest frame (ms) in the top right - for troubleshooting")},
    {N_("Developer mode (SSH)"), IT_TOGGLE, "ssh_enabled", "0", onoff, .changed = apply_ssh, .help = N_("SSH login as root (password: notatv)")},
    {N_("Install on internal disk"), IT_ACTION, .action = act_install, .help = N_("Copies NotaTV from USB to the box's SSD")},
    {N_("Update from USB"), IT_ACTION, .action = act_update_usb, .help = N_("Put notatv.efi or a new ISO on a USB stick")},
    {N_("Sleep"), IT_ACTION, .action = act_sleep},
    {N_("Restart"), IT_ACTION, .action = act_reboot},
    {N_("Power off"), IT_ACTION, .action = act_poweroff},
    {N_("Reset to factory settings"), IT_ACTION, .action = act_reset},
};
/* Pi: updated from GitHub releases - no installing/updating from USB */
static const Item sys_items_rpi[] = {
    {N_("Language"), IT_ACTION, .action = act_language, .value = lang_value, .help = N_("The language of the menus. The user interface restarts when you change it")},
    {N_("Version"), IT_INFO, .info_cmd = "sys version"},
    {N_("Boot time"), IT_INFO, .info_cmd = "sys boottime"},
    {N_("Uptime"), IT_INFO, .info_cmd = "sys uptime"},
    {N_("Hardware"), IT_INFO, .info_cmd = "sys hw"},
    {N_("Running from"), IT_INFO, .info_cmd = "sys mode"},
    {N_("Run the setup guide again"), IT_ACTION, .action = act_setup, .help = N_("Language, country, time, network, name and apps - like the first start")},
    {N_("Screensaver"), IT_CYCLE, "screensaver_min", "10", ss_o, ss_l},
    {N_("Show FPS"), IT_TOGGLE, "show_fps", "0", onoff, .help = N_("Frames per second and slowest frame (ms) in the top right - for troubleshooting")},
    {N_("Developer mode (SSH)"), IT_TOGGLE, "ssh_enabled", "0", onoff, .changed = apply_ssh, .help = N_("SSH login as root (password: notatv)")},
    {N_("Update NotaTV"), IT_ACTION, .action = act_update_gh, .help = N_("Looks for a new version on GitHub and installs it")},
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
static const char *const mres_o[] = {"1280x720", "1920x1080", "2560x1440", "3840x2160", NULL};
static const char *const mres_l[] = {"720p", "1080p", "1440p", "4K", NULL};
static const char *const mfps_o[] = {"30", "60", "90", "120", NULL}, *const mfps_l[] = {"30", "60", "90", "120", NULL};
static const char *const mbr_o[] = {"5000", "10000", "15000", "20000", "30000", "40000", "60000", "80000", NULL};
static const char *const mbr_l[] = {"5 Mbit/s", "10 Mbit/s", "15 Mbit/s", "20 Mbit/s", "30 Mbit/s", "40 Mbit/s", "60 Mbit/s", "80 Mbit/s", NULL};
static const char *const mcod_o[] = {"auto", "hevc", "h264", NULL}, *const mcod_l[] = {N_("Automatic (HEVC)"), "HEVC", N_("H.264 (slow)"), NULL};
static const char *const mmode_o[] = {"", "1920x1080@120", "1920x1080@60", "3840x2160@60", NULL};
static const char *const mmode_l[] = {N_("Keep the UI's"), N_("1080p 120 Hz (lowest latency)"), "1080p 60 Hz", "4K 60 Hz", NULL};
static const Item game_items[] = {
    {N_("Resolution"), IT_CYCLE, "moon_res", "1920x1080", mres_o, mres_l, .help = N_("The resolution the PC streams in")},
    {N_("Frames per second"), IT_CYCLE, "moon_fps", "60", mfps_o, mfps_l, .help = N_("Above 60 requires the display mode to be 120 Hz")},
    {"Bitrate", IT_CYCLE, "moon_bitrate", "20000", mbr_o, mbr_l, .help = N_("Higher = sharper picture, but more data on the network")},
    {"Codec", IT_CYCLE, "moon_codec", "auto", mcod_o, mcod_l, .help = N_("HEVC decodes in ~4 ms, H.264 takes ~67 ms on a Pi 4")},
    {N_("Display mode while gaming"), IT_CYCLE, "moon_mode", "1920x1080@120", mmode_o, mmode_l, .help = N_("120 Hz halves the wait for the next frame. The TV switches mode at start")},
    {"HDR", IT_TOGGLE, "moon_hdr", "0", onoff, .help = N_("HEVC 10-bit HDR, if the PC's display is in HDR")},
};
#define NI(a) (int)(sizeof(a) / sizeof(a[0]))
static Section sections[] = { /* not const: the System items are swapped on the Pi (settings_screen) */
    {N_("Network"), ICON_WIFI, net_items, NI(net_items)},
    {N_("Picture and sound"), ICON_FILM, av_items, NI(av_items)},
    {"HDMI-CEC", ICON_TV, cec_items, NI(cec_items)},
    {"Bluetooth", ICON_MUSIC, bt_items, NI(bt_items)},
    {N_("Apps"), ICON_PLAY, app_items, NI(app_items)},
    {N_("Games"), ICON_PLAY, game_items, NI(game_items)},
    {N_("Date and time"), ICON_CLOCK, time_items, NI(time_items)},
    {N_("System"), ICON_GEAR, sys_items, NI(sys_items)},
    {"Overclock", ICON_POWER, oc_items, NI(oc_items)}, /* Pi only - must be last (see N_SEC) */
};
#define N_SEC (NI(sections) - (board_is_rpi() ? 0 : 1))
#define MAX_ITEMS 32 /* the most items in one section (Apps has 20) */
_Static_assert(NI(net_items) <= MAX_ITEMS && NI(av_items) <= MAX_ITEMS && NI(cec_items) <= MAX_ITEMS && NI(bt_items) <= MAX_ITEMS &&
               NI(app_items) <= MAX_ITEMS && NI(sys_items) <= MAX_ITEMS && NI(sys_items_rpi) <= MAX_ITEMS && NI(oc_items) <= MAX_ITEMS, "øg MAX_ITEMS");

/* ---------- screen ---------- */
typedef struct {
    int col, sec, item;
    float scroll;
    char *info[MAX_ITEMS];
    int info_sec;
    double info_t;
    bool info_loading;
} Settings;

typedef struct { Settings *s; int sec; char *vals[MAX_ITEMS]; int token; } InfoJob;
static int settings_token;

static void info_work(void *arg) {
    InfoJob *j = arg;
    const Section *sc = &sections[j->sec];
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
    if (j->token == settings_token && j->s->info_sec == j->sec) {
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
    j->sec = s->info_sec = s->sec;
    j->token = settings_token;
    job_submit(info_work, info_done, j);
}

static void settings_enter(Screen *sc) {
    Settings *s = sc->st;
    settings_token++;
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

static void settings_update(Screen *sc, float dt) {
    Settings *s = sc->st;
    if (s->info_sec != s->sec || app_time() - s->info_t > 4) {
        if (s->info_sec != s->sec) {
            for (int i = 0; i < MAX_ITEMS; i++) { free(s->info[i]); s->info[i] = NULL; }
            s->info_loading = false;
        }
        info_refresh(s);
    }
    float target = s->item * 88 - 88 * 5;
    if (target < 0) target = 0;
    s->scroll = anim_to(s->scroll, target, dt, 14);
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
    for (int i = 0; i < N_SEC; i++) {
        float y = 170 + i * 76;
        bool sel = i == s->sec, f = sel && s->col == 0;
        if (f) fill_rrect(80, y, 440, 64, 14, C_FOCUS);
        else if (sel) fill_rrect(80, y, 440, 64, 14, C_SURFACE2);
        Color c = f ? RGB(10, 10, 10) : sel ? C_TEXT : C_TEXT2;
        draw_icon(sections[i].icon, 122, y + 32, 30, c);
        text_draw(F_SEMI, 28, 160, y + 15, c, AL_LEFT, T(sections[i].name));
    }
    const Section *sec = &sections[s->sec];
    float x = 580, w = UI_W - x - 80;
    clip_push(x - 10, 150, w + 20, UI_H - 240);
    for (int i = 0; i < sec->n; i++) {
        const Item *it = &sec->items[i];
        float y = 170 + i * 88 - s->scroll;
        if (y < 80 || y > UI_H) continue;
        bool f = s->col == 1 && s->item == i;
        bool info = it->type == IT_INFO;
        if (f) fill_rrect(x, y, w, 76, 16, C_FOCUS);
        else if (!info) fill_rrect(x, y, w, 76, 16, C_SURFACE);
        Color lc = f ? RGB(10, 10, 10) : info ? C_TEXT2 : C_TEXT;
        Color vc = f ? RGB(50, 50, 55) : C_TEXT2;
        text_draw(F_SEMI, 28, x + 32, y + 21, lc, AL_LEFT, T(it->label));
        float vx = x + w - 32;
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
            text_draw_clip(F_REG, 24, vx, y + 24, w * 0.5f, vc, AL_RIGHT, !*v ? T("Not set") : secret ? "••••••••" : v);
            break;
        }
        case IT_ACTION:
            fill_tri(vx - 14, y + 28, vx - 2, y + 38, vx - 14, y + 48, vc);
            if (it->value) text_draw_clip(F_REG, 26, vx - 30, y + 23, w * 0.5f, vc, AL_RIGHT, it->value());
            break;
        }
    }
    clip_pop();
    if (s->col == 1 && s->item < sec->n && sec->items[s->item].help)
        text_draw(F_REG, 24, x + 32, UI_H - 70, C_TEXT3, AL_LEFT, T(sec->items[s->item].help));
}

static int next_selectable(const Section *sec, int from, int dir) {
    int i = from + dir;
    while (i >= 0 && i < sec->n && sec->items[i].type == IT_INFO) i += dir;
    return i >= 0 && i < sec->n ? i : from;
}

static bool settings_input(Screen *sc, const InputEvent *ev) {
    Settings *s = sc->st;
    const Section *sec = &sections[s->sec];
    if (s->col == 0) {
        switch (ev->a) {
        case A_UP: if (s->sec > 0) s->sec--; s->item = 0; return true;
        case A_DOWN: if (s->sec < N_SEC - 1) s->sec++; s->item = 0; return true;
        case A_RIGHT: case A_OK: {
            s->col = 1;
            s->item = sec->items[0].type == IT_INFO ? next_selectable(sec, 0, 1) : 0;
            return true;
        }
        default: return false;
        }
    }
    const Item *it = &sec->items[s->item];
    switch (ev->a) {
    case A_UP: s->item = next_selectable(sec, s->item, -1); return true;
    case A_DOWN: s->item = next_selectable(sec, s->item, 1); return true;
    case A_BACK: s->col = 0; return true;
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
            cfg_set_int(it->key, ev->a == A_RIGHT);
            if (it->changed) it->changed();
            return true;
        }
        if (ev->a == A_LEFT) { s->col = 0; return true; }
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
            keyboard_open(T(it->label), cfg_get(it->key, it->def), false, text_done, NULL);
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
    if (board_is_rpi())
        for (int i = 0; i < NI(sections); i++)
            if (sections[i].items == sys_items) { sections[i].items = sys_items_rpi; sections[i].n = NI(sys_items_rpi); }
    Screen *s = calloc(1, sizeof *s);
    s->name = "settings";
    s->st = calloc(1, sizeof(Settings));
    s->enter = settings_enter;
    s->resume = settings_enter;
    s->update = settings_update;
    s->draw = settings_draw;
    s->input = settings_input;
    s->destroy = settings_destroy;
    return s;
}
