/* NotaTV shell - the sections of Settings (ui_settings.c sections[]), for opening Settings on one of them */
#pragma once
typedef enum {
    SET_HOME, SET_NET, SET_PICTURE, SET_SOUND, SET_LANG, SET_DEVICES, SET_CAST, SET_APPS, SET_SYSTEM, SET_ADVANCED, SET_N
} SetSec;
Screen *settings_screen_at(int sec); /* Settings with the list of section sec in focus */
