/* NotaTV shell - DLNA/UPnP MediaRenderer: apps on a phone/PC can send video, music and pictures to the TV
 * (e.g. BubbleUPnP, "Cast to device" in Windows, the Plex/Jellyfin servers' DLNA). libupnp (pupnp, BSD) handles
 * discovery (SSDP), SOAP control and events in its own threads; the playback itself happens in the player (mpv)
 * on the main thread. Local network only. */
#include "tv.h"
#include <upnp.h>
#include <upnptools.h>
#include <ixml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>

#define WEBROOT "/tmp/notatv-dlna"
#define AVT "urn:schemas-upnp-org:service:AVTransport:1"
#define RC "urn:schemas-upnp-org:service:RenderingControl:1"
#define CM "urn:schemas-upnp-org:service:ConnectionManager:1"
#define AVT_ID "urn:upnp-org:serviceId:AVTransport"
#define RC_ID "urn:upnp-org:serviceId:RenderingControl"

/* only addresses on the network can be played from a phone: http(s) and rtsp */
static bool net_url(const char *u) {
    return u && (!strncasecmp(u, "http://", 7) || !strncasecmp(u, "https://", 8) || !strncasecmp(u, "rtsp://", 7));
}

static const char *SINK =
    "http-get:*:video/mp4:*,http-get:*:video/x-matroska:*,http-get:*:video/webm:*,http-get:*:video/mpeg:*,"
    "http-get:*:video/x-msvideo:*,http-get:*:video/quicktime:*,http-get:*:video/mp2t:*,http-get:*:video/x-flv:*,"
    "http-get:*:application/vnd.apple.mpegurl:*,http-get:*:application/x-mpegurl:*,"
    "http-get:*:audio/mpeg:*,http-get:*:audio/mp4:*,http-get:*:audio/aac:*,http-get:*:audio/flac:*,http-get:*:audio/x-flac:*,"
    "http-get:*:audio/ogg:*,http-get:*:audio/wav:*,http-get:*:audio/x-wav:*,http-get:*:audio/L16:*,"
    "http-get:*:image/jpeg:*,http-get:*:image/png:*,http-get:*:image/gif:*,http-get:*:image/webp:*,http-get:*:*:*";

/* ---------- shared state (UPnP threads <-> main thread) ---------- */
enum { C_NONE, C_PLAY, C_PAUSE, C_STOP, C_SEEK, C_VOLUME, C_MUTE };
typedef struct { int cmd; double arg; } DCmd;

static SDL_Mutex *mu;
static UpnpDevice_Handle dev = -1;
static bool up;
static double next_try;
static char udn[64];
static struct {
    char uri[2048], meta[8192];      /* set by SetAVTransportURI */
    char state[32];                  /* TransportState */
    double pos, dur;                 /* from the player */
    int volume;
    bool mute, new_uri;
    DCmd q[16];
    int qn;
} S = {.state = "NO_MEDIA_PRESENT", .volume = 100};

static void push(int cmd, double arg) {
    SDL_LockMutex(mu);
    if (S.qn < 16) S.q[S.qn++] = (DCmd){cmd, arg};
    SDL_UnlockMutex(mu);
}

/* ---------- which devices may control the TV ----------
 * Like YouTube cast: the first time a phone/PC sends something to play (or controls playback), the TV asks
 * "Allow ...?". The UPnP thread waits for the answer (up to WAIT_S), so the phone's request simply succeeds once
 * it is allowed. Allowed devices are remembered until tvshell restarts; a "Deny" holds for 10 minutes.
 * Only the main thread can ask (not during playback): a new device is turned away meanwhile. */
#define MAX_PEERS 16
#define WAIT_S 25
static struct {
    char ok[MAX_PEERS][64];
    int nok;
    struct { char ip[64]; double t; } no[MAX_PEERS];
    int nno;
    char ask_ip[64];
    bool ask;           /* the main thread should ask about ask_ip */
    double asked_t;     /* the question is on screen (0 = none) */
} P;

