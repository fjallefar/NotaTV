/* NotaTV Peezy: Plezy's mpv channels for flutter-pi (see docs/peezy.md).
 *
 *  com.plezy/mpv_audio_player  music: libmpv in this process, no video, so the UI stays on screen.
 *  com.plezy/mpv_player        video: NotaTV's own player. At the first `loadfile` flutter-pi lends the display and
 *                              input to tvshell, the peezy app.so plays the URL with player_run(), and this plugin
 *                              follows tvshell's mpv over its JSON IPC socket, so Plezy still gets its properties and
 *                              events (progress, watched state, auto-play next).
 *
 * Both answer the same methods Plezy's Linux runner does: initialize, dispose, command, setProperty, getProperty,
 * observeProperty, setLogLevel, setVisible, setVideoRect, updateFrame, isHDRSupported, isInitialized. Events go to
 * "<channel>/events": property changes as [id, value, sourceId] and the rest as {type:"event", name, data}.
 * Everything runs on flutter-pi's platform thread (method calls and the fds below), so nothing here takes a lock. */
#define _GNU_SOURCE
#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "util/logging.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <mpv/client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MPV_SOCK "/tmp/notatv-mpv.sock" /* tvshell's player.c SOCK_PATH */
#define MAX_OBS 64
#define MAX_PEND 64
#define MAX_PROPS 64

enum fmt { F_NODE, F_DOUBLE, F_FLAG, F_STRING, F_INT64 };
enum pend_kind { P_COMMAND, P_GET, P_SET };
enum vstate { V_IDLE, V_STARTING, V_PLAYING };

struct obs { int id; char *name; enum fmt fmt; };
struct pend { int64_t rid; enum pend_kind kind; FlutterPlatformMessageResponseHandle *h; };
struct prop { char *name, *value; };

struct core {
    const char *chan, *evchan;
    bool video, inited, listening;
    struct obs obs[MAX_OBS];
    int n_obs;
    /* audio: libmpv */
    mpv_handle *mpv;
    int wake_fd;
    sd_event_source *wake_src;
    /* video: tvshell's mpv */
    enum vstate vs;
    int ipc_fd;
    sd_event_source *ipc_src;
    char ipc_buf[65536];
    size_t ipc_len;
    struct pend pend[MAX_PEND];
    int64_t next_rid;
    struct prop props[MAX_PROPS]; /* set before the first loadfile: they become mpv options */
    int n_props;
    char *headers;                /* http-header-fields, from change-list */
    FlutterPlatformMessageResponseHandle *load_h; /* the loadfile that started playback, answered once mpv is reachable */
    int64_t entry;                /* mpv's playlist entry id of what plays */
    bool ended_sent, eof;
    struct { int n; cJSON *meta, *skip; } extra;
};

static struct core audio = { .chan = "com.plezy/mpv_audio_player", .evchan = "com.plezy/mpv_audio_player/events", .wake_fd = -1, .ipc_fd = -1 };
static struct core video = { .chan = "com.plezy/mpv_player", .evchan = "com.plezy/mpv_player/events", .video = true, .wake_fd = -1, .ipc_fd = -1 };

static int ctl_fd = -1; /* to the peezy app.so in tvshell */
static sd_event_source *ctl_src;
static char ctl_buf[4096];
static size_t ctl_len;

/* ---------- std_value helpers ---------- */
static void std_free(struct std_value *v) {
    if (v->type == kStdString) free(v->string_value);
    else if (v->type == kStdList) {
        for (size_t i = 0; i < v->size; i++) std_free(&v->list[i]);
        free(v->list);
    } else if (v->type == kStdMap) {
        for (size_t i = 0; i < v->size; i++) { std_free(&v->keys[i]); std_free(&v->values[i]); }
        free(v->keys);
        free(v->values);
    }
}

static struct std_value std_str(const char *s) { return STDSTRING(strdup(s ? s : "")); }

static struct std_value json_to_std(const cJSON *j) {
    if (!j || cJSON_IsNull(j)) return STDNULL;
    if (cJSON_IsBool(j)) return STDBOOL(cJSON_IsTrue(j));
    if (cJSON_IsNumber(j)) {
        double d = j->valuedouble;
        if (d == (double)(int64_t)d && d > -9e15 && d < 9e15) return STDINT64((int64_t)d);
        return STDFLOAT64(d);
    }
    if (cJSON_IsString(j)) return std_str(j->valuestring);
    int n = cJSON_GetArraySize(j), i = 0;
    if (cJSON_IsArray(j)) {
        struct std_value v = { .type = kStdList, .size = (size_t)n, .list = calloc((size_t)n + 1, sizeof(struct std_value)) };
        for (const cJSON *c = j->child; c; c = c->next) v.list[i++] = json_to_std(c);
        return v;
    }
    struct std_value v = { .type = kStdMap, .size = (size_t)n };
    v.keys = calloc((size_t)n + 1, sizeof(struct std_value));
    v.values = calloc((size_t)n + 1, sizeof(struct std_value));
    for (const cJSON *c = j->child; c; c = c->next, i++) { v.keys[i] = std_str(c->string); v.values[i] = json_to_std(c); }
    return v;
}

