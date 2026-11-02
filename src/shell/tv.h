/* NotaTV shell - shared definitions */
#ifndef TV_H
#define TV_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "i18n.h"

/* A function added to this header after the current NOTA_API (below) came out is marked NOTA_NEW: an app must
   check that it is there before calling it (if (app_close) ...), because an older NotaTV with the same NOTA_API
   gives NULL. That way an app can use it and still run on boxes that are not updated yet. */
#define NOTA_NEW __attribute__((weak))
#ifndef TV_VERSION
#define TV_VERSION "dev" /* set from VERSION by the build (tvshell.mk / Makefile) */
#endif
#define UI_W 1920
/* LG over CEC: a held key = repeated presses (the first after ~0.5 s); the same key again within this = held */
#define HOLD_GAP 0.6
#define UI_H 1080

/* ---------- Farver / tema ---------- */
typedef struct { uint8_t r, g, b, a; } Color;
#define RGBA(r, g, b, a) ((Color){(r), (g), (b), (a)})
#define RGB(r, g, b) RGBA(r, g, b, 255)
extern const Color C_BG, C_BG2, C_SURFACE, C_SURFACE2, C_TEXT, C_TEXT2, C_TEXT3,
    C_ACCENT, C_FOCUS, C_DANGER, C_OK;
Color color_alpha(Color c, float a);
Color color_mix(Color a, Color b, float t);

/* ---------- Input ---------- */
typedef enum {
    A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_OK, A_BACK, A_HOME, A_MENU,
    A_PLAYPAUSE, A_PLAY, A_PAUSE, A_STOP, A_FF, A_REW, A_NEXT, A_PREV,
    A_CHUP, A_CHDOWN, A_INFO, A_RED, A_GREEN, A_YELLOW, A_BLUE,
    A_DIGIT, A_CHAR, A_BACKSPACE, A_POWER, A_VOLUP, A_VOLDOWN, A_MUTE,
    A_SUBTITLE, A_AUDIO, A_SEARCH,
} Action;

typedef struct { Action a; uint32_t ch; bool repeat; } InputEvent;

void input_init(bool use_evdev);
void input_shutdown(void);
bool input_evdev_active(void);
/* Reads evdev devices (non-blocking) and puts events in the queue */
void input_poll(void);
void input_push(Action a, uint32_t ch, bool repeat);
bool input_next(InputEvent *ev);
void input_handle_sdl(const SDL_Event *e);
int input_fds(int *fds, int max);
double input_last_activity(void);

/* ---------- Grafik ---------- */
typedef enum { F_REG, F_SEMI, F_BOLD, F_COUNT } FontId;
typedef enum { AL_LEFT, AL_CENTER, AL_RIGHT } Align;

bool gfx_init(SDL_Renderer *r, int out_w, int out_h);
void gfx_shutdown(void);
void gfx_lost(void); /* the renderer disappeared - all textures are invalid */
void gfx_set_hdr(bool on);
bool gfx_get_hdr(void);
void fill_vgrad_dither(float x, float y, float w, float h, Color top, Color bot); /* without banding */
void gfx_stats(int *geom, int *tex_new, int *imgs);
/* own display presentation on the Pi (kms_present.c): UI in 1080p, scaled by the display hardware */
bool kms_present_init(SDL_Renderer *ren, int w, int h);
void kms_present_quit(void);
bool kms_present_active(void);
SDL_Texture *kms_present_target(void);
void kms_present_frame(SDL_Renderer *ren); /* debugging: count since the last call */ /* draw the UI in HDR10 (PQ/BT.2020) when the TV is always in HDR */
SDL_Renderer *gfx_renderer(void);
float gfx_scale(void);

