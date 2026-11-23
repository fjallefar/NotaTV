################################################################################
#
# retroarch - only for the app package "retro" (src/apps/retro/app.mk copies it from staging)
#
################################################################################

RETROARCH_VERSION = 1.22.2
RETROARCH_SITE = $(call github,libretro,RetroArch,v$(RETROARCH_VERSION))
RETROARCH_LICENSE = GPL-3.0+
RETROARCH_LICENSE_FILES = COPYING
RETROARCH_DEPENDENCIES = host-pkgconf alsa-lib libdrm libegl libgles mesa3d udev zlib
RETROARCH_INSTALL_STAGING = YES
RETROARCH_INSTALL_TARGET = NO

# a TV box: KMS + EGL + GLES, ALSA, udev input. No X11/Wayland, no network features (netplay, online updater,
# achievements), no Qt/SDL/FFmpeg - the app starts it with its own config file and a core
define RETROARCH_CONFIGURE_CMDS
	cd $(@D) && $(TARGET_CONFIGURE_OPTS) CROSS_COMPILE="$(TARGET_CROSS)" \
		PKG_CONF_PATH="$(PKG_CONFIG_HOST_BINARY)" \
		./configure --prefix=/usr \
		--disable-x11 --disable-wayland --disable-xvideo --disable-xinerama --disable-xrandr \
		--enable-kms --enable-egl --enable-opengles --enable-opengles3 --disable-opengl --disable-opengl1 \
		--disable-vulkan --disable-sdl --disable-sdl2 --disable-qt --disable-ffmpeg --disable-mpv \
		--enable-alsa --disable-pulse --disable-jack --disable-oss --disable-pipewire --disable-roar \
		--enable-udev --disable-networking --disable-netplaydiscovery --disable-cheevos --disable-discord \
		--disable-online_updater --disable-update_cores --disable-update_assets --disable-ssl --disable-builtinmbedtls \
		--disable-videocore --disable-caca --disable-sixel --disable-v4l2 --disable-microphone --disable-cdrom
endef

define RETROARCH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" HAVE_LAKKA=0
endef

define RETROARCH_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0755 $(@D)/retroarch $(STAGING_DIR)/usr/lib/notatv-retro/retroarch
	$(TARGET_STRIP) $(STAGING_DIR)/usr/lib/notatv-retro/retroarch
endef

$(eval $(generic-package))