/* a JSON value as the format Plezy observed it with */
static struct std_value json_as(const cJSON *j, enum fmt f) {
    if (!j || cJSON_IsNull(j)) return STDNULL;
    char buf[64];
    switch (f) {
    case F_DOUBLE: return cJSON_IsNumber(j) ? STDFLOAT64(j->valuedouble) : STDNULL;
    case F_INT64: return cJSON_IsNumber(j) ? STDINT64((int64_t)j->valuedouble) : STDNULL;
    case F_FLAG: return cJSON_IsBool(j) ? STDBOOL(cJSON_IsTrue(j)) : cJSON_IsString(j) ? STDBOOL(!strcmp(j->valuestring, "yes")) : STDNULL;
    case F_STRING:
        if (cJSON_IsString(j)) return std_str(j->valuestring);
        if (cJSON_IsBool(j)) return std_str(cJSON_IsTrue(j) ? "yes" : "no");
        if (cJSON_IsNumber(j)) {
            if (j->valuedouble == (double)(int64_t)j->valuedouble) snprintf(buf, sizeof buf, "%lld", (long long)j->valuedouble);
            else snprintf(buf, sizeof buf, "%f", j->valuedouble);
            return std_str(buf);
        }
        return STDNULL;
    default: return json_to_std(j);
    }
}

static struct std_value node_to_std(const mpv_node *n) {
    switch (n->format) {
    case MPV_FORMAT_STRING: return std_str(n->u.string);
    case MPV_FORMAT_FLAG: return STDBOOL(n->u.flag);
    case MPV_FORMAT_INT64: return STDINT64(n->u.int64);
    case MPV_FORMAT_DOUBLE: return STDFLOAT64(n->u.double_);
    case MPV_FORMAT_NODE_ARRAY: {
        mpv_node_list *l = n->u.list;
        struct std_value v = { .type = kStdList, .size = (size_t)l->num, .list = calloc((size_t)l->num + 1, sizeof(struct std_value)) };
        for (int i = 0; i < l->num; i++) v.list[i] = node_to_std(&l->values[i]);
        return v;
    }
    case MPV_FORMAT_NODE_MAP: {
        mpv_node_list *l = n->u.list;
        struct std_value v = { .type = kStdMap, .size = (size_t)l->num };
        v.keys = calloc((size_t)l->num + 1, sizeof(struct std_value));
        v.values = calloc((size_t)l->num + 1, sizeof(struct std_value));
        for (int i = 0; i < l->num; i++) { v.keys[i] = std_str(l->keys[i]); v.values[i] = node_to_std(&l->values[i]); }
        return v;
    }
    default: return STDNULL;
    }
}

static const char *arg_str(struct platch_obj *o, const char *key) {
    if (!STDVALUE_IS_MAP(o->std_arg)) return NULL;
    struct std_value *v = stdmap_get_str(&o->std_arg, (char *)key);
    return v && STDVALUE_IS_STRING(*v) ? STDVALUE_AS_STRING(*v) : NULL;
}

static bool arg_int(struct platch_obj *o, const char *key, int64_t *out) {
    if (!STDVALUE_IS_MAP(o->std_arg)) return false;
    struct std_value *v = stdmap_get_str(&o->std_arg, (char *)key);
    if (!v || !STDVALUE_IS_INT(*v)) return false;
    *out = STDVALUE_AS_INT(*v);
    return true;
}

static enum fmt fmt_of(const char *s) {
    if (!s) return F_NODE;
    if (!strcmp(s, "double")) return F_DOUBLE;
    if (!strcmp(s, "flag")) return F_FLAG;
    if (!strcmp(s, "string")) return F_STRING;
    if (!strcmp(s, "int64")) return F_INT64;
    return F_NODE;
}

/* ---------- events to Dart ---------- */
static void send_ev(struct core *c, struct std_value *v) {
    if (c->listening) { int r_ = platch_send_success_event_std((char *)c->evchan, v); if (r_) LOG_ERROR("nota_mpv: event send failed: %d (type %d)\n", r_, v->type); }
    std_free(v);
}

/* {type:"event", name, data}; data is consumed (may be NULL) */
static void send_named(struct core *c, const char *name, struct std_value *data) {
    struct std_value keys[3] = { std_str("type"), std_str("name"), std_str("data") };
    struct std_value vals[3] = { std_str("event"), std_str(name), data ? *data : STDNULL };
    struct std_value m = { .type = kStdMap, .size = data ? 3 : 2, .keys = malloc(sizeof keys), .values = malloc(sizeof vals) };
    memcpy(m.keys, keys, sizeof keys);
    memcpy(m.values, vals, sizeof vals);
    if (!data) { std_free(&keys[2]); }
    send_ev(c, &m);
}

static struct std_value map1(const char *k, struct std_value v) {
    struct std_value m = { .type = kStdMap, .size = 1, .keys = malloc(sizeof(struct std_value)), .values = malloc(sizeof(struct std_value)) };
    m.keys[0] = std_str(k);
    m.values[0] = v;
    return m;
}

static void send_source_event(struct core *c, const char *name, int64_t entry) {
    struct std_value d = map1("sourceId", STDINT64(entry));
    send_named(c, name, &d);
}

static void send_prop(struct core *c, int id, struct std_value val, bool has_src, int64_t src) {
    struct std_value l = { .type = kStdList, .size = 3, .list = malloc(3 * sizeof(struct std_value)) };
    l.list[0] = STDINT64(id);
    l.list[1] = val;
    l.list[2] = has_src ? STDINT64(src) : STDNULL;
    send_ev(c, &l);
}

/* MPV_END_FILE_REASON_* from the IPC's reason text */
static int end_reason(const char *s) {
    if (!s) return 2;
    if (!strcmp(s, "eof")) return 0;
    if (!strcmp(s, "stop")) return 2;
    if (!strcmp(s, "quit")) return 3;
    if (!strcmp(s, "error")) return 4;
    if (!strcmp(s, "redirect")) return 5;
    return 2;
}

