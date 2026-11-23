################################################################################
#
# libretro-fceumm - NES core for the app package "retro" (staging only)
#
################################################################################

LIBRETRO_FCEUMM_VERSION = fb919fb6fa881aaa6279b765d480220579e5c606
LIBRETRO_FCEUMM_SITE = $(call github,libretro,libretro-fceumm,$(LIBRETRO_FCEUMM_VERSION))
LIBRETRO_FCEUMM_LICENSE = GPL-2.0+
LIBRETRO_FCEUMM_LICENSE_FILES = Copying
LIBRETRO_FCEUMM_INSTALL_STAGING = YES
LIBRETRO_FCEUMM_INSTALL_TARGET = NO

define LIBRETRO_FCEUMM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro platform=unix \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" AR="$(TARGET_AR)"
endef

define LIBRETRO_FCEUMM_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/fceumm_libretro.so $(STAGING_DIR)/usr/lib/notatv-retro/cores/fceumm_libretro.so
	$(TARGET_STRIP) $(STAGING_DIR)/usr/lib/notatv-retro/cores/fceumm_libretro.so
endef

$(eval $(generic-package))