void fill_rect(float x, float y, float w, float h, Color c);
void fill_rrect(float x, float y, float w, float h, float r, Color c);
void fill_rrect_vgrad(float x, float y, float w, float h, float r, Color top, Color bot);
void stroke_rrect(float x, float y, float w, float h, float r, float t, Color c);
void fill_vgrad(float x, float y, float w, float h, Color top, Color bot);
void fill_hgrad(float x, float y, float w, float h, Color l, Color r);
void fill_circle(float cx, float cy, float rad, Color c);
void draw_shadow(float x, float y, float w, float h, float r, float spread, float alpha);
void fill_tri(float x1, float y1, float x2, float y2, float x3, float y3, Color c);

float text_draw(FontId f, float size, float x, float y, Color c, Align al, const char *s);
float text_draw_clip(FontId f, float size, float x, float y, float maxw, Color c, Align al, const char *s);
int text_draw_wrap(FontId f, float size, float x, float y, float w, int maxlines,
                   float lh, Color c, const char *s);
float text_width(FontId f, float size, const char *s);
void clip_push(float x, float y, float w, float h);
void clip_pop(void);

typedef enum { IMG_LOADING, IMG_READY, IMG_FAILED } ImgState;
typedef struct Image Image;
Image *img_get(const char *url);
ImgState img_state(Image *im);
bool img_draw(Image *im, float x, float y, float w, float h, float radius, float alpha);
/* draw with "cover" cropping (fills the whole rectangle) */
bool img_draw_cover(Image *im, float x, float y, float w, float h, float radius, float alpha);
void img_cache_trim(void);
/* save an image (from the cache or fetched now) as raw BGRA scaled to max w*h - for the mpv overlay */
bool img_export_bgra(const char *url, const char *path, int maxw, int maxh, int *ow, int *oh);
bool img_export_bgra_ex(const char *url, const char *path, int maxw, int maxh, int *ow, int *oh, bool hdr);
bool img_cached(const char *url);
typedef struct Layer Layer;
Layer *layer_new(int w, int h);
bool layer_valid(const Layer *l);
bool layer_begin(Layer *l); /* draw into the layer until layer_end */
void layer_end(Layer *l);
void layer_draw(Layer *l, float x, float y, float alpha);
void layer_set_opaque(Layer *l, bool on); /* the layer covers everything: drawn without blending */
void layer_free(Layer *l);
bool img_size(Image *im, int *w, int *h);
/* title logo at the bottom left of a 16:9 image; false if it is not ready */
bool draw_title_logo(const char *url, float x, float y, float maxw, float maxh, float alpha); /* in memory or on disk (can be exported without network) */

typedef enum {
    ICON_TV, ICON_PLEX, ICON_YOUTUBE, ICON_FOLDER, ICON_GEAR,
    ICON_PLAY, ICON_WIFI, ICON_ETH, ICON_STAR, ICON_SEARCH, ICON_POWER,
    ICON_MOON, ICON_BACK, ICON_USB, ICON_MUSIC, ICON_FILM, ICON_CHECK, ICON_LOCK, ICON_CLOCK,
} IconId;
void draw_icon(IconId id, float cx, float cy, float size, Color c);

/* ---------- Jobs (background threads) ---------- */
typedef void (*JobFn)(void *arg);
void jobs_init(int nthreads);
void jobs_shutdown(void);
/* work runs in a background thread, done runs on the main thread */
void job_submit(JobFn work, JobFn done, void *arg); /* fast queue */
typedef enum { JQ_FAST, JQ_IMAGE, JQ_SLOW, JQ_COUNT } JobQueueId;
void job_submit_q(JobQueueId q, JobFn work, JobFn done, void *arg);
void jobs_pump(void);

/* ---------- HTTP ---------- */
typedef struct { char *data; size_t len; long status; } HttpBuf;
void http_init(void);
/* headers: NULL-terminated list "Name: value" */
bool http_request(const char *method, const char *url, const char *const *headers,
                  const char *body, HttpBuf *out, long timeout_s);