static void send_end_file(struct core *c, int64_t entry, int reason, const char *err) {
    struct std_value d = { .type = kStdMap, .size = err ? 4 : 2, .keys = calloc(4, sizeof(struct std_value)), .values = calloc(4, sizeof(struct std_value)) };
    d.keys[0] = std_str("sourceId"); d.values[0] = STDINT64(entry);
    d.keys[1] = std_str("reason"); d.values[1] = STDINT64(reason);
    if (err) {
        d.keys[2] = std_str("error"); d.values[2] = STDINT64(-13); /* MPV_ERROR_LOADING_FAILED */
        d.keys[3] = std_str("message"); d.values[3] = std_str(err);
    }
    send_named(c, "end-file", &d);
}

/* ---------- audio core: libmpv in this process ---------- */
static void audio_wakeup(void *ud) {
    (void)ud;
    uint64_t one = 1;
    (void)!write(audio.wake_fd, &one, sizeof one);
}

static struct obs *obs_find(struct core *c, int id) {
    for (int i = 0; i < c->n_obs; i++) if (c->obs[i].id == id) return &c->obs[i];
    return NULL;
}

static int64_t audio_entry = -1;

static void c_numeric(void);

static int on_audio_wake(sd_event_source *s, int fd, uint32_t revents, void *ud) {
    (void)s; (void)revents; (void)ud;
    c_numeric();
    uint64_t v;
    while (read(fd, &v, sizeof v) > 0) {}
    while (audio.mpv) {
        mpv_event *e = mpv_wait_event(audio.mpv, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        switch (e->event_id) {
        case MPV_EVENT_COMMAND_REPLY: {
            FlutterPlatformMessageResponseHandle *h = (void *)(uintptr_t)e->reply_userdata;
            if (!h) break;
            if (e->error < 0) { platch_respond_error_std(h, "COMMAND_FAILED", "MPV command failed", NULL); break; }
            mpv_event_command *cmd = e->data;
            struct std_value r = STDNULL;
            if (cmd && cmd->result.format == MPV_FORMAT_NODE_MAP) {
                mpv_node_list *l = cmd->result.u.list;
                for (int i = 0; i < l->num; i++)
                    if (!strcmp(l->keys[i], "playlist_entry_id") && l->values[i].format == MPV_FORMAT_INT64)
                        r = map1("playlistEntryId", STDINT64(l->values[i].u.int64));
            }
            platch_respond_success_std(h, &r);
            std_free(&r);
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property *p = e->data;
            struct obs *o = obs_find(&audio, (int)e->reply_userdata);
            if (!o) break;
            struct std_value val = STDNULL;
            switch (p->format) {
            case MPV_FORMAT_DOUBLE: val = STDFLOAT64(*(double *)p->data); break;
            case MPV_FORMAT_FLAG: val = STDBOOL(*(int *)p->data); break;
            case MPV_FORMAT_INT64: val = STDINT64(*(int64_t *)p->data); break;
            case MPV_FORMAT_STRING: val = std_str(*(char **)p->data); break;
            case MPV_FORMAT_NODE: val = node_to_std(p->data); break;
            default: break;
            }
            send_prop(&audio, o->id, val, audio_entry >= 0, audio_entry);
            break;
        }
        case MPV_EVENT_START_FILE: {
            mpv_event_start_file *sf = e->data;
            audio_entry = sf ? sf->playlist_entry_id : -1;
            send_source_event(&audio, "start-file", audio_entry);
            break;
        }
        case MPV_EVENT_FILE_LOADED: send_source_event(&audio, "file-loaded", audio_entry); break;
        case MPV_EVENT_PLAYBACK_RESTART: send_source_event(&audio, "playback-restart", audio_entry); break;
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file *ef = e->data;
            send_end_file(&audio, ef ? ef->playlist_entry_id : audio_entry, ef ? (int)ef->reason : 2,
                          ef && ef->reason == MPV_END_FILE_REASON_ERROR ? mpv_error_string(ef->error) : NULL);
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            mpv_event_log_message *m = e->data;
            struct std_value d = { .type = kStdMap, .size = 3, .keys = malloc(3 * sizeof(struct std_value)), .values = malloc(3 * sizeof(struct std_value)) };
            d.keys[0] = std_str("prefix"); d.values[0] = std_str(m->prefix);
            d.keys[1] = std_str("level"); d.values[1] = std_str(m->level);
            d.keys[2] = std_str("text"); d.values[2] = std_str(m->text);
            send_named(&audio, "log-message", &d);
            break;
        }
        default: break;
        }
    }
    return 0;
}

static bool audio_init(void) {
    if (audio.mpv) return true;
    audio.mpv = mpv_create();
    if (!audio.mpv) return false;
    mpv_set_option_string(audio.mpv, "config", "no");
    mpv_set_option_string(audio.mpv, "vo", "null");
    mpv_set_option_string(audio.mpv, "video", "no");
    mpv_set_option_string(audio.mpv, "audio-display", "no");
    mpv_set_option_string(audio.mpv, "idle", "yes");
    mpv_set_option_string(audio.mpv, "gapless-audio", "yes");
    mpv_set_option_string(audio.mpv, "ao", "alsa");
    /* no Lua in here: mpv's built-in scripts (ytdl_hook, osc, ...) are not needed for music, and LuaJIT inside the
       Flutter process crashed (tested on x86) */
    static const char *const off[] = { "ytdl", "osc", "load-scripts", "load-stats-overlay", "load-console",
        "load-auto-profiles", "load-select", "load-positioning", "load-commands", "load-context-menu",
        "input-default-bindings", "input-vo-keyboard", NULL };
    for (int i = 0; off[i]; i++) mpv_set_option_string(audio.mpv, off[i], "no"); /* an unknown one is ignored */
    if (mpv_initialize(audio.mpv) < 0) { mpv_destroy(audio.mpv); audio.mpv = NULL; return false; }
    audio.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    flutterpi_sd_event_add_io(&audio.wake_src, audio.wake_fd, EPOLLIN, on_audio_wake, NULL);
    mpv_set_wakeup_callback(audio.mpv, audio_wakeup, NULL);
    return true;
}

