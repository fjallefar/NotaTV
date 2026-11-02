/* NotaTV shell - Bluetooth via BlueZ (D-Bus): the box as a speaker for phones (A2DP via bluez-alsa).
 * - agent: pairing must be approved on the TV (the same code as on the phone)
 * - "Now playing": track info and state from the phone (AVRCP / MediaPlayer1, MediaTransport1)
 * - control: pause/play/next/previous are sent back to the phone
 * Everything runs on the main thread: bt_tick() handles D-Bus messages without blocking. */
#define _GNU_SOURCE /* strchrnul */
#include "tv.h"
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "Now playing" info goes to the Music app - only when it is installed (see MusicIface in tv.h) */
#define MUSIC(fn, ...) do { const MusicIface *mi_ = app_iface("music"); if (mi_) mi_->fn(__VA_ARGS__); } while (0)

#define ADAPTER "/org/bluez/hci0"
#define AGENT_PATH "/notatv/agent"

static DBusConnection *bus;
static bool agent_ok, agent_pending;
static double next_try;
static char player_path[256];     /* the phone's MediaPlayer1 (for control) */
static char transport_dev[128];   /* device whose audio connection is active */
static double pair_until;         /* parringstilstand slutter */
/* "only when in use" (cfg bt_ondemand): the box is invisible and cannot be connected to, unless something is
 * connected, pairing is going on, or it has been woken (Music open, connect from the TV) */
static char conn_dev[8][128];     /* connected devices (phones, keyboards ...) */
static int n_conn;
static double hold_until;         /* the radio is kept on until (woken) */
static bool radio_on;

/* ---------- small helpers ---------- */
static void track_conn(const char *dev, bool on) {
    int i;
    for (i = 0; i < n_conn && strcmp(conn_dev[i], dev); i++) {}
    if (on && i == n_conn && n_conn < 8) snprintf(conn_dev[n_conn++], sizeof conn_dev[0], "%s", dev);
    if (!on && i < n_conn) {
        conn_dev[i][0] = 0;
        if (i < n_conn - 1) memcpy(conn_dev[i], conn_dev[n_conn - 1], sizeof conn_dev[0]);
        n_conn--;
        if (hold_until < app_time() + 30) hold_until = app_time() + 30; /* short grace period: the phone may come back */
    }
}

static void send_call(DBusMessage *m) {
    if (bus && m) dbus_connection_send(bus, m, NULL);
    if (m) dbus_message_unref(m);
}

static void set_prop(const char *path, const char *iface, const char *name, int type, const void *val) {
    DBusMessage *m = dbus_message_new_method_call("org.bluez", path, "org.freedesktop.DBus.Properties", "Set");
    if (!m) return;
    DBusMessageIter it, v;
    char sig[2] = {(char)type, 0};
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &v);
    dbus_message_iter_append_basic(&v, type, val);
    dbus_message_iter_close_container(&it, &v);
    send_call(m);
}
static void set_bool(const char *path, const char *iface, const char *name, bool on) {
    dbus_bool_t b = on;
    set_prop(path, iface, name, DBUS_TYPE_BOOLEAN, &b);
}

/* blocking Properties.Get of a string (short timeout; rarely used) */
static void get_str(const char *path, const char *iface, const char *name, char *out, size_t n) {
    out[0] = 0;
    if (!bus) return;
    DBusMessage *m = dbus_message_new_method_call("org.bluez", path, "org.freedesktop.DBus.Properties", "Get");
    if (!m) return;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 400, NULL);
    dbus_message_unref(m);
    if (!r) return;
    DBusMessageIter it, v;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &v);
        if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) {
            const char *s;
            dbus_message_iter_get_basic(&v, &s);
            snprintf(out, n, "%s", s);
        }
    }
    dbus_message_unref(r);
}

static void device_name(const char *dev_path, char *out, size_t n) {
    get_str(dev_path, "org.bluez.Device1", "Alias", out, n);
    if (!out[0]) snprintf(out, n, T("a phone"));
}

