/* NotaTV app package "flimmer" (Flimmer) - shared between the package's own files */
#ifndef FLIMMER_H
#define FLIMMER_H
#include "tv.h"

Screen *flimmer_screen(void);
void flimmer_forget(void);

/* find Flimmertv on the local network (disc.c); cb is called on the main thread */
#define FL_MAX_FOUND 8
typedef struct { char name[96], url[64]; } FlFound;
typedef void (*FlDiscDone)(void *ud, const FlFound *f, int n);
void flimmer_discover(FlDiscDone cb, void *ud);

#endif