static void audio_dispose(void) {
    if (!audio.mpv) return;
    mpv_set_wakeup_callback(audio.mpv, NULL, NULL);
    mpv_terminate_destroy(audio.mpv);
    audio.mpv = NULL;
    if (audio.wake_src) { sd_event_source_disable_unref(audio.wake_src); audio.wake_src = NULL; }
    if (audio.wake_fd >= 0) { close(audio.wake_fd); audio.wake_fd = -1; }
}

/* ---------- video core: tvshell's mpv over IPC ---------- */
static void ctl_send(const char *line) {
    if (ctl_fd < 0) return;
    size_t n = strlen(line);
    while (n) {
        ssize_t w = send(ctl_fd, line, n, MSG_NOSIGNAL);
        if (w <= 0) { if (errno == EINTR) continue; LOG_ERROR("nota_mpv: control socket: %s\n", strerror(errno)); return; }
        line += w;
        n -= (size_t)w;
    }
}

static void ipc_write(cJSON *msg) {
    char *s = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!s) return;
    if (video.ipc_fd >= 0) {
        size_t n = strlen(s);
        char *line = malloc(n + 2);
        memcpy(line, s, n);
        line[n] = '\n';
        line[n + 1] = 0;
        const char *p = line;
        size_t left = n + 1;
        while (left) {
            ssize_t w = send(video.ipc_fd, p, left, MSG_NOSIGNAL);
            if (w <= 0) { if (errno == EINTR) continue; break; }
            p += w;
            left -= (size_t)w;
        }
        free(line);
    }
    free(s);
}

/* send an IPC command; h (may be NULL) is answered with the reply */
static void ipc_cmd(cJSON *args, enum pend_kind kind, FlutterPlatformMessageResponseHandle *h) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "command", args);
    if (h) {
        int64_t rid = ++video.next_rid;
        cJSON_AddNumberToObject(msg, "request_id", (double)rid);
        int slot = -1;
        for (int i = 0; i < MAX_PEND; i++) if (!video.pend[i].h) { slot = i; break; }
        if (slot < 0) { platch_respond_error_std(h, "BUSY", "too many requests", NULL); cJSON_Delete(msg); return; }
        video.pend[slot] = (struct pend){ rid, kind, h };
    }
    ipc_write(msg);
}

static cJSON *strarr(const char *const *a, int n) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(a[i]));
    return arr;
}

static void ipc_observe_all(void) {
    for (int i = 0; i < video.n_obs; i++) {
        cJSON *a = cJSON_CreateArray();
        cJSON_AddItemToArray(a, cJSON_CreateString("observe_property"));
        cJSON_AddItemToArray(a, cJSON_CreateNumber(video.obs[i].id));
        cJSON_AddItemToArray(a, cJSON_CreateString(video.obs[i].name));
        ipc_cmd(a, P_COMMAND, NULL);
    }
}

static void fail_all_pending(void) {
    for (int i = 0; i < MAX_PEND; i++) {
        if (!video.pend[i].h) continue;
        struct std_value n = STDNULL;
        if (video.pend[i].kind == P_COMMAND) platch_respond_error_std(video.pend[i].h, "COMMAND_FAILED", "player closed", NULL);
        else platch_respond_success_std(video.pend[i].h, &n);
        video.pend[i].h = NULL;
    }
}

static void ipc_close(void) {
    if (video.ipc_src) { sd_event_source_disable_unref(video.ipc_src); video.ipc_src = NULL; }
    if (video.ipc_fd >= 0) { close(video.ipc_fd); video.ipc_fd = -1; }
    video.ipc_len = 0;
    fail_all_pending();
}

static void video_answer_load(int64_t entry);