/* /org/bluez/hci0/dev_XX_XX/... -> /org/bluez/hci0/dev_XX_XX */
static void dev_of(const char *path, char *out, size_t n) {
    const char *d = strstr(path, "/dev_");
    size_t l = d ? (size_t)(strchrnul(d + 1, '/') - path) : strlen(path);
    snprintf(out, n, "%.*s", (int)(l < n ? l : n - 1), path);
}

/* ---------- agent: pairing is approved on the TV ---------- */
static DBusMessage *pending_req; /* waiting for the user's answer */

static void answer(bool yes) {
    if (!pending_req) return;
    DBusMessage *r = yes ? dbus_message_new_method_return(pending_req)
                         : dbus_message_new_error(pending_req, "org.bluez.Error.Rejected", T("Declined on the TV"));
    if (yes) { /* trust the device: it may reconnect without asking */
        const char *dev = NULL;
        if (dbus_message_get_args(pending_req, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_INVALID) && dev)
            set_bool(dev, "org.bluez.Device1", "Trusted", true);
    }
    send_call(r);
    dbus_message_unref(pending_req);
    pending_req = NULL;
}
static void confirm_cb(void *ud, bool yes) { (void)ud; answer(yes); app_toast(yes ? T("Paired") : T("Pairing declined")); }

static DBusHandlerResult agent_msg(DBusConnection *c, DBusMessage *m, void *ud) {
    (void)c; (void)ud;
    if (!dbus_message_has_interface(m, "org.bluez.Agent1")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *mem = dbus_message_get_member(m);
    const char *dev = NULL;
    dbus_uint32_t pk = 0;
    if (!strcmp(mem, "RequestConfirmation") &&
        dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_UINT32, &pk, DBUS_TYPE_INVALID)) {
        if (pending_req) answer(false);
        pending_req = dbus_message_ref(m);
        char name[128], msg[300];
        device_name(dev, name, sizeof name);
        snprintf(msg, sizeof msg, T("Code: %06u\n\nDoes the code match the one on %s?"), pk, name);
        confirm_open(T("Bluetooth pairing"), msg, T("Pair"), T("Decline"), confirm_cb, NULL);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(mem, "RequestAuthorization") && dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_INVALID)) {
        if (pending_req) answer(false);
        pending_req = dbus_message_ref(m);
        char name[128], msg[300];
        device_name(dev, name, sizeof name);
        snprintf(msg, sizeof msg, T("%s wants to connect to the TV."), name);
        confirm_open(T("Bluetooth pairing"), msg, T("Allow"), T("Decline"), confirm_cb, NULL);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    DBusMessage *r;
    if (!strcmp(mem, "AuthorizeService")) {
        /* only while pairing or for devices we have already approved (they are "Trusted" and are not asked) */
        r = app_time() < pair_until ? dbus_message_new_method_return(m)
                                    : dbus_message_new_error(m, "org.bluez.Error.Rejected", T("Not in pairing mode"));
    } else if (!strcmp(mem, "Cancel")) {
        if (pending_req) { dbus_message_unref(pending_req); pending_req = NULL; }
        app_toast(T("Pairing cancelled"));
        r = dbus_message_new_method_return(m);
    } else if (!strcmp(mem, "Release") || !strcmp(mem, "DisplayPasskey") || !strcmp(mem, "DisplayPinCode")) {
        if (!strcmp(mem, "Release")) agent_ok = false;
        r = dbus_message_new_method_return(m);
    } else { /* RequestPinCode/RequestPasskey: old-style pairing with typing - not supported */
        r = dbus_message_new_error(m, "org.bluez.Error.Rejected", T("Not supported"));
    }
    send_call(r);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static void registered(DBusPendingCall *pc, void *ud) {
    (void)ud;
    DBusMessage *r = dbus_pending_call_steal_reply(pc);
    agent_pending = false;
    if (r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        agent_ok = true;
        const char *p = AGENT_PATH;
        DBusMessage *m = dbus_message_new_method_call("org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent");
        if (m) { dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID); send_call(m); }
        /* already connected devices (e.g. tvshell restarted during music): the radio must not be turned off under them */
        BtDevice d[8];
        int n = bt_devices(d, 8);
        for (int i = 0; i < n; i++) if (d[i].connected) track_conn(d[i].path, true);
        bt_apply();
    }
    if (r) dbus_message_unref(r);
    dbus_pending_call_unref(pc);
}