bool http_get(const char *url, const char *const *headers, HttpBuf *out);
bool http_request_cookies(const char *method, const char *url, const char *const *headers,
                          const char *body, HttpBuf *out, long timeout_s, const char *cookie_jar);
bool http_download(const char *url, const char *path, long timeout_s);
bool http_download_ua(const char *url, const char *path, long timeout_s, const char *ua);
void http_free(HttpBuf *b);
void http_cookies_forget(void) NOTA_NEW; /* sign-out: drop the cookies http_request_cookies keeps in memory */
char *url_encode(const char *s);

/* ---------- Configuration ---------- */
void cfg_load(void);
const char *cfg_get(const char *key, const char *def);
int cfg_int(const char *key, int def);
void cfg_set(const char *key, const char *val);
void cfg_set_int(const char *key, int v);
int cfg_gen(void);   /* changes whenever a setting changes - for values read every frame */
void cfg_flush(void); /* fsync the settings now (shutdown); cfg_set leaves that to a thread */
void cfg_unset(const char *key); /* the default applies again */
const char *data_dir(void);
/* troubleshooting log (Settings → System, cfg debug_log): more details (every network request), and the logs of
   the user interface and playback are kept in <data>/logs, so they survive a restart. The phone page offers
   them for download with secrets masked (cgi-bin/logs). */
bool debug_log_on(void);
void debug_log_apply(void);      /* after cfg debug_log has changed (and at start) */
const char *mpv_log_path(void);  /* where mpv writes its log for the next playback */
const char *asset_dir(void); /* fonts, icons and language files (lang/) */
void lang_init(void);        /* language from the settings (language), before anything is drawn */
const char *lang_media_default(const char *key); /* sub_lang/audio_lang default based on the UI language */
const char *tr_msg(const char *msg);
bool i18n_load_extra(const char *dir); /* an app package's translations: <dir>/lang/<language>.po */ /* translate a message from a helper script (see util.c) */

/* ---------- Systemkald ---------- */
char *run_capture(const char *cmd); /* malloc'ed output */
char *run_capture_argv(const char *const *argv); /* without a shell (NULL-terminated argv) */
char *log_url(const char *url);  /* URL without tokens/passwords for log files (malloc'ed) */
int run_cmd(const char *fmt, ...);
const char *helper_path(void);

/* ---------- Player ---------- */
/* skip intro / skip credits / next episode (filled in by the source, e.g. Plex markers, once playback has started).
   Times are in mpv's own timeline (seconds). */
