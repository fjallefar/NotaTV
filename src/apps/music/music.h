/* NotaTV app package "music" (Music) - shared between the package's own files */
#ifndef MUSIC_H
#define MUSIC_H
#include "tv.h"

Screen *music_screen(void);
void music_init(void);
void music_tick(void); /* new AirPlay playback -> show "Now playing" */
bool music_now(char **title, char **artist, char **cover, bool *playing);
void music_bt_state(bool active, bool playing, const char *sender, bool new_session);
void music_bt_track(const char *title, const char *artist, const char *album, double dur);
void music_bt_pos(double pos);

#endif