static void peer_ip(UpnpActionRequest *req, char *ip, size_t n) {
    ip[0] = 0;
    const struct sockaddr_storage *ss = UpnpActionRequest_get_CtrlPtIPAddr(req);
    if (!ss) return;
    if (ss->ss_family == AF_INET) inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, ip, (socklen_t)n);
    else if (ss->ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)ss)->sin6_addr, ip, (socklen_t)n);
        if (!strncmp(ip, "::ffff:", 7)) memmove(ip, ip + 7, strlen(ip + 7) + 1);
    }
}
/* mu locked */
static int peer_state(const char *ip) { /* 1 allowed, -1 denied recently, 0 unknown */
    if (!strcmp(ip, "127.0.0.1") || !strcmp(ip, "::1")) return 1;
    for (int i = 0; i < P.nok; i++) if (!strcmp(P.ok[i], ip)) return 1;
    double now = app_time();
    for (int i = 0; i < P.nno;) {
        if (now - P.no[i].t >= 600) { P.no[i] = P.no[--P.nno]; continue; }
        if (!strcmp(P.no[i].ip, ip)) return -1;
        i++;
    }
    return 0;
}
/* UPnP thread: may ip control the TV? Asks on the TV and waits for the answer if it is new. */
static bool peer_allowed(const char *ip) {
    if (!ip[0]) return false;
    SDL_LockMutex(mu);
    int st = peer_state(ip);
    if (st == 0 && !P.ask && !P.asked_t) { snprintf(P.ask_ip, sizeof P.ask_ip, "%s", ip); P.ask = true; }
    SDL_UnlockMutex(mu);
    for (int i = 0; st == 0 && i < WAIT_S * 10; i++) {
        SDL_Delay(100);
        SDL_LockMutex(mu);
        st = peer_state(ip);
        bool open = P.ask || (P.asked_t && !strcmp(P.ask_ip, ip));
        SDL_UnlockMutex(mu);
        if (!open && st == 0) break; /* someone else is being asked, or the question was closed without an answer */
    }
    if (st != 1) SDL_Log("dlna: %s afvist (ikke godkendt på TV'et)", ip);
    return st == 1;
}
static void peer_answer(void *ud, bool yes) {
    (void)ud;
    SDL_LockMutex(mu);
    if (P.ask_ip[0]) {
        if (yes) {
            if (P.nok < MAX_PEERS) snprintf(P.ok[P.nok++], sizeof P.ok[0], "%s", P.ask_ip);
            else { memmove(P.ok[0], P.ok[1], sizeof P.ok[0] * (MAX_PEERS - 1)); snprintf(P.ok[MAX_PEERS - 1], sizeof P.ok[0], "%s", P.ask_ip); }
        } else {
            int i = P.nno < MAX_PEERS ? P.nno++ : 0;
            snprintf(P.no[i].ip, sizeof P.no[i].ip, "%s", P.ask_ip);
            P.no[i].t = app_time();
        }
    }
    P.asked_t = 0;
    SDL_UnlockMutex(mu);
}
static void peer_tick(void) { /* main thread, outside playback */
    SDL_LockMutex(mu);
    bool ask = P.ask;
    char ip[64];
    snprintf(ip, sizeof ip, "%s", P.ask_ip);
    if (ask) { P.ask = false; P.asked_t = app_time(); }
    else if (P.asked_t && app_time() - P.asked_t > WAIT_S + 5) P.asked_t = 0; /* closed with Home: ask again next time */
    SDL_UnlockMutex(mu);
    if (ask) {
        char msg[200];
        snprintf(msg, sizeof msg, T("Allow %s to play on the TV (DLNA)?"), ip);
        confirm_open(T("Play from another device"), msg, T("Allow"), T("Deny"), peer_answer, NULL);
    }
}

