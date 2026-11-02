/* NotaTV app package "plex" (Plex) - shared between the package's own files */
#ifndef PLEX_H
#define PLEX_H
#include "tv.h"

Screen *plex_screen(void);
/* Plex cast (plexcast.c): the box as a Plex player (GDM + Companion on port 3005) */
typedef struct { char key[256], server_id[96], address[64], protocol[8], token[300], container[256]; int port; double offset; } PlexCastMedia;
typedef struct { char key[256], rating_key[32], container[256], pq_id[24], pq_item[24], server_id[96], address[64], protocol[8]; int port; double duration; } PlexCastNow;
void plexcast_tick(void);
bool plex_token_is_ours(const char *token); /* the token belongs to this box's Plex account or its Plex Home */
void plexcast_apply(void);
void plexcast_play_tick(void);                          /* from the Plex playback's rq.tick */
void plexcast_playing(const PlexCastNow *now, double base);
void plexcast_stopped(void);
int plexcast_take_skip(void);                           /* +1/-1 = the app asked for next/previous */
bool plexcast_play_pending(void);                       /* the app has sent new content */
void plex_cast_play(const PlexCastMedia *m);            /* plex.c */
const char *plex_client_id(void);
int plex_ondeck(HomeItem **items); /* returns the count (cache) */
void plex_refresh_ondeck(void);
void plex_play_key(const char *key);
void plex_forget(void);                  /* sign out: delete everything saved for the account */

#endif