static void register_agent(void) {
    const char *p = AGENT_PATH, *cap = "DisplayYesNo";
    DBusMessage *m = dbus_message_new_method_call("org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent");
    if (!m) return;
    dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_STRING, &cap, DBUS_TYPE_INVALID);
    DBusPendingCall *pc = NULL;
    if (dbus_connection_send_with_reply(bus, m, &pc, 3000) && pc) {
        agent_pending = true;
        dbus_pending_call_set_notify(pc, registered, NULL, NULL);
    }
    dbus_message_unref(m);
}

/* ---------- playback: signals from BlueZ ---------- */
static bool variant_str(DBusMessageIter *v, const char **s) {
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_STRING && dbus_message_iter_get_arg_type(v) != DBUS_TYPE_OBJECT_PATH) return false;
    dbus_message_iter_get_basic(v, s);
    return true;
}
static bool variant_u32(DBusMessageIter *v, dbus_uint32_t *u) {
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_UINT32) return false;
    dbus_message_iter_get_basic(v, u);
    return true;
}

static void parse_track(DBusMessageIter *dict) { /* a{sv}: Title, Artist, Album, Duration (ms) */
    const char *title = NULL, *artist = NULL, *album = NULL;
    double dur = -1;
    DBusMessageIter e;
    dbus_message_iter_recurse(dict, &e);
    while (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter kv, v;
        const char *k;
        dbus_message_iter_recurse(&e, &kv);
        dbus_message_iter_get_basic(&kv, &k);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &v);
        dbus_uint32_t u;
        if (!strcmp(k, "Title")) variant_str(&v, &title);
        else if (!strcmp(k, "Artist")) variant_str(&v, &artist);
        else if (!strcmp(k, "Album")) variant_str(&v, &album);
        else if (!strcmp(k, "Duration") && variant_u32(&v, &u)) dur = u / 1000.0;
        dbus_message_iter_next(&e);
    }
    MUSIC(bt_track, title ? title : "", artist ? artist : "", album ? album : "", dur);
}

/* dict = a{sv} with properties for iface at path (from PropertiesChanged or InterfacesAdded) */
static void handle_props(const char *path, const char *iface, DBusMessageIter dict) {
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter kv, v;
        const char *k, *s;
        dbus_uint32_t u;
        dbus_message_iter_recurse(&dict, &kv);
        dbus_message_iter_get_basic(&kv, &k);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &v);
        if (!strcmp(iface, "org.bluez.MediaPlayer1")) {
            snprintf(player_path, sizeof player_path, "%s", path);
            if (!strcmp(k, "Track") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_ARRAY) parse_track(&v);
            else if (!strcmp(k, "Status") && variant_str(&v, &s) && transport_dev[0])
                MUSIC(bt_state, true, !strcmp(s, "playing"), NULL, false);
            else if (!strcmp(k, "Position") && variant_u32(&v, &u)) MUSIC(bt_pos, u / 1000.0);
        } else if (!strcmp(iface, "org.bluez.MediaTransport1") && !strcmp(k, "State") && variant_str(&v, &s)) {
            /* the audio connection: "active" = music is streaming */
            char dev[128], name[128];
            dev_of(path, dev, sizeof dev);
            if (!strcmp(s, "active")) {
                bool fresh = strcmp(transport_dev, dev) != 0; /* normally already set at Connected */
                snprintf(transport_dev, sizeof transport_dev, "%s", dev);
                device_name(dev, name, sizeof name);
                MUSIC(bt_state, true, true, name, fresh);
            } else if (!strcmp(transport_dev, dev)) {
                MUSIC(bt_state, true, false, NULL, false); /* idle: paused (the connection is still there) */
            }
        } else if (!strcmp(iface, "org.bluez.Adapter1") && !strcmp(path, ADAPTER) &&
                   dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BOOLEAN) {
            /* the adapter was (re)powered, or connectable was turned off (e.g. when visibility is turned off): set it right again,
               otherwise paired phones cannot connect */
            dbus_bool_t b;
            dbus_message_iter_get_basic(&v, &b);
            if ((!strcmp(k, "Powered") && b) || (!strcmp(k, "Connectable") && !b && cfg_int("bt_enabled", 1))) bt_apply();
        } else if (!strcmp(iface, "org.bluez.Device1") && !strcmp(k, "Connected") &&
                   dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t b;
            dbus_message_iter_get_basic(&v, &b);
            char dev[128];
            dev_of(path, dev, sizeof dev);
            track_conn(dev, b);
            if (!b && !strcmp(transport_dev, dev)) { /* the phone went away */
                transport_dev[0] = player_path[0] = 0;
                MUSIC(bt_state, false, false, NULL, false);
            } else if (b && strcmp(transport_dev, dev)) { /* connected: show it right away (the audio comes when something plays) */
                char name[128], icon[64];
                get_str(dev, "org.bluez.Device1", "Icon", icon, sizeof icon);
                if (!strncmp(icon, "input", 5)) { dbus_message_iter_next(&dict); continue; } /* keyboard/controller: not music */
                device_name(dev, name, sizeof name);
                snprintf(transport_dev, sizeof transport_dev, "%s", dev);
                MUSIC(bt_state, true, false, name, true);
            }
        }
        dbus_message_iter_next(&dict);
    }
}

