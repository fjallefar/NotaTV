################################################################################
#
# libretro-mgba - Game Boy / GBA core for the app package "retro" (staging only)
#
################################################################################

LIBRETRO_MGBA_VERSION = 7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6
LIBRETRO_MGBA_SITE = $(call github,libretro,mgba,$(LIBRETRO_MGBA_VERSION))
LIBRETRO_MGBA_LICENSE = MPL-2.0
LIBRETRO_MGBA_LICENSE_FILES = LICENSE
LIBRETRO_MGBA_INSTALL_STAGING = YES
LIBRETRO_MGBA_INSTALL_TARGET = NO
LIBRETRO_MGBA_SUPPORTS_IN_SOURCE_BUILD = NO
# only the libretro core: no frontends, no optional libraries (the core is self-contained)
LIBRETRO_MGBA_CONF_OPTS = -DBUILD_LIBRETRO=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_GL=OFF -DBUILD_GLES2=OFF \
	-DBUILD_GLES3=OFF -DBUILD_SHARED=OFF -DBUILD_STATIC=OFF -DSKIP_LIBRARY=ON -DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF \
	-DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_LZMA=OFF -DUSE_EDITLINE=OFF \
	-DUSE_DISCORD_RPC=OFF -DUSE_EPOXY=OFF -DUSE_LUA=OFF -DUSE_JSON_C=OFF -DENABLE_SCRIPTING=OFF -DM_CORE_DS=OFF

define LIBRETRO_MGBA_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/buildroot-build/mgba_libretro.so $(STAGING_DIR)/usr/lib/notatv-retro/cores/mgba_libretro.so
	$(TARGET_STRIP) $(STAGING_DIR)/usr/lib/notatv-retro/cores/mgba_libretro.so
endef

$(eval $(cmake-package))