typedef struct { double start, end; bool credits; } PlayMarker;
typedef struct {
    PlayMarker m[6];
    int n;
    bool has_next;         /* an episode follows: "Next episode" is offered at the end/credits */
    char next_title[160];
} PlaySkip;
typedef struct {
    const char *url;          /* file/URL or playlist */
    const char *title;
    const char *subtitle;     /* e.g. "Series · S1 E2" or a channel */
    const char *description;  /* shown on the pause screen */
    const char *poster;       /* image URL for the pause screen */
    double start;             /* seconds */
    const char *const *extra; /* extra mpv arguments (NULL-terminated) */
    bool is_live;             /* IPTV: channel change with up/down */
    int playlist_start;
    /* called about every 10 seconds and at the end: pos/dur in seconds */
    void (*progress)(void *ud, double pos, double dur, bool ended, bool paused);
    void (*channel_changed)(void *ud, int idx);
    int playlist_n;                                /* live: number of channels (channel changes are gathered, see zap) */
    void (*channel_preview)(void *ud, int idx);    /* live: show the channel while browsing */
    void (*tick)(void *ud);                        /* called often from the playback loop */
    void (*skip_info)(void *ud, PlaySkip *out);    /* called once shortly after the start (may block briefly) */
    /* settings menu during playback */
    const char *const *qualities; /* NULL-terminated, e.g. {"Original","1080p","720p"} */
    const char *quality;          /* current */
    bool speed;                   /* show speed (YouTube) */
    const char *channel;          /* "Go to channel" (YouTube) */
    bool chat;                    /* Twitch: yellow toggles the chat panel (cfg tw_chat) */
    int (*sub_search)(void *ud, char ***labels);   /* search subtitles: number of results */
    char *(*sub_fetch)(void *ud, int idx);         /* fetch the chosen one: local file path (malloc) */
    void *ud;
    int media_hdr;            /* known by the source: 0 = unknown (ffprobe checks), 1 = SDR, 2 = HDR (PQ/HLG) */
    int media_w;              /* video width if known (0 = unknown) */
} PlayRequest;
typedef struct {
    double pos, dur;
    bool eof;
    int exit_code;
    char quality[32];  /* the user chose a new quality: the caller restarts at pos */
    bool goto_channel; /* the user chose "Go to channel" */
    bool cancelled;    /* cancelled before playback started (Back during "Loading…") */
} PlayResult;
void player_message(const char *msg, const char *arg); /* script-message to mpv's OSD script */
void player_cmd(const char *const *args);   /* mpv command during playback (e.g. {"seek","30","absolute",NULL}) */
bool player_state(double *pos, double *dur, bool *paused); /* false = nothing is playing */
PlayResult player_run(const PlayRequest *rq);
void drm_reset_hdr(void);
bool display_hdr_ok(void);                 /* false if the connected display's EDID has no HDR support */
bool display_known(void);                  /* a display is connected and its EDID could be read */
bool hdr_cfg(const char *key, int def);    /* HDR setting that only counts if the display supports HDR */
typedef enum { DA_AC3, DA_EAC3, DA_DTS, DA_DTSHD, DA_TRUEHD, DA_HD } DisplayAudio; /* HD = TrueHD or DTS-HD */
bool display_audio_ok(DisplayAudio a);     /* false if the display's EDID lists no such audio format */
bool audio_cfg(const char *key, int def, DisplayAudio a); /* passthrough setting that only counts if the display accepts it */
void drm_set_ui_hdr(bool on); /* the TV in HDR10 for the UI (on) or back to SDR (off) */
/* OpenSubtitles */
int subs_search(int tmdb, const char *query, int season, int episode, char ***labels);
char *subs_fetch(int idx);
void player_osd(const char *text, int ms);

/* ---------- App / screens ---------- */
typedef struct Screen Screen;
struct Screen {
    const char *name;
    void (*enter)(Screen *s);
    void (*leave)(Screen *s);
    void (*resume)(Screen *s); /* when the screen comes back on top */
    void (*update)(Screen *s, float dt);
    void (*draw)(Screen *s);
    bool (*input)(Screen *s, const InputEvent *ev);
    void (*destroy)(Screen *s);
    void *st;
    float anim;   /* 0..1 fade-in */
    bool overlay; /* drawn on top of the screen underneath */
};

void app_push(Screen *s);
void app_pop(void);
void app_home(void);
Screen *app_top(void);
void app_close(Screen *s) NOTA_NEW;      /* close s even when it is not on top (e.g. from its own update()) */
void app_replace(Screen *s, Screen *with) NOTA_NEW; /* with takes s's place in the stack (freed if s is gone) */
void app_toast(const char *fmt, ...);
void app_request_redraw(void);
double app_time(void);
bool app_video_suspend(void);
/* moon.c: game streaming via tvmoon (gives up the display meanwhile) */
int moon_run(char *const *argv, void (*on_line)(const char *line, void *ud), void *ud);
void moon_inbox(const char *args);
void app_video_resume(void);
void app_quit(void);
const char *app_ip(void);
float app_boot_time(void);
int app_cfg_gen(void); /* counted up when the configuration is changed from outside (phone) */
void app_cfg_changed(void); /* count it up (e.g. an app saved something that came from the phone) */