static void props_changed(const char *path, DBusMessage *m) {
    DBusMessageIter it, dict;
    const char *iface;
    if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return;
    dbus_message_iter_get_basic(&it, &iface);
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&it, &dict);
    handle_props(path, iface, dict);
}

/* new object: e.g. the phone's player - its current track is only here (not in PropertiesChanged) */
static void interfaces_added(DBusMessage *m) {
    DBusMessageIter it, ifaces;
    const char *path;
    if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_OBJECT_PATH) return;
    dbus_message_iter_get_basic(&it, &path);
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&it, &ifaces);
    while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter kv, dict;
        const char *iface;
        dbus_message_iter_recurse(&ifaces, &kv);
        dbus_message_iter_get_basic(&kv, &iface);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &dict);
        if (!strcmp(iface, "org.bluez.MediaPlayer1") || !strcmp(iface, "org.bluez.MediaTransport1")) handle_props(path, iface, dict);
        dbus_message_iter_next(&ifaces);
    }
}

/* the audio connection disappeared (the phone disconnected): show it right away instead of waiting for Connected=false */
static void interfaces_removed(DBusMessage *m) {
    DBusMessageIter it, arr;
    const char *path;
    if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_OBJECT_PATH) return;
    dbus_message_iter_get_basic(&it, &path);
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *iface;
        dbus_message_iter_get_basic(&arr, &iface);
        char dev[128];
        dev_of(path, dev, sizeof dev);
        if (!strcmp(iface, "org.bluez.MediaTransport1") && !strcmp(transport_dev, dev)) {
            transport_dev[0] = player_path[0] = 0;
            MUSIC(bt_state, false, false, NULL, false);
        }
        dbus_message_iter_next(&arr);
    }
}

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *m, void *ud) {
    (void)c; (void)ud;
    if (dbus_message_is_signal(m, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
        const char *path = dbus_message_get_path(m);
        if (path && !strncmp(path, "/org/bluez/", 11)) props_changed(path, m);
    } else if (dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded")) {
        interfaces_added(m);
    } else if (dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")) {
        interfaces_removed(m);
    } else if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name, *old, *nw;
        if (dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old, DBUS_TYPE_STRING, &nw, DBUS_TYPE_INVALID) &&
            !strcmp(name, "org.bluez")) { /* bluetoothd restarted: register the agent again */
            agent_ok = false;
            next_try = 0;
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---------- offentlig API ---------- */
void bt_init(void) {
    DBusError e;
    dbus_error_init(&e);
    bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
    if (!bus) { SDL_Log("bt: ingen system-D-Bus (%s)", e.message ? e.message : "?"); dbus_error_free(&e); return; }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    dbus_bus_add_match(bus, "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", NULL);
    dbus_bus_add_match(bus, "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'", NULL);
    dbus_bus_add_match(bus, "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'", NULL);
    dbus_connection_add_filter(bus, filter, NULL, NULL);
    static const DBusObjectPathVTable vt = {.message_function = agent_msg};
    dbus_connection_register_object_path(bus, AGENT_PATH, &vt, NULL);
}

void bt_tick(void) {
    if (!bus) return;
    while (dbus_connection_read_write_dispatch(bus, 0) && dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS) {}
    if (!agent_ok && !agent_pending && app_time() >= next_try) { /* bluetoothd may start after us */
        next_try = app_time() + 5;
        register_agent();
    }
    if (pair_until > 0 && app_time() >= pair_until) { /* pairing mode expired */
        pair_until = 0;
        set_bool(ADAPTER, "org.bluez.Adapter1", "Discoverable", false);
        set_bool(ADAPTER, "org.bluez.Adapter1", "Connectable", cfg_int("bt_enabled", 1) != 0);
    }
    /* only when in use: turn off when nothing is connected and the grace period has expired */
    if (radio_on && cfg_int("bt_ondemand", 1) && n_conn == 0 && pair_until == 0 && app_time() >= hold_until) bt_apply();
}

void bt_wake(double secs) {
    if (!bus || !cfg_int("bt_enabled", 1)) return;
    if (hold_until < app_time() + secs) hold_until = app_time() + secs;
    if (!radio_on) bt_apply();
}

void bt_apply(void) {
    if (!bus) return;
    bool enabled = cfg_int("bt_enabled", 1) != 0;
    /* "only when in use" closes for connections instead of turning off the radio: bluealsa (A2DP) needs
       a powered adapter - started while it was off, phones connected but got no audio */
    bool on = enabled && (!cfg_int("bt_ondemand", 1) || n_conn > 0 || pair_until > 0 || app_time() < hold_until);
    if (on != radio_on) SDL_Log("bt: %s", on ? "åben for forbindelser" : "lukket for forbindelser (ikke i brug)");
    radio_on = on;
    const char *name = cfg_get("device_name", "NotaTV");
    set_prop(ADAPTER, "org.bluez.Adapter1", "Alias", DBUS_TYPE_STRING, &name);
    /* paired controllers/keyboards connect by themselves when turned on: so the box is connectable (but invisible) */
    bool input = false;
    if (enabled && !on) {
        BtDevice d[16];
        int n = bt_devices(d, 16);
        for (int i = 0; i < n; i++) input |= d[i].input;
    }
    set_bool(ADAPTER, "org.bluez.Adapter1", "Powered", enabled);
    set_bool(ADAPTER, "org.bluez.Adapter1", "Pairable", on);
    /* paired phones must be able to connect by themselves (e.g. "NotaTV" in Spotify's device list), even when the TV is not visible */
    set_bool(ADAPTER, "org.bluez.Adapter1", "Connectable", on || input);
    if (!on) set_bool(ADAPTER, "org.bluez.Adapter1", "Discoverable", false);
}

void bt_media(const char *cmd) {
    if (!bus || !player_path[0]) return;
    send_call(dbus_message_new_method_call("org.bluez", player_path, "org.bluez.MediaPlayer1", cmd));
}

/* ---------- connect from the TV ---------- */
static bool variant_bool(DBusMessageIter *v) {
    dbus_bool_t b = FALSE;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_BOOLEAN) dbus_message_iter_get_basic(v, &b);
    return b;
}

int bt_devices(BtDevice *out, int max) {
    if (!bus) return 0;
    DBusMessage *m = dbus_message_new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    DBusMessage *r = m ? dbus_connection_send_with_reply_and_block(bus, m, 500, NULL) : NULL;
    if (m) dbus_message_unref(m);
    if (!r) return 0;
    int n = 0;
    DBusMessageIter it, objs;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &objs);
        while (dbus_message_iter_get_arg_type(&objs) == DBUS_TYPE_DICT_ENTRY && n < max) {
            DBusMessageIter kv, ifaces;
            const char *path;
            dbus_message_iter_recurse(&objs, &kv);
            dbus_message_iter_get_basic(&kv, &path);
            dbus_message_iter_next(&kv);
            dbus_message_iter_recurse(&kv, &ifaces);
            while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) { /* a{sa{sv}} */
                DBusMessageIter ik, props;
                const char *iface;
                dbus_message_iter_recurse(&ifaces, &ik);
                dbus_message_iter_get_basic(&ik, &iface);
                if (!strcmp(iface, "org.bluez.Device1")) {
                    dbus_message_iter_next(&ik);
                    dbus_message_iter_recurse(&ik, &props);
                    BtDevice d = {0};
                    bool paired = false;
                    snprintf(d.path, sizeof d.path, "%s", path);
                    while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter pk, v;
                        const char *k, *s;
                        dbus_message_iter_recurse(&props, &pk);
                        dbus_message_iter_get_basic(&pk, &k);
                        dbus_message_iter_next(&pk);
                        dbus_message_iter_recurse(&pk, &v);
                        if (!strcmp(k, "Alias") && variant_str(&v, &s)) snprintf(d.name, sizeof d.name, "%s", s);
                        else if (!strcmp(k, "Paired")) paired = variant_bool(&v);
                        else if (!strcmp(k, "Connected")) d.connected = variant_bool(&v);
                        else if (!strcmp(k, "Icon") && variant_str(&v, &s)) d.input = !strncmp(s, "input", 5);
                        dbus_message_iter_next(&props);
                    }
                    if (paired) out[n++] = d;
                }
                dbus_message_iter_next(&ifaces);
            }
            dbus_message_iter_next(&objs);
        }
    }
    dbus_message_unref(r);
    return n;
}

