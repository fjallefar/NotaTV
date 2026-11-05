/* NotaTV app package "seerr" (Seerr) - shared between the package's own files */
#ifndef SEERR_H
#define SEERR_H
#include "tv.h"

Screen *seerr_screen(void);
/* ratings via Seerr (used by Plex when the server does not have IMDb/RT itself) */
void seerr_ratings(bool tv, int tmdb, void (*cb)(void *ud, int rt_critic, int rt_audience, int imdb10), void *ud);
void seerr_forget(void);

/* find Seerr/Overseerr/Jellyseerr on the local network (disc.c); cb is called on the main thread */
#define SEERR_MAX_FOUND 8
typedef struct { char name[96], url[64]; } SeerrFound;
typedef void (*SeerrDiscDone)(void *ud, const SeerrFound *f, int n);
void seerr_discover(SeerrDiscDone cb, void *ud);

#endif