/* ---------- Apps ----------
 * Everything on the home screen is an app. An app describes itself with a NotaApp; the system (apps.c) keeps a
 * register of them, and the home screen, Settings → Apps, the App Store and sign-out all work from that register.
 * Settings and the App Store are compiled into tvshell; every other app is a package, a folder in /usr/lib/notatv/apps/<id>/ (manifest.json +
 * app.so + lang/<code>.po) that are loaded at start. app.so exports `const NotaApp nota_app` and may use everything
 * in this header (tvshell exports its symbols). Bump NOTA_API whenever anything an app uses here changes. */
#define NOTA_API 2
#ifndef APP_VERSION
#define APP_VERSION "0" /* an app's version, from its manifest.json (src/apps/Makefile) */
#endif

/* one line in Settings (also used by apps for their own settings) */
typedef enum { IT_INFO, IT_TOGGLE, IT_CYCLE, IT_ACTION, IT_TEXT, IT_SUB } ItemType; /* IT_SUB: opens sub-section number .sub */
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
    int sub;                    /* IT_SUB: index in the section's subs */
    const char *(*locked)(void); /* not NULL = the item cannot be changed, and the text is shown instead of the control;
                                    "" (ITEM_HIDDEN) = the item is not shown at all (e.g. "Sign out" when nobody is signed in) */
} Item;
#define ITEM_HIDDEN "" /* returned by Item.locked: hide the item (older NotaTV shows it empty and locked) */
extern const char *const set_onoff[]; /* {"0","1",NULL} for IT_TOGGLE */
const char *set_hdr_locked(void);     /* "Not supported by the display" when the display has no HDR */
/* "Sign out of <app>?" - confirmed, forget() runs and a toast says so */
void app_ask_signout(const char *app, void (*forget)(void));

typedef struct HomeItem HomeItem;
enum {
    APP_SYSTEM = 1,      /* part of the system: always installed, not in the App Store (Settings, App Store) */
    APP_DEFAULT_OFF = 2, /* not installed from the start */
    APP_ROW_LOGOS = 4,   /* the home row shows logos on a light surface (channel logos) */
};
typedef struct NotaApp {
    int api;                   /* NOTA_API when the app was built */
    const char *id;            /* short, unique: "twitch" (folder name, cfg app_<id>, data folder) */
    const char *name, *desc;   /* English source text (translated with T) */
    const char *version;       /* app version: APP_VERSION (from manifest.json, set by src/apps/Makefile) */
    int order;                 /* place on the home screen (lowest first) */
    unsigned flags;
    IconId icon;
    Color c1, c2, fg;          /* tile gradient and icon color */
    Screen *(*open)(void);
    void (*init)(void);        /* once per run of tvshell, when installed (also after install). Not again after an
                                  uninstall + install in the same run: forget() must leave the app ready to be used
                                  again (threads/mutexes made in init stay) */
    void (*tick)(void);        /* every frame while installed (background services) */
    void (*apply)(void);       /* installed/uninstalled or settings changed: start/stop background services */
    void (*forget)(void);      /* delete everything saved (sign out, uninstall); NULL = nothing saved */
    const Item *settings;      /* Settings → Apps → <name>; NULL = none */
    int n_settings;
    /* the row under the apps on the home screen (all optional) */
    const char *row_title;                         /* "Continue watching" */
    const char *shelf_label;                       /* above the title in the big top: "CONTINUE IN PLEX" */
    int (*row)(HomeItem **items);                  /* the items (the app keeps them; copied by the home screen) */
    void (*row_refresh)(void);                     /* ask for fresh items (async) */
    void (*row_open)(const HomeItem *it, int idx); /* OK on an item */
    bool (*draw_shelf)(float x, float y);          /* own big top instead of icon + description; false = default */
    /* a message from the phone page or a script (/tmp/notatv-inbox/<name>); true = taken */
    bool (*inbox)(const char *name, const char *val);
    const void *iface;         /* functions other parts use, see "Interfaces" below (NULL = none) */
} NotaApp;

