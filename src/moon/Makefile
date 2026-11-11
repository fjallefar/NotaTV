# tvmoon - game streaming (the Moonlight protocol)
PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
# libgamestream has no .pc file; "=" = inside the toolchain's sysroot. Override for another layout
GAMESTREAM_INC ?= =/usr/include/gamestream
override CFLAGS += -I$(GAMESTREAM_INC) $(shell $(PKG_CONFIG) --cflags libavcodec libavformat libavutil libdrm alsa opus avahi-client libcurl openssl expat uuid)
LIBS = -lgamestream $(shell $(PKG_CONFIG) --libs libavcodec libavformat libavutil libdrm alsa opus avahi-client libcurl openssl expat uuid) -lpthread -lm

SRCS = main.c i18n.c video.c overlay.c videotest.c audio.c input.c
OBJS = $(SRCS:.c=.o)

tvmoon: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c tvmoon.h i18n.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f tvmoon $(OBJS)

.PHONY: clean
