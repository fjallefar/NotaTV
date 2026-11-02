# NotaTV app packages: every folder here with a manifest.json is an app.
# Each one is built into $(OUT)/<id>/ = manifest.json + app.so + lang/ (the package folder that tvshell loads,
# see src/shell/apps.c). Apps use tv.h from src/shell and call into tvshell (it exports its symbols).
# An app can add compiler flags in <id>/app.mk as CFLAGS_<id>.
# The app's version is the "version" in its manifest.json; the code gets it as APP_VERSION (one place to change it).
PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2 -g
SHELL_DIR ?= $(CURDIR)/../shell
SECRETS ?= $(CURDIR)/../../secrets
OUT ?= $(CURDIR)/out
override CFLAGS += -std=gnu11 -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation
override CFLAGS += -I$(SHELL_DIR) $(shell $(PKG_CONFIG) --cflags sdl3 libcurl)
# the NotaTV version (some apps send it to servers, e.g. Jellyfin)
VERSION_FILE ?= $(CURDIR)/../../VERSION
TV_VER := $(shell cat $(VERSION_FILE) 2>/dev/null)
override CFLAGS += -DTV_VERSION=\"$(if $(TV_VER),$(TV_VER),dev)\"

APPS := $(patsubst %/manifest.json,%,$(wildcard */manifest.json))
-include $(wildcard */app.mk)

all: $(foreach a,$(APPS),$(OUT)/$(a)/app.so)

.SECONDEXPANSION:
$(OUT)/%/app.so: $$(wildcard %/*.c) $$(wildcard %/*.h) $(SHELL_DIR)/tv.h $(SHELL_DIR)/i18n.h %/manifest.json $$(wildcard %/lang/*.po) $$(wildcard %/app.mk)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CFLAGS_$*) -DAPP_VERSION="\"$$(sed -n 's/^ *"version": *"\([0-9.]*\)".*/\1/p' $*/manifest.json)\"" \
		-shared -o $@ $(filter %.c,$^) $(LDFLAGS)
	cp $*/manifest.json $(@D)/manifest.json
	rm -rf $(@D)/lang
	if [ -d $*/lang ]; then cp -r $*/lang $(@D)/lang; fi

# install every package into $(DESTDIR)/usr/lib/notatv/apps/<id>/
install: all
	for a in $(APPS); do \
		mkdir -p $(DESTDIR)/usr/lib/notatv/apps/$$a; \
		cp -a $(OUT)/$$a/. $(DESTDIR)/usr/lib/notatv/apps/$$a/; \
	done

clean:
	rm -rf $(OUT)

.PHONY: all install clean