/* Interfaces: what an app offers the system or other apps through NotaApp.iface. They are reached with
   app_iface("<id>"), which gives NULL when the app is not installed - so callers simply skip the feature. */
typedef struct { /* "music": Bluetooth playback info from bt.c for "Now playing" */
    void (*bt_state)(bool active, bool playing, const char *sender, bool new_session);
    void (*bt_track)(const char *title, const char *artist, const char *album, double dur);
    void (*bt_pos)(double pos);
} MusicIface;
typedef struct { /* "seerr": IMDb/Rotten Tomatoes ratings (Plex uses them when its server has none) */
    void (*ratings)(bool tv, int tmdb, void (*cb)(void *ud, int rt_critic, int rt_audience, int imdb10), void *ud);
} SeerrIface;
const void *app_iface(const char *id);

int app_count(void);
const NotaApp *app_at(int i);            /* sorted by order */
const NotaApp *app_find(const char *id);
bool app_installed(const NotaApp *a);
bool app_on(const char *id);             /* installed (false for unknown ids) */
bool app_packaged(const NotaApp *a);     /* loaded from a package (not compiled into tvshell) */
const char *app_pkg_dir(const NotaApp *a); /* the package folder ("" for built-in apps) */
const char *app_version(const NotaApp *a);      /* the version that runs (from manifest.json) */
const char *app_next_version(const NotaApp *a); /* a newer version downloaded, used after a restart ("" = none) */
void app_set_next_version(const NotaApp *a, const char *ver);
const char *app_data(const NotaApp *a);  /* the app's own data folder, created when asked for */
void app_install(const NotaApp *a);
void app_uninstall(const NotaApp *a);    /* forget() + the data folder is wiped; background services stop */
void apps_init(void);
const char *apps_pkg_root(void);          /* <data>/apps-pkg: the packages downloaded from the App Store */
const NotaApp *apps_load_pkg(const char *id); /* load <apps-pkg>/<id> right after it was downloaded */

/* App Store catalog (appcat.c): the apps that can be downloaded from GitHub, and installing them */
typedef struct {
    char id[40], version[24], file[128], sha[72], name[96], desc[300];
    IconId icon;
    Color c1, c2, fg;
    int order;
    bool def;  /* was on by default in NotaTV 1.0 (used when moving such boxes to App Store apps) */
    long size; /* bytes */
    /* what is new in this version (manifest "changes", translated): one short line and a few points */
    char news[200], points[4][160];
    int n_points;
} CatApp;
typedef enum { CAT_NONE, CAT_LOADING, CAT_OK, CAT_ERROR } CatState;
CatState appcat_state(void);
const char *appcat_error(void);
void appcat_refresh(void);                 /* fetch the catalog (in the background) */
int appcat_count(void);
const CatApp *appcat_at(int i);
const CatApp *appcat_find(const char *id);
bool appcat_has_update(const char *id);    /* installed, and the catalog has a newer version (not downloaded yet) */
int appcat_updates(void);                  /* how many installed apps have an update */
void appcat_update_all(void);              /* queue every update */
void appcat_install(const char *id);       /* download + load + install (queued, one at a time) */
int appcat_pending(const char *id);        /* 0 = not queued, 1 = waiting, 2 = being installed */
bool appcat_busy(int *done, int *total, const char **current); /* installs left; progress of this round */
bool appcat_restart_needed(void);          /* an app was updated while loaded: the new version needs a restart */
void appcat_tick(void);
IconId icon_by_name(const char *name);
int version_cmp(const char *a, const char *b);
void apps_tick(void);
int apps_gen(void);                      /* changes when apps are loaded, installed or uninstalled */
bool apps_inbox(const char *name, const char *val); /* gives an inbox message to the app that takes it */
Screen *appstore_screen(void);
/* what is new in a system update (the summary at the top of its release text, update_notice.c) */
#define UPDATE_NEWS_MAX 5
typedef struct { char summary[300], points[UPDATE_NEWS_MAX][200]; int n; } UpdateNews;
bool update_news(const char *ver, UpdateNews *n); /* false = the release says nothing short (or is not known) */