/* ---------- small helpers ---------- */
static void fmt_time(double t, char *b, size_t n) {
    if (t < 0) t = 0;
    int s = (int)t;
    snprintf(b, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
}
static double parse_time(const char *s) { /* "H:MM:SS(.mmm)" */
    int h = 0, m = 0;
    double sec = 0;
    if (sscanf(s, "%d:%d:%lf", &h, &m, &sec) == 3) return h * 3600 + m * 60 + sec;
    return atof(s);
}
static char *arg_of(IXML_Document *doc, const char *name) {
    IXML_NodeList *l = ixmlDocument_getElementsByTagName(doc, name);
    char *v = NULL;
    if (l) {
        IXML_Node *n = ixmlNodeList_item(l, 0), *t = n ? ixmlNode_getFirstChild(n) : NULL;
        const char *val = t ? ixmlNode_getNodeValue(t) : NULL;
        v = str_dup(val ? val : "");
        ixmlNodeList_free(l);
    }
    return v ? v : str_dup("");
}
/* <dc:title>…</dc:title> in DIDL-Lite (simple text search; XML entities for & < > " ' are decoded) */
static void didl(const char *meta, const char *tag, char *out, size_t n) {
    out[0] = 0;
    char open[48];
    snprintf(open, sizeof open, "<%s", tag);
    const char *a = strstr(meta, open);
    if (!a || !(a = strchr(a, '>'))) return;
    a++;
    const char *b = strstr(a, "</");
    if (!b) return;
    size_t o = 0;
    for (const char *p = a; p < b && o + 1 < n; p++) {
        if (*p == '&') {
            static const struct { const char *e; char c; } ent[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool hit = false;
            for (size_t i = 0; i < 5 && !hit; i++)
                if (!strncmp(p, ent[i].e, strlen(ent[i].e))) { out[o++] = ent[i].c; p += strlen(ent[i].e) - 1; hit = true; }
            if (hit) continue;
        }
        out[o++] = *p;
    }
    out[o] = 0;
}

/* ---------- events (LastChange) ---------- */
static void notify_avt(void) {
    if (!up) return;
    char ev[1024], d[32];
    SDL_LockMutex(mu);
    fmt_time(S.dur, d, sizeof d);
    snprintf(ev, sizeof ev,
             "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"><InstanceID val=\"0\"><TransportState val=\"%s\"/>"
             "<TransportStatus val=\"OK\"/><CurrentTrackDuration val=\"%s\"/><CurrentMediaDuration val=\"%s\"/></InstanceID></Event>",
             S.state, d, d);
    SDL_UnlockMutex(mu);
    const char *names[] = {"LastChange"}, *vals[] = {ev};
    UpnpNotify(dev, udn, AVT_ID, names, vals, 1);
}
static void notify_rc(void) {
    if (!up) return;
    char ev[512];
    SDL_LockMutex(mu);
    snprintf(ev, sizeof ev,
             "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"><InstanceID val=\"0\"><Volume channel=\"Master\" val=\"%d\"/>"
             "<Mute channel=\"Master\" val=\"%d\"/></InstanceID></Event>", S.volume, S.mute ? 1 : 0);
    SDL_UnlockMutex(mu);
    const char *names[] = {"LastChange"}, *vals[] = {ev};
    UpnpNotify(dev, udn, RC_ID, names, vals, 1);
}
static void set_state(const char *st) {
    SDL_LockMutex(mu);
    bool changed = strcmp(S.state, st) != 0;
    snprintf(S.state, sizeof S.state, "%s", st);
    SDL_UnlockMutex(mu);
    if (changed) notify_avt();
}

/* ---------- SOAP actions (UPnP threads) ---------- */
#define ADD(name, val) UpnpAddToActionResponse(&res, act, svc, name, val)

static int on_action(UpnpActionRequest *req) {
    const char *act = UpnpActionRequest_get_ActionName_cstr(req), *sid = UpnpActionRequest_get_ServiceID_cstr(req);
    IXML_Document *in = UpnpActionRequest_get_ActionRequest(req), *res = NULL;
    int kind = strstr(sid, "AVTransport") ? 0 : strstr(sid, "RenderingControl") ? 1 : 2;
    const char *svc = kind == 0 ? AVT : kind == 1 ? RC : CM;
    char b1[32], b2[32];
    /* reading the state is open to everyone on the network; playing and controlling only for allowed devices */
    bool control = (kind == 0 && (!strcmp(act, "SetAVTransportURI") || !strcmp(act, "Play") || !strcmp(act, "Pause") ||
                                  !strcmp(act, "Stop") || !strcmp(act, "Seek"))) ||
                   (kind == 1 && (!strcmp(act, "SetVolume") || !strcmp(act, "SetMute")));
    if (control) {
        char ip[64];
        peer_ip(req, ip, sizeof ip);
        if (!peer_allowed(ip)) {
            UpnpActionRequest_set_ErrCode(req, 501); /* action failed */
            return 0;
        }
    }
    if (kind == 0) {
        if (!strcmp(act, "SetAVTransportURI")) {
            char *u = arg_of(in, "CurrentURI"), *m = arg_of(in, "CurrentURIMetaData");
            if (!net_url(u)) { /* file://, mpv's own protocols (edl://, av:// ...): never from the network */
                SDL_Log("dlna: afviser adressen (kun http, https og rtsp)");
                free(u); free(m);
                UpnpActionRequest_set_ErrCode(req, 716); /* resource not found */
                return 0;
            }
            SDL_LockMutex(mu);
            snprintf(S.uri, sizeof S.uri, "%s", u);
            snprintf(S.meta, sizeof S.meta, "%s", m);
            S.new_uri = true;
            bool playing = !strcmp(S.state, "PLAYING") || !strcmp(S.state, "PAUSED_PLAYBACK");
            if (!playing) snprintf(S.state, sizeof S.state, "STOPPED");
            SDL_UnlockMutex(mu);
            if (playing) push(C_PLAY, 0); /* change track while something is playing */
            free(u); free(m);
            notify_avt();
        } else if (!strcmp(act, "Play")) push(C_PLAY, 0);
        else if (!strcmp(act, "Pause")) push(C_PAUSE, 0);
        else if (!strcmp(act, "Stop")) push(C_STOP, 0);
        else if (!strcmp(act, "Seek")) {
            char *unit = arg_of(in, "Unit"), *tgt = arg_of(in, "Target");
            if (!strcmp(unit, "REL_TIME") || !strcmp(unit, "ABS_TIME")) push(C_SEEK, parse_time(tgt));
            free(unit); free(tgt);
        } else if (!strcmp(act, "GetTransportInfo")) {
            SDL_LockMutex(mu);
            char st[32];
            snprintf(st, sizeof st, "%s", S.state);
            SDL_UnlockMutex(mu);
            ADD("CurrentTransportState", st);
            ADD("CurrentTransportStatus", "OK");
            ADD("CurrentSpeed", "1");
        } else if (!strcmp(act, "GetPositionInfo")) {
            SDL_LockMutex(mu);
            fmt_time(S.dur, b1, sizeof b1);
            fmt_time(S.pos, b2, sizeof b2);
            char *uri = str_dup(S.uri), *meta = str_dup(S.meta);
            SDL_UnlockMutex(mu);
            ADD("Track", uri[0] ? "1" : "0");
            ADD("TrackDuration", b1);
            ADD("TrackMetaData", meta);
            ADD("TrackURI", uri);
            ADD("RelTime", b2);
            ADD("AbsTime", b2);
            ADD("RelCount", "2147483647");
            ADD("AbsCount", "2147483647");
            free(uri); free(meta);
        } else if (!strcmp(act, "GetMediaInfo")) {
            SDL_LockMutex(mu);
            fmt_time(S.dur, b1, sizeof b1);
            char *uri = str_dup(S.uri), *meta = str_dup(S.meta);
            SDL_UnlockMutex(mu);
            ADD("NrTracks", uri[0] ? "1" : "0");
            ADD("MediaDuration", b1);
            ADD("CurrentURI", uri);
            ADD("CurrentURIMetaData", meta);
            ADD("NextURI", "");
            ADD("NextURIMetaData", "");
            ADD("PlayMedium", "NETWORK");
            ADD("RecordMedium", "NOT_IMPLEMENTED");
            ADD("WriteStatus", "NOT_IMPLEMENTED");
            free(uri); free(meta);
        } else if (!strcmp(act, "GetTransportSettings")) {
            ADD("PlayMode", "NORMAL");
            ADD("RecQualityMode", "NOT_IMPLEMENTED");
        } else if (!strcmp(act, "GetDeviceCapabilities")) {
            ADD("PlayMedia", "NETWORK");
            ADD("RecMedia", "NOT_IMPLEMENTED");
            ADD("RecQualityModes", "NOT_IMPLEMENTED");
        } else if (!strcmp(act, "GetCurrentTransportActions")) {
            ADD("Actions", "Play,Pause,Stop,Seek");
        } else {
            UpnpActionRequest_set_ErrCode(req, 401); /* unknown action */
            return 0;
        }
    } else if (kind == 1) {
        if (!strcmp(act, "GetVolume")) {
            snprintf(b1, sizeof b1, "%d", S.volume);
            ADD("CurrentVolume", b1);
        } else if (!strcmp(act, "SetVolume")) {
            char *v = arg_of(in, "DesiredVolume");
            int vol = atoi(v);
            free(v);
            SDL_LockMutex(mu);
            S.volume = vol < 0 ? 0 : vol > 100 ? 100 : vol;
            SDL_UnlockMutex(mu);
            push(C_VOLUME, S.volume);
            notify_rc();
        } else if (!strcmp(act, "GetMute")) {
            ADD("CurrentMute", S.mute ? "1" : "0");
        } else if (!strcmp(act, "SetMute")) {
            char *v = arg_of(in, "DesiredMute");
            S.mute = !strcmp(v, "1") || !strcasecmp(v, "true");
            free(v);
            push(C_MUTE, S.mute);
            notify_rc();
        } else if (!strcmp(act, "ListPresets")) {
            ADD("CurrentPresetNameList", "FactoryDefaults");
        } else if (!strcmp(act, "SelectPreset")) {
        } else {
            UpnpActionRequest_set_ErrCode(req, 401);
            return 0;
        }
    } else {
        if (!strcmp(act, "GetProtocolInfo")) {
            ADD("Source", "");
            ADD("Sink", SINK);
        } else if (!strcmp(act, "GetCurrentConnectionIDs")) {
            ADD("ConnectionIDs", "0");
        } else if (!strcmp(act, "GetCurrentConnectionInfo")) {
            ADD("RcsID", "0");
            ADD("AVTransportID", "0");
            ADD("ProtocolInfo", "");
            ADD("PeerConnectionManager", "");
            ADD("PeerConnectionID", "-1");
            ADD("Direction", "Input");
            ADD("Status", "OK");
        } else {
            UpnpActionRequest_set_ErrCode(req, 401);
            return 0;
        }
    }
    if (!res) res = UpnpMakeActionResponse(act, svc, 0, NULL); /* action without reply values */
    UpnpActionRequest_set_ActionResult(req, res);
    UpnpActionRequest_set_ErrCode(req, 0);
    return 0;
}

static int on_event(Upnp_EventType type, const void *ev, void *cookie) {
    (void)cookie;
    if (type == UPNP_CONTROL_ACTION_REQUEST) return on_action((UpnpActionRequest *)ev);
    if (type == UPNP_EVENT_SUBSCRIPTION_REQUEST) { /* new subscribers get the state right away */
        const UpnpSubscriptionRequest *r = ev;
        const char *sid = UpnpSubscriptionRequest_get_ServiceId_cstr(r);
        char evx[1024];
        SDL_LockMutex(mu);
        if (strstr(sid, "RenderingControl"))
            snprintf(evx, sizeof evx, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"><InstanceID val=\"0\"><Volume channel=\"Master\" val=\"%d\"/><Mute channel=\"Master\" val=\"%d\"/></InstanceID></Event>", S.volume, S.mute ? 1 : 0);
        else if (strstr(sid, "AVTransport"))
            snprintf(evx, sizeof evx, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"><InstanceID val=\"0\"><TransportState val=\"%s\"/><TransportStatus val=\"OK\"/></InstanceID></Event>", S.state);
        else snprintf(evx, sizeof evx, "%s", SINK);
        SDL_UnlockMutex(mu);
        bool cm = !strstr(sid, "RenderingControl") && !strstr(sid, "AVTransport");
        const char *names[] = {cm ? "SinkProtocolInfo" : "LastChange"}, *vals[] = {evx};
        UpnpAcceptSubscription(dev, UpnpSubscriptionRequest_get_UDN_cstr(r), sid, names, vals, 1, UpnpSubscriptionRequest_get_SID_cstr(r));
    }
    return 0;
}

/* ---------- descriptions (served by libupnp's web server) ---------- */
static void write_file(const char *name, const char *data) {
    char p[256];
    snprintf(p, sizeof p, WEBROOT "/%s", name);
    FILE *f = fopen(p, "w");
    if (f) { fputs(data, f); fclose(f); }
}

/* SCPD from compact tables: action + arguments "i|u name relatedVariable" */
typedef struct { const char *name; const char *args[12]; } Act;
typedef struct { const char *name, *type; bool events; } Var;
static char *scpd(const Act *a, int na, const Var *v, int nv) {
    size_t cap = 32768, n = 0;
    char *o = malloc(cap);
    n += snprintf(o + n, cap - n, "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><actionList>");
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < na; i++) {
        n += snprintf(o + n, cap - n, "<action><name>%s</name><argumentList>", a[i].name);
        if (n > cap - 1) n = cap - 1;
        for (int k = 0; a[i].args[k]; k++) {
            char dir, an[64], rv[64];
            sscanf(a[i].args[k], "%c %63s %63s", &dir, an, rv);
            n += snprintf(o + n, cap - n, "<argument><name>%s</name><direction>%s</direction><relatedStateVariable>%s</relatedStateVariable></argument>",
                          an, dir == 'i' ? "in" : "out", rv);
            if (n > cap - 1) n = cap - 1;
        }
        n += snprintf(o + n, cap - n, "</argumentList></action>");
        if (n > cap - 1) n = cap - 1;
    }
    n += snprintf(o + n, cap - n, "</actionList><serviceStateTable>");
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < nv; i++) {
        n += snprintf(o + n, cap - n, "<stateVariable sendEvents=\"%s\"><name>%s</name><dataType>%s</dataType></stateVariable>",
                      v[i].events ? "yes" : "no", v[i].name, v[i].type);
        if (n > cap - 1) n = cap - 1;
    }
    snprintf(o + n, cap - n, "</serviceStateTable></scpd>");
    return o;
}

