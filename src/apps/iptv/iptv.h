/* NotaTV app package "iptv" (Live TV) - shared between the package's own files */
#ifndef IPTV_H
#define IPTV_H
#include "tv.h"

Screen *iptv_screen(void);
int iptv_recent(HomeItem **items);
void iptv_play_recent(int idx);
void iptv_forget_all(void);
void iptv_xtream_login(void);   /* add (and use) an Xtream account */
const char *iptv_account_line(void); /* settings: the account in use and when it ends */
void iptv_account_switch(void);
void iptv_account_signout(void);

#endif
