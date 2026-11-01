################################################################################
#
# rpi-tools
#
################################################################################

RPI_TOOLS_VERSION = 1.0
RPI_TOOLS_SITE = $(BR2_EXTERNAL_NOTATV_PATH)/board/rpi4/tools
RPI_TOOLS_SITE_METHOD = local
RPI_TOOLS_LICENSE = MIT

define RPI_TOOLS_BUILD_CMDS
	$(MAKE) -C $(@D) clean
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define RPI_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tvmon $(TARGET_DIR)/usr/bin/tvmon
	$(INSTALL) -D -m 0755 $(@D)/tryboot $(TARGET_DIR)/usr/sbin/tryboot
endef

$(eval $(generic-package))
