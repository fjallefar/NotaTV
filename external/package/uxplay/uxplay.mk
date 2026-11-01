################################################################################
#
# uxplay
#
################################################################################

UXPLAY_VERSION = 1.73.7
UXPLAY_SITE = $(call github,FDH2,UxPlay,v$(UXPLAY_VERSION))
UXPLAY_LICENSE = GPL-3.0+
UXPLAY_LICENSE_FILES = LICENSE
UXPLAY_DEPENDENCIES = host-pkgconf dbus openssl libplist avahi gstreamer1 gst1-plugins-base
# no X11, no -march=native
UXPLAY_CONF_OPTS = -DNO_X11_DEPS=ON -DNO_MARCH_NATIVE=ON

$(eval $(cmake-package))