static void ipc_line(char *line) {
    cJSON *j = cJSON_Parse(line);
    if (!j) return;
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(j, "request_id");
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(j, "event");
    if (cJSON_IsNumber(rid) && !ev) {
        int64_t id = (int64_t)rid->valuedouble;
        if (id == -1) { /* video_started's playlist/0/id */
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
            video_answer_load(cJSON_IsNumber(d) ? (int64_t)d->valuedouble : 1);
            cJSON_Delete(j);
            return;
        }
        for (int i = 0; i < MAX_PEND; i++) {
            if (!video.pend[i].h || video.pend[i].rid != id) continue;
            FlutterPlatformMessageResponseHandle *h = video.pend[i].h;
            video.pend[i].h = NULL;
            const cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
            bool ok = cJSON_IsString(err) && !strcmp(err->valuestring, "success");
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(j, "data");
            struct std_value r = STDNULL;
            if (video.pend[i].kind == P_COMMAND) {
                if (!ok) { platch_respond_error_std(h, "COMMAND_FAILED", "MPV command failed", NULL); break; }
                const cJSON *pe = data ? cJSON_GetObjectItemCaseSensitive(data, "playlist_entry_id") : NULL;
                if (cJSON_IsNumber(pe)) r = map1("playlistEntryId", STDINT64((int64_t)pe->valuedouble));
            } else if (video.pend[i].kind == P_GET) {
                if (ok && cJSON_IsString(data)) r = std_str(data->valuestring);
            }
            platch_respond_success_std(h, &r);
            std_free(&r);
            break;
        }
        cJSON_Delete(j);
        return;
    }
    if (!cJSON_IsString(ev)) { cJSON_Delete(j); return; }
    const char *name = ev->valuestring;
    const cJSON *pe = cJSON_GetObjectItemCaseSensitive(j, "playlist_entry_id");
    int64_t entry = cJSON_IsNumber(pe) ? (int64_t)pe->valuedouble : video.entry;
    if (!strcmp(name, "property-change")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
        struct obs *o = cJSON_IsNumber(id) ? obs_find(&video, (int)id->valuedouble) : NULL;
        if (o) send_prop(&video, o->id, json_as(cJSON_GetObjectItemCaseSensitive(j, "data"), o->fmt), video.entry >= 0, video.entry);
    } else if (!strcmp(name, "start-file")) {
        video.entry = entry;
        video.ended_sent = false;
        send_source_event(&video, "start-file", entry);
    } else if (!strcmp(name, "file-loaded") || !strcmp(name, "playback-restart")) {
        send_source_event(&video, name, video.entry);
    } else if (!strcmp(name, "end-file")) {
        const cJSON *rs = cJSON_GetObjectItemCaseSensitive(j, "reason");
        const cJSON *fe = cJSON_GetObjectItemCaseSensitive(j, "file_error");
        int reason = end_reason(cJSON_IsString(rs) ? rs->valuestring : NULL);
        if (reason == 0) video.eof = true;
        send_end_file(&video, entry, reason, reason == 4 && cJSON_IsString(fe) ? fe->valuestring : NULL);
        video.ended_sent = true;
    } else if (!strcmp(name, "log-message")) {
        struct std_value d = json_to_std(j);
        send_named(&video, "log-message", &d);
    }
    cJSON_Delete(j);
}

static void c_numeric(void);

static int on_ipc(sd_event_source *s, int fd, uint32_t revents, void *ud) {
    (void)s; (void)ud;
    c_numeric();
    for (;;) {
        if (video.ipc_len >= sizeof video.ipc_buf - 1) video.ipc_len = 0; /* a line that never ends: drop it */
        ssize_t r = read(fd, video.ipc_buf + video.ipc_len, sizeof video.ipc_buf - 1 - video.ipc_len);
        if (r > 0) {
            video.ipc_len += (size_t)r;
            video.ipc_buf[video.ipc_len] = 0;
            char *nl;
            while ((nl = memchr(video.ipc_buf, '\n', video.ipc_len))) {
                *nl = 0;
                ipc_line(video.ipc_buf);
                size_t used = (size_t)(nl + 1 - video.ipc_buf);
                memmove(video.ipc_buf, nl + 1, video.ipc_len - used);
                video.ipc_len -= used;
            }
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
        ipc_close(); /* mpv quit; app.so says "ended" when tvshell is done with the display */
        break;
    }
    (void)revents;
    return 0;
}

static bool ipc_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", MPV_SOCK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return false; }
    video.ipc_fd = fd;
    flutterpi_sd_event_add_io(&video.ipc_src, fd, EPOLLIN, on_ipc, NULL);
    return true;
}

/* properties that are NotaTV's to decide (display, decoding, audio output, subtitle look, OSD), never Plezy's */
static bool prop_is_ours(const char *n) {
    static const char *const pre[] = { "vo", "gpu-", "hwdec", "ao", "audio-device", "audio-exclusive", "audio-spdif",
        "audio-channels", "input-", "idle", "force-window", "keep-open", "osd-", "osc", "terminal", "msg-", "config",
        "video-sync", "display-", "target-", "tone-mapping", "hdr-", "icc-", "log-file", "sub-font", "sub-color",
        "sub-border", "sub-shadow", "sub-back", "sub-bold", "sub-italic", "sub-spacing", "sub-margin", "sub-pos",
        "sub-scale", "sub-ass-", "sub-use-margins", "blend-subtitles", "video-zoom", "panscan", "interpolation",
        "tscale", "scale", "dscale", "cscale", "deband", "glsl-", "profile", "drm-", "vf", "af", "volume", "mute",
        "wayland", "x11", "screenshot", "watch-later", "save-position", "resume-playback", "stop-screensaver",
        "pause", NULL };
    for (int i = 0; pre[i]; i++) if (!strncmp(n, pre[i], strlen(pre[i]))) return true;
    return false;
}

static void prop_store(const char *name, const char *value) {
    for (int i = 0; i < video.n_props; i++)
        if (!strcmp(video.props[i].name, name)) { free(video.props[i].value); video.props[i].value = strdup(value); return; }
    if (video.n_props < MAX_PROPS) video.props[video.n_props++] = (struct prop){ strdup(name), strdup(value) };
}

static void props_clear(void) {
    for (int i = 0; i < video.n_props; i++) { free(video.props[i].name); free(video.props[i].value); }
    video.n_props = 0;
}

static const char *prop_get(const char *name) {
    for (int i = 0; i < video.n_props; i++) if (!strcmp(video.props[i].name, name)) return video.props[i].value;
    return NULL;
}