/* shared UI helpers */
float anim_to(float cur, float target, float dt, float speed);
void draw_background(void);
void draw_topbar(const char *title);
void draw_hint(float x, float y, const char *keys, const char *label);
/* the color keys' actions as buttons at the bottom (↓ from the bottom row gives focus; the screen's input must
   return false for A_DOWN when there is nothing more below). Green=A_INFO, Yellow=A_YELLOW, Blue=A_SEARCH, Red=A_MENU */
float draw_keybar(float x, float y, const char *ok, const char *green, const char *yellow, const char *blue, const char *red);
bool keybar_focused(void); /* the key bar has focus (the screen can dim its own focus) */
void draw_spinner(float cx, float cy, float r, Color c);
void draw_empty(const char *title, const char *msg);
/* one frame while the main thread waits for something (e.g. ffprobe): draws the screen + spinner; true = Back pressed */
bool app_wait_frame(const char *msg);
/* button row on login/setup screens (centered on cx); sel = the button in focus, -1 = none */
void draw_buttons(float cx, float y, const char *const *labels, int n, int sel);
/* left/right between the buttons; false when left is pressed on the first button (then the side menu can open) */
bool buttons_nav(int *sel, int n, const InputEvent *ev);
char *fmt_duration(double secs, char *buf, size_t n);
char *str_dup(const char *s);
char *file_read_all(const char *path, long max); /* whole file, 0-terminated; NULL if missing or > max */
char *str_printf(const char *fmt, ...);
void str_trim(char *s);

/* Dialogs / keyboard */
typedef void (*KbDone)(void *ud, const char *text); /* text NULL = cancelled */
void keyboard_open(const char *title, const char *initial, bool password, KbDone cb, void *ud);
void keyboard_inject(const char *text);
typedef void (*ConfirmDone)(void *ud, bool yes);
void confirm_open(const char *title, const char *msg, const char *yes, const char *no,
                  ConfirmDone cb, void *ud);
/* "what's new": a small heading, a title, one short summary and up to 8 points; no = "" gives one button */
void notes_open(const char *kicker, const char *title, const char *summary, const char *const *points, int n,
                const char *yes, const char *no, ConfirmDone cb, void *ud);
typedef void (*ChoiceDone)(void *ud, int idx); /* -1 = cancelled */
void choice_open(const char *title, const char *const *items, int n, int sel, ChoiceDone cb, void *ud);

/* "Next episode" countdown overlay (auto-advance). play=true: the user pressed OK or the countdown ran out;
   play=false: the user pressed Back. */
typedef void (*NextUpDone)(void *ud, bool play);
void nextup_open(const char *poster, const char *title, const char *subtitle, int seconds, NextUpDone cb, void *ud);

/* Side menu: a narrow icon bar that slides out when you press left from the content */
#define SIDE_W 112      /* bredde lukket */
#define SIDE_OPEN_W 440 /* width when open */
typedef struct { char label[128]; IconId icon; bool gap_before; } SideItem;
typedef struct {
    char title[64], subtitle[128];
    IconId logo;
    Color accent;
    SideItem items[40];
    int n, sel, active;
    bool open;
    float t, scroll;
} SideMenu;
void side_init(SideMenu *m, const char *title, IconId logo, Color accent);
void side_clear(SideMenu *m);
int side_add(SideMenu *m, const char *label, IconId icon, bool gap_before);
void side_update(SideMenu *m, float dt);
void side_draw(SideMenu *m);
/* when the menu is open: returns true if the event was used; *activated = chosen item (otherwise -1) */
bool side_input(SideMenu *m, const InputEvent *ev, int *activated);
void side_open(SideMenu *m);

