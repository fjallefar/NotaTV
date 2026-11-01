/* tvmoon - NotaTV' game streaming client (the Moonlight protocol against Sunshine/GeForce Experience).
 *
 * Its own process, started by tvshell (like mpv): a crash here does not take the user interface with it.
 * The protocol is moonlight-common-c, pairing and the app list are libgamestream (moonlight-embedded);
 * video, audio and input are our own: v4l2 decoder -> DRM PRIME -> the display plane, Opus -> ALSA, evdev.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "i18n.h"
#include <gamestream/Limelight.h>

#define TVMOON_DIR "/data/notatv/moonlight"

/* log to stderr (tvshell sends it to /tmp/tvmoon.log) */
void tlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* line for tvshell on stdout (status, errors) - always flushed */
void tout(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
double now_s(void);

/* video.c */
extern DECODER_RENDERER_CALLBACKS video_callbacks;
typedef struct {
    const char *mode; /* "WxH" or NULL: keep the display's mode */
} VideoOpts;
void video_configure(const VideoOpts *o);
void video_set_hdr(bool on);
typedef struct {
    int rx, shown, skip, err, w, h, hz, disp_h;
    double dec_ms, total_ms, max_ms, host_ms, mbit;
    bool hevc, hdr;
} VStat;
void video_stats(char *out, size_t n, VStat *st); /* this second's numbers; reset */

/* overlay.c: stats on top of the video (Green) */
void overlay_init(int fd, uint32_t crtc, int crtc_idx, int mode_w, int mode_h);
void overlay_set_text(const char *const *lines, int n, int highlight, bool hdr); /* highlight: highlighted line, -1 = none */
void overlay_show(bool on);
bool overlay_visible(void);
bool overlay_dirty(void);
void overlay_commit(void *atomic_req);
void overlay_quit(void);
extern volatile bool stats_toggle; /* set by input (Green) */

/* audio.c */
extern AUDIO_RENDERER_CALLBACKS audio_callbacks;
void audio_stats(char *out, size_t n);

/* input.c */
bool input_start(void);   /* evdev thread: keyboard, mouse, controllers, CEC */
void input_stop(void);
extern volatile bool quit_requested; /* quit combination pressed */
/* Red = menu (like the rest of NotaTV): Resume / Stats / Quit. The remote's arrows/OK/Back control
 * the menu while it is open, and are not sent to the game */
#define MENU_N 3
extern volatile bool menu_open, menu_dirty;
extern volatile int menu_sel, menu_pick; /* menu_pick: chosen item (-1 = none) */
