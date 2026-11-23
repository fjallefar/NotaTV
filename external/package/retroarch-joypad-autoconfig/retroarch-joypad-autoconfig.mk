################################################################################
#
# retroarch-joypad-autoconfig - controller profiles for the app package "retro" (staging only)
#
################################################################################

RETROARCH_JOYPAD_AUTOCONFIG_VERSION = 0a675f2e98bd43d2eff088a2dd580797d3a8e89f
RETROARCH_JOYPAD_AUTOCONFIG_SITE = $(call github,libretro,retroarch-joypad-autoconfig,$(RETROARCH_JOYPAD_AUTOCONFIG_VERSION))
RETROARCH_JOYPAD_AUTOCONFIG_LICENSE = MIT
RETROARCH_JOYPAD_AUTOCONFIG_LICENSE_FILES = COPYING
RETROARCH_JOYPAD_AUTOCONFIG_INSTALL_STAGING = YES
RETROARCH_JOYPAD_AUTOCONFIG_INSTALL_TARGET = NO

define RETROARCH_JOYPAD_AUTOCONFIG_INSTALL_STAGING_CMDS
	rm -rf $(STAGING_DIR)/usr/lib/notatv-retro/autoconfig
	mkdir -p $(STAGING_DIR)/usr/lib/notatv-retro/autoconfig
	cp -a $(@D)/udev $(STAGING_DIR)/usr/lib/notatv-retro/autoconfig/
	$(INSTALL) -m 0644 $(@D)/COPYING $(STAGING_DIR)/usr/lib/notatv-retro/autoconfig/COPYING
endef

$(eval $(generic-package))
