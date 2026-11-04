/* NotaTV app package "youtube" (YouTube) - shared between the package's own files */
#ifndef YOUTUBE_H
#define YOUTUBE_H
#include "tv.h"

Screen *youtube_screen(void);
/* YouTube cast (ytcast.c): DIAL + YouTube lounge ("Cast" and "Link with TV code" in the YouTube app) */
void ytcast_tick(void);
void ytcast_apply(void);
void ytcast_forget(void);
void ytcast_play_tick(void);
char *ytcast_pairing_code(void);                        /* blocking; "123 456 789 012" or NULL */
bool youtube_cast_play(const char *id, double start);   /* youtube.c; true = played to the end */
int youtube_recent(HomeItem **items); /* recently watched videos */
void youtube_play_recent(int idx);
void youtube_forget_all(void);

#endif