static void write_scpds(void) {
    static const Act avt[] = {
        {"SetAVTransportURI", {"i InstanceID A_ARG_TYPE_InstanceID", "i CurrentURI AVTransportURI", "i CurrentURIMetaData AVTransportURIMetaData"}},
        {"Play", {"i InstanceID A_ARG_TYPE_InstanceID", "i Speed TransportPlaySpeed"}},
        {"Pause", {"i InstanceID A_ARG_TYPE_InstanceID"}},
        {"Stop", {"i InstanceID A_ARG_TYPE_InstanceID"}},
        {"Seek", {"i InstanceID A_ARG_TYPE_InstanceID", "i Unit A_ARG_TYPE_SeekMode", "i Target A_ARG_TYPE_SeekTarget"}},
        {"GetTransportInfo", {"i InstanceID A_ARG_TYPE_InstanceID", "o CurrentTransportState TransportState", "o CurrentTransportStatus TransportStatus", "o CurrentSpeed TransportPlaySpeed"}},
        {"GetPositionInfo", {"i InstanceID A_ARG_TYPE_InstanceID", "o Track CurrentTrack", "o TrackDuration CurrentTrackDuration", "o TrackMetaData CurrentTrackMetaData",
                             "o TrackURI CurrentTrackURI", "o RelTime RelativeTimePosition", "o AbsTime AbsoluteTimePosition", "o RelCount RelativeCounterPosition", "o AbsCount AbsoluteCounterPosition"}},
        {"GetMediaInfo", {"i InstanceID A_ARG_TYPE_InstanceID", "o NrTracks NumberOfTracks", "o MediaDuration CurrentMediaDuration", "o CurrentURI AVTransportURI",
                          "o CurrentURIMetaData AVTransportURIMetaData", "o NextURI NextAVTransportURI", "o NextURIMetaData NextAVTransportURIMetaData",
                          "o PlayMedium PlaybackStorageMedium", "o RecordMedium RecordStorageMedium", "o WriteStatus RecordMediumWriteStatus"}},
        {"GetTransportSettings", {"i InstanceID A_ARG_TYPE_InstanceID", "o PlayMode CurrentPlayMode", "o RecQualityMode CurrentRecordQualityMode"}},
        {"GetDeviceCapabilities", {"i InstanceID A_ARG_TYPE_InstanceID", "o PlayMedia PossiblePlaybackStorageMedia", "o RecMedia PossibleRecordStorageMedia", "o RecQualityModes PossibleRecordQualityModes"}},
        {"GetCurrentTransportActions", {"i InstanceID A_ARG_TYPE_InstanceID", "o Actions CurrentTransportActions"}},
    };
    static const Var avtv[] = {
        {"A_ARG_TYPE_InstanceID", "ui4", 0}, {"AVTransportURI", "string", 0}, {"AVTransportURIMetaData", "string", 0}, {"TransportPlaySpeed", "string", 0},
        {"A_ARG_TYPE_SeekMode", "string", 0}, {"A_ARG_TYPE_SeekTarget", "string", 0}, {"TransportState", "string", 0}, {"TransportStatus", "string", 0},
        {"CurrentTrack", "ui4", 0}, {"CurrentTrackDuration", "string", 0}, {"CurrentTrackMetaData", "string", 0}, {"CurrentTrackURI", "string", 0},
        {"RelativeTimePosition", "string", 0}, {"AbsoluteTimePosition", "string", 0}, {"RelativeCounterPosition", "i4", 0}, {"AbsoluteCounterPosition", "i4", 0},
        {"NumberOfTracks", "ui4", 0}, {"CurrentMediaDuration", "string", 0}, {"NextAVTransportURI", "string", 0}, {"NextAVTransportURIMetaData", "string", 0},
        {"PlaybackStorageMedium", "string", 0}, {"RecordStorageMedium", "string", 0}, {"RecordMediumWriteStatus", "string", 0}, {"CurrentPlayMode", "string", 0},
        {"CurrentRecordQualityMode", "string", 0}, {"PossiblePlaybackStorageMedia", "string", 0}, {"PossibleRecordStorageMedia", "string", 0},
        {"PossibleRecordQualityModes", "string", 0}, {"CurrentTransportActions", "string", 0}, {"LastChange", "string", 1},
    };
    static const Act rc[] = {
        {"ListPresets", {"i InstanceID A_ARG_TYPE_InstanceID", "o CurrentPresetNameList PresetNameList"}},
        {"SelectPreset", {"i InstanceID A_ARG_TYPE_InstanceID", "i PresetName A_ARG_TYPE_PresetName"}},
        {"GetVolume", {"i InstanceID A_ARG_TYPE_InstanceID", "i Channel A_ARG_TYPE_Channel", "o CurrentVolume Volume"}},
        {"SetVolume", {"i InstanceID A_ARG_TYPE_InstanceID", "i Channel A_ARG_TYPE_Channel", "i DesiredVolume Volume"}},
        {"GetMute", {"i InstanceID A_ARG_TYPE_InstanceID", "i Channel A_ARG_TYPE_Channel", "o CurrentMute Mute"}},
        {"SetMute", {"i InstanceID A_ARG_TYPE_InstanceID", "i Channel A_ARG_TYPE_Channel", "i DesiredMute Mute"}},
    };
    static const Var rcv[] = {
        {"A_ARG_TYPE_InstanceID", "ui4", 0}, {"A_ARG_TYPE_Channel", "string", 0}, {"A_ARG_TYPE_PresetName", "string", 0},
        {"PresetNameList", "string", 0}, {"Volume", "ui2", 0}, {"Mute", "boolean", 0}, {"LastChange", "string", 1},
    };
    static const Act cm[] = {
        {"GetProtocolInfo", {"o Source SourceProtocolInfo", "o Sink SinkProtocolInfo"}},
        {"GetCurrentConnectionIDs", {"o ConnectionIDs CurrentConnectionIDs"}},
        {"GetCurrentConnectionInfo", {"i ConnectionID A_ARG_TYPE_ConnectionID", "o RcsID A_ARG_TYPE_RcsID", "o AVTransportID A_ARG_TYPE_AVTransportID",
                                      "o ProtocolInfo A_ARG_TYPE_ProtocolInfo", "o PeerConnectionManager A_ARG_TYPE_ConnectionManager",
                                      "o PeerConnectionID A_ARG_TYPE_ConnectionID", "o Direction A_ARG_TYPE_Direction", "o Status A_ARG_TYPE_ConnectionStatus"}},
    };
    static const Var cmv[] = {
        {"SourceProtocolInfo", "string", 1}, {"SinkProtocolInfo", "string", 1}, {"CurrentConnectionIDs", "string", 1},
        {"A_ARG_TYPE_ConnectionID", "i4", 0}, {"A_ARG_TYPE_RcsID", "i4", 0}, {"A_ARG_TYPE_AVTransportID", "i4", 0},
        {"A_ARG_TYPE_ProtocolInfo", "string", 0}, {"A_ARG_TYPE_ConnectionManager", "string", 0}, {"A_ARG_TYPE_Direction", "string", 0},
        {"A_ARG_TYPE_ConnectionStatus", "string", 0},
    };
#define N(x) (int)(sizeof x / sizeof x[0])
    char *s;
    write_file("avt.xml", s = scpd(avt, N(avt), avtv, N(avtv))); free(s);
    write_file("rc.xml", s = scpd(rc, N(rc), rcv, N(rcv))); free(s);
    write_file("cm.xml", s = scpd(cm, N(cm), cmv, N(cmv))); free(s);
}

