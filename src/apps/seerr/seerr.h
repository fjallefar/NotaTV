/* NotaTV app package "seerr" (Seerr) - shared between the package's own files */
#ifndef SEERR_H
#define SEERR_H
#include "tv.h"

Screen *seerr_screen(void);
/* ratings via Seerr (used by Plex when the server does not have IMDb/RT itself) */
void seerr_ratings(bool tv, int tmdb, void (*cb)(void *ud, int rt_critic, int rt_audience, int imdb10), void *ud);
void seerr_forget(void);

#endif