/* the first loadfile: lend the display and ask tvshell to play */
static void video_start(char **args, int n, FlutterPlatformMessageResponseHandle *h) {
    if (ctl_fd < 0) { platch_respond_error_std(h, "COMMAND_FAILED", "NotaTV is not reachable", NULL); return; }
    cJSON *rq = cJSON_CreateObject();
    cJSON_AddStringToObject(rq, "url", args[1]);
    const char *start = prop_get("start");
    if (start && strcmp(start, "none")) cJSON_AddNumberToObject(rq, "start", atof(start));
    const char *title = prop_get("force-media-title");
    if (title) cJSON_AddStringToObject(rq, "title", title);
    cJSON *extra = cJSON_AddArrayToObject(rq, "extra");
    char opt[8192];
    for (int i = 0; i < video.n_props; i++) {
        const char *k = video.props[i].name;
        if (!strcmp(k, "start") || !strcmp(k, "force-media-title") || prop_is_ours(k)) continue;
        snprintf(opt, sizeof opt, "--%s=%s", k, video.props[i].value);
        cJSON_AddItemToArray(extra, cJSON_CreateString(opt));
    }
    if (video.headers) {
        snprintf(opt, sizeof opt, "--http-header-fields=%s", video.headers);
        cJSON_AddItemToArray(extra, cJSON_CreateString(opt));
    }
    /* loadfile's per-file options (5th argument "key=value,..."), e.g. http-header-fields */
    if (n >= 5 && args[4] && *args[4]) {
        char *copy = strdup(args[4]), *sv = NULL;
        for (char *kv = strtok_r(copy, ",", &sv); kv; kv = strtok_r(NULL, ",", &sv)) {
            char *eq = strchr(kv, '=');
            if (!eq || prop_is_ours(kv)) continue;
            snprintf(opt, sizeof opt, "--%s", kv);
            cJSON_AddItemToArray(extra, cJSON_CreateString(opt));
        }
        free(copy);
    }
    if (video.extra.meta) cJSON_AddItemToObject(rq, "meta", cJSON_Duplicate(video.extra.meta, 1));
    if (video.extra.skip) cJSON_AddItemToObject(rq, "skip", cJSON_Duplicate(video.extra.skip, 1));
    char *s = cJSON_PrintUnformatted(rq);
    cJSON_Delete(rq);
    /* what was set before this loadfile belongs to it: Plezy sets it all again for the next one */
    props_clear();
    free(video.headers);
    video.headers = NULL;
    video.vs = V_STARTING;
    video.load_h = h;
    video.eof = false;
    video.ended_sent = false;
    video.entry = -1;
    flutterpi_nota_display(false);
    size_t len = strlen(s) + 8;
    char *line = malloc(len);
    snprintf(line, len, "play\t%s\n", s);
    ctl_send(line);
    free(line);
    free(s);
}

/* tvshell's mpv runs: connect, catch up on what Plezy asked for, answer the loadfile */
static void video_started(void) {
    if (video.vs != V_STARTING) return;
    if (!ipc_connect()) {
        LOG_ERROR("nota_mpv: cannot reach mpv at %s\n", MPV_SOCK);
        return; /* "ended" will follow */
    }
    video.vs = V_PLAYING;
    ipc_observe_all();
    const char *ev[] = { "enable_event", "all" };
    ipc_cmd(strarr(ev, 2), P_COMMAND, NULL);
    /* what plays and how far it got: events that happened before we connected are sent now */
    if (video.load_h) {
        cJSON *a = cJSON_CreateArray();
        cJSON_AddItemToArray(a, cJSON_CreateString("get_property"));
        cJSON_AddItemToArray(a, cJSON_CreateString("playlist/0/id"));
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddItemToObject(msg, "command", a);
        cJSON_AddNumberToObject(msg, "request_id", -1);
        ipc_write(msg);
    }
}

static void video_ended(bool eof) {
    ipc_close();
    bool was = video.vs != V_IDLE;
    video.vs = V_IDLE;
    flutterpi_nota_display(true);
    if (video.load_h) { platch_respond_error_std(video.load_h, "COMMAND_FAILED", "playback did not start", NULL); video.load_h = NULL; }
    if (!was) return;
    if (!video.ended_sent) send_end_file(&video, video.entry >= 0 ? video.entry : 1, eof || video.eof ? 0 : 2, NULL);
    if (!(eof || video.eof)) send_named(&video, "nota-closed", NULL);
}

/* the loadfile reply (request_id -1) carries playlist/0/id */
static void video_answer_load(int64_t entry) {
    video.entry = entry;
    if (video.load_h) {
        struct std_value r = map1("playlistEntryId", STDINT64(entry));
        platch_respond_success_std(video.load_h, &r);
        std_free(&r);
        video.load_h = NULL;
    }
    send_source_event(&video, "start-file", entry);
    send_source_event(&video, "file-loaded", entry);
    send_source_event(&video, "playback-restart", entry);
}

/* ---------- control socket (app.so) ---------- */
static void ctl_line(char *line) {
    if (!strcmp(line, "started")) video_started();
    else if (!strncmp(line, "ended", 5)) video_ended(line[5] == '\t' && line[6] == '1');
}