static void dlna_start(void) {
    if (UpnpInit2(NULL, 0) != UPNP_E_SUCCESS) return; /* no network yet: try again later */
    mkdir(WEBROOT, 0755);
    write_scpds();
    UpnpSetWebServerRootDir(WEBROOT);
    const char *id = cfg_get("dlna_uuid", "");
    if (!*id) { /* fixed id per box, so apps recognize it */
        char u[40];
        snprintf(u, sizeof u, "%08x-%04x-4%03x-a%03x-%012llx", (unsigned)SDL_rand_bits(), (unsigned)SDL_rand_bits() & 0xffff,
                 (unsigned)SDL_rand_bits() & 0xfff, (unsigned)SDL_rand_bits() & 0xfff, (unsigned long long)SDL_rand_bits() << 16 | (SDL_rand_bits() & 0xffff));
        cfg_set("dlna_uuid", u);
        id = cfg_get("dlna_uuid", "");
    }
    snprintf(udn, sizeof udn, "uuid:%s", id);
    char name[128];
    snprintf(name, sizeof name, "%s", cfg_get("device_name", "NotaTV"));
    for (char *p = name; *p; p++) if (*p == '<' || *p == '>' || *p == '&') *p = ' ';
    char *desc = str_printf(
        "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion><device>"
        "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType><friendlyName>%s</friendlyName>"
        "<manufacturer>NotaTV</manufacturer><modelName>NotaTV</modelName><modelNumber>" TV_VERSION "</modelNumber>"
        "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC><UDN>%s</UDN><serviceList>"
        "<service><serviceType>" AVT "</serviceType><serviceId>" AVT_ID "</serviceId><SCPDURL>/avt.xml</SCPDURL><controlURL>/ctl/avt</controlURL><eventSubURL>/evt/avt</eventSubURL></service>"
        "<service><serviceType>" RC "</serviceType><serviceId>" RC_ID "</serviceId><SCPDURL>/rc.xml</SCPDURL><controlURL>/ctl/rc</controlURL><eventSubURL>/evt/rc</eventSubURL></service>"
        "<service><serviceType>" CM "</serviceType><serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId><SCPDURL>/cm.xml</SCPDURL><controlURL>/ctl/cm</controlURL><eventSubURL>/evt/cm</eventSubURL></service>"
        "</serviceList></device></root>", name, udn);
    int r = UpnpRegisterRootDevice2(UPNPREG_BUF_DESC, desc, strlen(desc), 1, on_event, NULL, &dev);
    free(desc);
    if (r != UPNP_E_SUCCESS) { SDL_Log("dlna: registrering fejlede (%d)", r); UpnpFinish(); return; }
    UpnpSendAdvertisement(dev, 1800);
    up = true;
    SDL_Log("dlna: klar som \"%s\" på %s:%u", name, UpnpGetServerIpAddress(), UpnpGetServerPort());
}