static void connect_done(DBusPendingCall *pc, void *ud) {
    char *name = ud;
    DBusMessage *r = dbus_pending_call_steal_reply(pc);
    if (r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR)
        app_toast(T("Could not connect to %s - is Bluetooth on on the phone?"), name);
    if (r) dbus_message_unref(r);
    dbus_pending_call_unref(pc);
    free(name);
}

void bt_connect(const char *path) {
    if (!bus || !path || !*path) return;
    bt_wake(60);
    DBusMessage *m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Device1", "Connect");
    if (!m) return;
    char nm[128];
    device_name(path, nm, sizeof nm);
    DBusPendingCall *pc = NULL;
    if (dbus_connection_send_with_reply(bus, m, &pc, 20000) && pc)
        dbus_pending_call_set_notify(pc, connect_done, str_dup(nm), NULL);
    dbus_message_unref(m);
}

void bt_disconnect(void) {
    if (!bus || !transport_dev[0]) return;
    send_call(dbus_message_new_method_call("org.bluez", transport_dev, "org.bluez.Device1", "Disconnect"));
    transport_dev[0] = player_path[0] = 0; /* do not wait for the radio connection to be fully down */
    MUSIC(bt_state, false, false, NULL, false);
}

/* forget all paired devices (Adapter1.RemoveDevice for each) */
void bt_forget_all(void) {
    if (!bus) return;
    DBusMessage *m = dbus_message_new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    DBusMessage *r = m ? dbus_connection_send_with_reply_and_block(bus, m, 1500, NULL) : NULL;
    if (m) dbus_message_unref(m);
    if (!r) { app_toast(T("Bluetooth is not responding")); return; }
    int n = 0;
    DBusMessageIter it, objs;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &objs);
        while (dbus_message_iter_get_arg_type(&objs) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter kv;
            const char *path;
            dbus_message_iter_recurse(&objs, &kv);
            dbus_message_iter_get_basic(&kv, &path);
            const char *d = strstr(path, "/dev_");
            if (!strncmp(path, ADAPTER "/", strlen(ADAPTER) + 1) && d && !strchr(d + 1, '/')) {
                DBusMessage *rm = dbus_message_new_method_call("org.bluez", ADAPTER, "org.bluez.Adapter1", "RemoveDevice");
                if (rm) { dbus_message_append_args(rm, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID); send_call(rm); n++; }
            }
            dbus_message_iter_next(&objs);
        }
    }
    dbus_message_unref(r);
    transport_dev[0] = player_path[0] = 0;
    MUSIC(bt_state, false, false, NULL, false);
    app_toast(n ? T("%d device(s) forgotten") : T("No paired devices"), n);
}

