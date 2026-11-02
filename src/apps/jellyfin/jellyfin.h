/* NotaTV app package "jellyfin" (Jellyfin) - shared between the package's own files */
#ifndef JELLYFIN_H
#define JELLYFIN_H
#include "tv.h"

Screen *jellyfin_screen(void);
void jellyfin_remote_tick(void); /* "Play on → NotaTV" from the Jellyfin app (WebSocket to the server) */
void jellyfin_forget(void);

#endif