/* ---------- playback (main thread) ---------- */
static bool playing_now;

static void apply_cmds(bool in_player) {
    DCmd q[16];
    int n;
    SDL_LockMutex(mu);
    n = S.qn;
    memcpy(q, S.q, sizeof(DCmd) * n);
    S.qn = 0;
    bool new_uri = S.new_uri;
    SDL_UnlockMutex(mu);
    for (int i = 0; i < n; i++) {
        char a[32];
        switch (q[i].cmd) {
        case C_PLAY:
            if (in_player && new_uri) { player_cmd((const char *[]){"quit", NULL}); return; } /* new content: start over */
            if (in_player) { player_cmd((const char *[]){"set", "pause", "no", NULL}); set_state("PLAYING"); }
            break;
        case C_PAUSE: if (in_player) { player_cmd((const char *[]){"set", "pause", "yes", NULL}); set_state("PAUSED_PLAYBACK"); } break;
        case C_STOP:
            SDL_LockMutex(mu);
            S.new_uri = false;
            SDL_UnlockMutex(mu);
            if (in_player) player_cmd((const char *[]){"quit", NULL});
            set_state("STOPPED");
            break;
        case C_SEEK:
            snprintf(a, sizeof a, "%.1f", q[i].arg);
            if (in_player) player_cmd((const char *[]){"seek", a, "absolute", NULL});
            break;
        case C_VOLUME:
            snprintf(a, sizeof a, "%d", (int)q[i].arg);
            if (in_player) player_cmd((const char *[]){"set", "volume", a, NULL});
            break;
        case C_MUTE: if (in_player) player_cmd((const char *[]){"set", "mute", q[i].arg ? "yes" : "no", NULL}); break;
        }
    }
    if (!in_player && n) { /* outside the player: only "Play" with new content starts something */
        for (int i = 0; i < n; i++)
            if (q[i].cmd == C_PLAY) {
                SDL_LockMutex(mu);
                bool have = S.uri[0] != 0;
                S.new_uri = have;
                SDL_UnlockMutex(mu);
                if (have) { push(C_NONE, 0); return; } /* dlna_tick starts the playback */
            }
    }
}

