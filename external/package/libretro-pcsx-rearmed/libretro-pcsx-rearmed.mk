################################################################################
#
# libretro-pcsx-rearmed - PlayStation core for the app package "retro" (staging only)
#
################################################################################

LIBRETRO_PCSX_REARMED_VERSION = 0f544ce1d18ac7dd114cb8f5507f01a29d3cd58c
LIBRETRO_PCSX_REARMED_SITE = $(call github,libretro,pcsx_rearmed,$(LIBRETRO_PCSX_REARMED_VERSION))
LIBRETRO_PCSX_REARMED_LICENSE = GPL-2.0
LIBRETRO_PCSX_REARMED_LICENSE_FILES = COPYING
LIBRETRO_PCSX_REARMED_INSTALL_STAGING = YES
LIBRETRO_PCSX_REARMED_INSTALL_TARGET = NO

define LIBRETRO_PCSX_REARMED_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro platform=unix ARCH=arm64 DYNAREC=ari64 \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" AR="$(TARGET_AR)" AS="$(TARGET_AS)"
endef

define LIBRETRO_PCSX_REARMED_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/pcsx_rearmed_libretro.so $(STAGING_DIR)/usr/lib/notatv-retro/cores/pcsx_rearmed_libretro.so
	$(TARGET_STRIP) $(STAGING_DIR)/usr/lib/notatv-retro/cores/pcsx_rearmed_libretro.so
endef

$(eval $(generic-package))