static int on_ctl(sd_event_source *s, int fd, uint32_t revents, void *ud) {
    (void)s; (void)revents; (void)ud;
    for (;;) {
        if (ctl_len >= sizeof ctl_buf - 1) ctl_len = 0;
        ssize_t r = read(fd, ctl_buf + ctl_len, sizeof ctl_buf - 1 - ctl_len);
        if (r > 0) {
            ctl_len += (size_t)r;
            ctl_buf[ctl_len] = 0;
            char *nl;
            while ((nl = memchr(ctl_buf, '\n', ctl_len))) {
                *nl = 0;
                ctl_line(ctl_buf);
                size_t used = (size_t)(nl + 1 - ctl_buf);
                memmove(ctl_buf, nl + 1, ctl_len - used);
                ctl_len -= used;
            }
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
        /* tvshell is gone: nothing can show us any more */
        LOG_ERROR("nota_mpv: control socket closed - exiting\n");
        exit(0);
    }
    return 0;
}

/* JSON numbers and libmpv need "." as the decimal point, but flutter-pi sets the user's locale (Danish: ",") at
   start. libmpv even refuses to start in another LC_NUMERIC. Cheap, so it is simply done on every entry. */
static void c_numeric(void) { setlocale(LC_NUMERIC, "C"); }

/* ---------- method calls ---------- */
static int respond_null(FlutterPlatformMessageResponseHandle *h) {
    return platch_respond_success_std(h, &STDNULL);
}

static char **args_list(struct platch_obj *o, int *n) {
    *n = 0;
    if (!STDVALUE_IS_MAP(o->std_arg)) return NULL;
    struct std_value *l = stdmap_get_str(&o->std_arg, "args");
    if (!l || !STDVALUE_IS_LIST(*l)) return NULL;
    char **a = calloc(l->size + 1, sizeof(char *));
    for (size_t i = 0; i < l->size; i++)
        if (STDVALUE_IS_STRING(l->list[i])) a[(*n)++] = STDVALUE_AS_STRING(l->list[i]);
    return a;
}

static int on_method(struct core *c, struct platch_obj *o, FlutterPlatformMessageResponseHandle *h) {
    const char *m = o->method;
    c_numeric();
    if (!strcmp(m, "initialize")) {
        if (!c->video && !audio_init()) return platch_respond_error_std(h, "INIT_FAILED", "Failed to initialize MPV player", NULL);
        if (c->video && ctl_fd < 0) return platch_respond_error_std(h, "INIT_FAILED", "NotaTV is not reachable", NULL);
        c->inited = true;
        return platch_respond_success_std(h, &STDBOOL(true));
    }
    if (!strcmp(m, "isInitialized")) return platch_respond_success_std(h, &STDBOOL(c->inited));
    if (!strcmp(m, "dispose")) {
        if (c->video) {
            if (video.vs == V_PLAYING) { const char *q[] = { "quit" }; ipc_cmd(strarr(q, 1), P_COMMAND, NULL); }
            props_clear();
            free(video.headers);
            video.headers = NULL;
            cJSON_Delete(video.extra.meta); video.extra.meta = NULL;
            cJSON_Delete(video.extra.skip); video.extra.skip = NULL;
        } else {
            audio_dispose();
        }
        for (int i = 0; i < c->n_obs; i++) free(c->obs[i].name);
        c->n_obs = 0;
        c->inited = false;
        return respond_null(h);
    }
    if (!strcmp(m, "observeProperty")) {
        const char *name = arg_str(o, "name"), *f = arg_str(o, "format");
        int64_t id;
        if (!name || !arg_int(o, "id", &id)) return platch_respond_illegal_arg_std(h, "Missing 'name' or 'id'");
        if (c->n_obs < MAX_OBS && !obs_find(c, (int)id)) c->obs[c->n_obs++] = (struct obs){ (int)id, strdup(name), fmt_of(f) };
        if (!c->video && audio.mpv) {
            static const mpv_format mf[] = { [F_NODE] = MPV_FORMAT_NODE, [F_DOUBLE] = MPV_FORMAT_DOUBLE, [F_FLAG] = MPV_FORMAT_FLAG,
                                             [F_STRING] = MPV_FORMAT_STRING, [F_INT64] = MPV_FORMAT_INT64 };
            mpv_observe_property(audio.mpv, (uint64_t)id, name, mf[fmt_of(f)]);
        } else if (c->video && video.vs == V_PLAYING) {
            cJSON *a = cJSON_CreateArray();
            cJSON_AddItemToArray(a, cJSON_CreateString("observe_property"));
            cJSON_AddItemToArray(a, cJSON_CreateNumber((double)id));
            cJSON_AddItemToArray(a, cJSON_CreateString(name));
            ipc_cmd(a, P_COMMAND, NULL);
        }
        return respond_null(h);
    }
    if (!strcmp(m, "setProperty")) {
        const char *name = arg_str(o, "name"), *value = arg_str(o, "value");
        if (!name || !value) return platch_respond_illegal_arg_std(h, "Missing 'name' or 'value'");
        if (!c->video) {
            if (!audio.mpv) return platch_respond_error_std(h, "NOT_INITIALIZED", "Player not initialized", NULL);
            int r = mpv_set_property_string(audio.mpv, name, value);
            if (r < 0) return platch_respond_error_std(h, "SET_PROPERTY_FAILED", (char *)mpv_error_string(r), NULL);
            return respond_null(h);
        }
        if (video.vs == V_PLAYING) {
            if (prop_is_ours(name) && strcmp(name, "pause")) return respond_null(h);
            const char *a[] = { "set_property_string", name, value };
            ipc_cmd(strarr(a, 3), P_SET, h);
            return 0;
        }
        prop_store(name, value);
        return respond_null(h);
    }
    if (!strcmp(m, "getProperty")) {
        const char *name = arg_str(o, "name");
        if (!name) return platch_respond_illegal_arg_std(h, "Missing 'name'");
        if (!c->video) {
            char *v = audio.mpv ? mpv_get_property_string(audio.mpv, name) : NULL;
            struct std_value r = v && *v ? std_str(v) : STDNULL;
            mpv_free(v);
            int ret = platch_respond_success_std(h, &r);
            std_free(&r);
            return ret;
        }
        if (video.vs == V_PLAYING) {
            const char *a[] = { "get_property_string", name };
            ipc_cmd(strarr(a, 2), P_GET, h);
            return 0;
        }
        const char *v = prop_get(name);
        struct std_value r = v ? std_str(v) : STDNULL;
        int ret = platch_respond_success_std(h, &r);
        std_free(&r);
        return ret;
    }
    if (!strcmp(m, "command")) {
        int n;
        char **a = args_list(o, &n);
        if (!a || n == 0) { free(a); return platch_respond_illegal_arg_std(h, "Missing 'args' list"); }
        if (!c->video) {
            if (!audio.mpv) { free(a); return platch_respond_error_std(h, "NOT_INITIALIZED", "Player not initialized", NULL); }
            mpv_command_async(audio.mpv, (uint64_t)(uintptr_t)h, (const char **)a);
            free(a);
            return 0;
        }
        if (video.vs == V_PLAYING) {
            ipc_cmd(strarr((const char *const *)a, n), P_COMMAND, h);
        } else if (video.vs == V_IDLE && !strcmp(a[0], "loadfile") && n >= 2) {
            video_start(a, n, h);
        } else if (!strcmp(a[0], "change-list") && n >= 4 && !strcmp(a[1], "http-header-fields")) {
            if (!strcmp(a[2], "clr")) { free(video.headers); video.headers = NULL; }
            else if (!strcmp(a[2], "append") || !strcmp(a[2], "add")) {
                size_t len = (video.headers ? strlen(video.headers) + 1 : 0) + strlen(a[3]) + 1;
                char *s = malloc(len);
                snprintf(s, len, "%s%s%s", video.headers ? video.headers : "", video.headers ? "," : "", a[3]);
                free(video.headers);
                video.headers = s;
            }
            respond_null(h);
        } else {
            respond_null(h); /* nothing plays: stop/seek/etc. have nothing to act on */
        }
        free(a);
        return 0;
    }
    if (!strcmp(m, "setLogLevel")) {
        const char *lvl = arg_str(o, "level");
        if (!c->video && audio.mpv && lvl) mpv_request_log_messages(audio.mpv, lvl);
        return respond_null(h);
    }
    if (!strcmp(m, "isHDRSupported")) return platch_respond_success_std(h, &STDBOOL(false)); /* NotaTV's player handles HDR */
    /* NotaTV: the title, poster and skip markers for NotaTV's player (sent by our Dart patch before loadfile) */
    if (c->video && (!strcmp(m, "notaMeta") || !strcmp(m, "notaSkip"))) {
        const char *json = arg_str(o, "json");
        cJSON **slot = !strcmp(m, "notaMeta") ? &video.extra.meta : &video.extra.skip;
        cJSON_Delete(*slot);
        *slot = json ? cJSON_Parse(json) : NULL;
        if (!strcmp(m, "notaSkip") && video.vs != V_IDLE && json) {
            size_t len = strlen(json) + 8;
            char *line = malloc(len);
            snprintf(line, len, "skip\t%s\n", json);
            ctl_send(line);
            free(line);
        }
        return respond_null(h);
    }
    if (!strcmp(m, "setVisible") || !strcmp(m, "setVideoRect") || !strcmp(m, "updateFrame") ||
        !strcmp(m, "setVideoZoom") || !strcmp(m, "awaitDisplayModeSwitch"))
        return respond_null(h);
    if (!strcmp(m, "setVideoFrameRate")) return platch_respond_success_std(h, &STDBOOL(false));
    return platch_respond_not_implemented(h);
}

static int on_video(char *ch, struct platch_obj *o, FlutterPlatformMessageResponseHandle *h) { (void)ch; return on_method(&video, o, h); }
static int on_audio(char *ch, struct platch_obj *o, FlutterPlatformMessageResponseHandle *h) { (void)ch; return on_method(&audio, o, h); }

static int on_events(struct core *c, struct platch_obj *o, FlutterPlatformMessageResponseHandle *h) {
    if (!strcmp(o->method, "listen")) c->listening = true;
    else if (!strcmp(o->method, "cancel")) c->listening = false;
    else return platch_respond_not_implemented(h);
    return respond_null(h);
}
static int on_video_ev(char *ch, struct platch_obj *o, FlutterPlatformMessageResponseHandle *h) { (void)ch; return on_events(&video, o, h); }
static int on_audio_ev(char *ch, struct platch_obj *o, FlutterPlatformMessageResponseHandle *h) { (void)ch; return on_events(&audio, o, h); }

static enum plugin_init_result nota_mpv_init(struct flutterpi *fpi, void **ud) {
    (void)fpi; (void)ud;
    const char *path = getenv("NOTA_PEEZY_SOCK");
    if (path && *path) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            ctl_fd = fd;
            flutterpi_sd_event_add_io(&ctl_src, fd, EPOLLIN, on_ctl, NULL);
        } else {
            LOG_ERROR("nota_mpv: cannot reach NotaTV at %s: %s\n", path, strerror(errno));
            if (fd >= 0) close(fd);
        }
    }
    if (plugin_registry_set_receiver_locked(video.chan, kStandardMethodCall, on_video) ||
        plugin_registry_set_receiver_locked(video.evchan, kStandardMethodCall, on_video_ev) ||
        plugin_registry_set_receiver_locked(audio.chan, kStandardMethodCall, on_audio) ||
        plugin_registry_set_receiver_locked(audio.evchan, kStandardMethodCall, on_audio_ev))
        return PLUGIN_INIT_RESULT_ERROR;
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

static void nota_mpv_deinit(struct flutterpi *fpi, void *ud) {
    (void)fpi; (void)ud;
    audio_dispose();
    ipc_close();
    plugin_registry_remove_receiver_locked(video.chan);
    plugin_registry_remove_receiver_locked(video.evchan);
    plugin_registry_remove_receiver_locked(audio.chan);
    plugin_registry_remove_receiver_locked(audio.evchan);
}

FLUTTERPI_PLUGIN("nota_mpv", nota_mpv, nota_mpv_init, nota_mpv_deinit)
