################################################################################
#
# tvshell
#
################################################################################

TVSHELL_VERSION = 1.0
TVSHELL_SITE = $(BR2_EXTERNAL_NOTATV_PATH)/../src/shell
TVSHELL_SITE_METHOD = local
TVSHELL_LICENSE = MIT
TVSHELL_DEPENDENCIES = sdl3 libcurl libdrm zlib dbus libupnp host-pkgconf

define TVSHELL_BUILD_CMDS
	$(MAKE) -C $(@D) clean
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		SECRETS="$(BR2_EXTERNAL_NOTATV_PATH)/../secrets" \
		VERSION_FILE="$(BR2_EXTERNAL_NOTATV_PATH)/../VERSION"
	# app packages (src/apps): built against this tvshell's tv.h into the build folder. They are NOT put in the
	# image - the release workflow packs them for the App Store (scripts/apps-catalog.py)
	rm -rf $(@D)/apps-out
	$(TARGET_MAKE_ENV) $(MAKE) -C $(BR2_EXTERNAL_NOTATV_PATH)/../src/apps \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		SHELL_DIR="$(@D)" \
		SECRETS="$(BR2_EXTERNAL_NOTATV_PATH)/../secrets" \
		OUT="$(@D)/apps-out"
endef

define TVSHELL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tvshell $(TARGET_DIR)/usr/bin/tvshell
	$(INSTALL) -D -m 0755 $(@D)/notasplash $(TARGET_DIR)/usr/bin/notasplash
	mkdir -p $(TARGET_DIR)/usr/share/tvshell
	cp -a $(@D)/assets/. $(TARGET_DIR)/usr/share/tvshell/
	rm -rf $(TARGET_DIR)/usr/lib/notatv/apps
endef

$(eval $(generic-package))