/* IMDb / Rotten Tomatoes badges; -1 = unknown. Returns x after the last badge */
float draw_ratings(float x, float y, int rt_critic, int rt_audience, int imdb10);

/* QR code for the screen (e.g. a phone link) */
void draw_qr(const char *text, float x, float y, float size);

/* Screens of the system (the apps' own screens are in their packages, src/apps) */
Screen *home_screen(void);
Screen *settings_screen(void);
void settings_wifi(void);         /* search for and choose Wi-Fi (from Settings) */
void settings_apply_name(void);   /* device name changed: restart AirPlay/CEC/BT/DLNA */
void update_install_open(const char *ver); /* update_notice.c: install the GitHub update with a progress bar, then offer a restart */
void app_background_tick(bool playing); /* main.c: jobs, inbox, Bluetooth, installs (+ services when not playing) */
void update_tick(void);           /* update_notice.c: looks for updates on GitHub and reminds every other day */
Screen *setup_screen(void);       /* setup guide (first boot) */
const char *phone_pin(void);      /* code for the phone page (created if missing) */
void phone_pin_new(void);
void phone_pin_change(void (*done)(void)); /* new random one / choose your own */
Screen *netwatch_screen(void); /* Settings → Network: who the box talks to */
/* Bluetooth (bt.c): agent for pairing (the code is shown on the TV), playback info and control via D-Bus/BlueZ */
void bt_init(void);
void bt_tick(void);
void bt_apply(void);              /* bt_enabled / device name changed */
void bt_wake(double secs);        /* turn the radio on for at least secs seconds (only when in use, cfg bt_ondemand) */
void bt_media(const char *cmd);   /* "Play", "Pause", "Next", "Previous" to the phone */
Screen *bt_pair_screen(void);
/* DLNA/UPnP receiver (dlna.c): apps can send video/music/pictures to the TV */
void dlna_tick(void);
void dlna_apply(void);
/* Time zone (tz.c): country/region is chosen manually; tz_dst = follow summer/winter time */
void tz_init(void);
void tz_apply(void);
const char *tz_label(void);
const char *tz_country_name(const char *code);
bool tz_zone_has_dst(const char *zone);
void tz_pick(void (*done)(void *ud, bool ok), void *ud); /* country -> region; saves and applies */

/* Find servers on the local network (netdisc.c): Plex (GDM 32414 + port 32400), Jellyfin (UDP 7359 + port 8096).
   Runs in the background; cb is called on the main thread. */
typedef enum { DISC_PLEX, DISC_JELLYFIN } DiscKind;
typedef struct { char name[128], url[160], id[96]; } DiscServer;
typedef void (*DiscDone)(void *ud, const DiscServer *s, int n);
void netdisc_start(DiscKind kind, DiscDone cb, void *ud);

/* screen mirroring (mirror.c): AirPlay via UxPlay + Android (own app, MPEG-TS on port 7400) */
void mirror_tick(void);
void mirror_apply(void);
const char *airplay_pin(void); /* the code a phone enters the first time it mirrors (made once per box) */
typedef struct { char path[128], name[128]; bool connected, input; } BtDevice; /* input: controller/keyboard (Icon input-*) */
int bt_devices(BtDevice *out, int max); /* paired devices (blocking, short timeout) */
void bt_connect(const char *path);      /* the box calls the phone: the music moves to the TV */
void bt_disconnect(void);               /* disconnect the phone that is playing */
void bt_forget_all(void);
void power_menu_open(void);

/* Shared data for the home screen */
struct HomeItem { char *title, *subtitle, *image, *key; double progress; char *logo; /* title logo (PNG) */ };
void wipe_file(const char *path);        /* overwrite (small files) and delete */
void wipe_dir(const char *dir);          /* everything in a folder */
void wipe_session_traces(void);          /* /tmp leftovers: mpv log, image cache */
void subs_forget(void);

#endif
