################################################################################
#
# tvmoon
#
################################################################################

TVMOON_VERSION = 1.0
TVMOON_SITE = $(BR2_EXTERNAL_NOTATV_PATH)/../src/moon
TVMOON_SITE_METHOD = local
TVMOON_LICENSE = GPL-3.0+
TVMOON_DEPENDENCIES = libgamestream ffmpeg libdrm alsa-lib opus avahi libcurl openssl expat util-linux host-pkgconf

define TVMOON_BUILD_CMDS
	$(MAKE) -C $(@D) clean
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)"
endef

define TVMOON_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tvmoon $(TARGET_DIR)/usr/bin/tvmoon
endef

$(eval $(generic-package))
