# NotaTV shell
PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation
override CFLAGS += $(shell $(PKG_CONFIG) --cflags sdl3 libcurl libdrm egl glesv2 gbm dbus-1 libupnp)
# Twitch app (dev.twitch.tv): the Client ID is read from the private file ../../secrets/twitch-client-id (not in git)
SECRETS ?= $(CURDIR)/../../secrets
TW_ID := $(shell cat $(SECRETS)/twitch-client-id 2>/dev/null)
override CFLAGS += -DTW_CLIENT_ID=\"$(TW_ID)\"
# version shown on the home screen etc.: from the VERSION file in the repo root
VERSION_FILE ?= $(CURDIR)/../../VERSION
TV_VER := $(shell cat $(VERSION_FILE) 2>/dev/null)
override CFLAGS += -DTV_VERSION=\"$(if $(TV_VER),$(TV_VER),dev)\"
LIBS = $(shell $(PKG_CONFIG) --libs sdl3 libcurl libdrm egl glesv2 gbm dbus-1 libupnp) -lz -lm -lpthread

SRCS = main.c i18n.c drm_util.c kms_present.c subs.c gfx.c input.c util.c player.c widgets.c ui_home.c ui_iptv.c ui_plex.c \
       ui_youtube.c ui_music.c ui_net.c bt.c dlna.c plexcast.c ytcast.c mirror.c moon.c ui_games.c ui_media.c ui_settings.c ui_seerr.c ui_jellyfin.c ui_twitch.c netdisc.c tz.c ui_setup.c stb_impl.c vendor/cJSON.c vendor/qrcodegen.c
OBJS = $(SRCS:.c=.o)

tvshell: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c tv.h i18n.h
	$(CC) $(CFLAGS) -c -o $@ $<

stb_impl.o: override CFLAGS += -Wno-unused-function -Wno-sign-compare
vendor/cJSON.o vendor/qrcodegen.o: override CFLAGS += -Wno-all -Wno-extra

clean:
	rm -f tvshell $(OBJS)

.PHONY: clean