/* ---------- pairing screen ---------- */
static void pair_draw(Screen *s) {
    (void)s;
    draw_background();
    draw_topbar("Bluetooth");
    float cx = UI_W / 2.0f;
    if (!bus) {
        text_draw(F_BOLD, 42, cx, 430, C_TEXT, AL_CENTER, T("Bluetooth is not available"));
        return;
    }
    fill_circle(cx, 280, 90, color_alpha(RGB(40, 120, 255), 0.25f));
    draw_icon(ICON_MUSIC, cx, 280, 90, RGB(90, 160, 255));
    double left = pair_until - app_time();
    if (left > 0) {
        text_draw(F_BOLD, 50, cx, 420, C_TEXT, AL_CENTER, T("Ready to pair"));
        char l1[200];
        snprintf(l1, sizeof l1, T("Open Bluetooth on your phone and choose \"%s\""), cfg_get("device_name", "NotaTV"));
        text_draw(F_REG, 32, cx, 500, C_TEXT2, AL_CENTER, l1);
        text_draw(F_REG, 28, cx, 556, C_TEXT2, AL_CENTER, T("The phone and the TV show the same code - confirm it here with OK"));
        char l2[80];
        snprintf(l2, sizeof l2, T("Visible for another %d:%02d"), (int)left / 60, (int)left % 60);
        draw_spinner(cx - 150, 660, 14, C_TEXT3);
        text_draw(F_REG, 26, cx - 120, 646, C_TEXT3, AL_LEFT, l2);
    } else {
        text_draw(F_BOLD, 50, cx, 420, C_TEXT, AL_CENTER, T("Not visible"));
        text_draw(F_REG, 30, cx, 500, C_TEXT2, AL_CENTER, T("Press OK to make the TV visible for 3 minutes"));
    }
    text_draw(F_REG, 22, cx, UI_H - 70, C_TEXT3, AL_CENTER,
              T("Paired phones reconnect by themselves. Music then just plays - choose \"NotaTV\" as the audio output."));
}

