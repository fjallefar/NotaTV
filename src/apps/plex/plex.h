/* NotaTV app package "plex" (Plex) - shared between the package's own files */
#ifndef PLEX_H
#define PLEX_H
#include "tv.h"

Screen *plex_screen(void);
const char *plex_client_id(void);
int plex_ondeck(HomeItem **items); /* returns the count (cache) */
void plex_refresh_ondeck(void);
void plex_play_key(const char *key);
void plex_forget(void);                  /* sign out: delete everything saved for the account */

#endif