static void play_tick(void *ud) {
    (void)ud;
    apply_cmds(true);
    double pos, dur;
    bool paused;
    if (player_state(&pos, &dur, &paused)) {
        SDL_LockMutex(mu);
        bool dur_changed = (int)dur != (int)S.dur;
        S.pos = pos;
        S.dur = dur;
        SDL_UnlockMutex(mu);
        set_state(paused ? "PAUSED_PLAYBACK" : "PLAYING");
        if (dur_changed) notify_avt();
    }
}

static void play_current(void) {
    char uri[2048], meta[8192];
    SDL_LockMutex(mu);
    snprintf(uri, sizeof uri, "%s", S.uri);
    snprintf(meta, sizeof meta, "%s", S.meta);
    S.new_uri = false;
    S.pos = S.dur = 0;
    SDL_UnlockMutex(mu);
    char title[300], artist[200], art[1024], cls[128];
    didl(meta, "dc:title", title, sizeof title);
    didl(meta, "upnp:artist", artist, sizeof artist);
    if (!artist[0]) didl(meta, "dc:creator", artist, sizeof artist);
    didl(meta, "upnp:albumArtURI", art, sizeof art);
    if (!net_url(art)) art[0] = 0; /* a cover only from the network, never a file on the box */
    didl(meta, "upnp:class", cls, sizeof cls);
    bool image = strstr(cls, "imageItem") != NULL;
    char vol[32];
    snprintf(vol, sizeof vol, "--volume=%d", S.volume);
    const char *extra[] = {vol, image ? "--image-display-duration=inf" : NULL, NULL};
    PlayRequest rq = {0};
    rq.url = uri;
    rq.title = title[0] ? title : T("From your phone");
    rq.subtitle = artist[0] ? artist : "DLNA";
    rq.poster = art[0] ? art : NULL;
    rq.extra = extra;
    rq.tick = play_tick;
    playing_now = true;
    set_state("TRANSITIONING");
    player_run(&rq);
    playing_now = false;
    SDL_LockMutex(mu);
    bool again = S.new_uri; /* the phone sent new content while we were playing */
    SDL_UnlockMutex(mu);
    set_state("STOPPED");
    if (again) push(C_NONE, 0);
}

void dlna_tick(void) {
    if (!mu) mu = SDL_CreateMutex();
    if (up) peer_tick();
    if (!up) {
        if (!cfg_int("dlna_enabled", 1) || app_time() < next_try) return;
        next_try = app_time() + 10;
        dlna_start();
        return;
    }
    if (playing_now) return;
    apply_cmds(false);
    SDL_LockMutex(mu);
    bool start = S.new_uri && S.qn > 0 && S.q[0].cmd == C_NONE;
    if (start) S.qn = 0;
    SDL_UnlockMutex(mu);
    if (start) play_current();
}

void dlna_apply(void) { /* turned on/off or a new name: register again */
    if (up) { UpnpUnRegisterRootDevice(dev); UpnpFinish(); up = false; }
    next_try = 0;
}