static void pair_start(void) {
    if (!bus) return;
    if (!cfg_int("bt_enabled", 1)) { cfg_set_int("bt_enabled", 1); bt_apply(); }
    bt_wake(180);
    dbus_uint32_t t = 180;
    set_prop(ADAPTER, "org.bluez.Adapter1", "DiscoverableTimeout", DBUS_TYPE_UINT32, &t);
    set_bool(ADAPTER, "org.bluez.Adapter1", "Pairable", true);
    set_bool(ADAPTER, "org.bluez.Adapter1", "Discoverable", true);
    pair_until = app_time() + 180;
}

static bool pair_input(Screen *s, const InputEvent *ev) {
    (void)s;
    if (ev->a == A_OK && !ev->repeat) { pair_start(); return true; }
    return false;
}
static void pair_enter(Screen *s) { (void)s; pair_start(); }
static void pair_leave(Screen *s) {
    (void)s;
    if (pair_until > 0) {
        pair_until = 0;
        set_bool(ADAPTER, "org.bluez.Adapter1", "Discoverable", false);
        set_bool(ADAPTER, "org.bluez.Adapter1", "Connectable", cfg_int("bt_enabled", 1) != 0); /* turning off visibility also turns this off */
    }
}

Screen *bt_pair_screen(void) {
    Screen *s = calloc(1, sizeof *s);
    s->name = "bt-pair";
    s->enter = pair_enter;
    s->leave = pair_leave;
    s->draw = pair_draw;
    s->input = pair_input;
    return s;
}
