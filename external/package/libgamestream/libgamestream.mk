################################################################################
#
# libgamestream (moonlight-embedded/libgamestream + moonlight-common-c)
#
################################################################################

# moonlight-embedded with moonlight-common-c as a git submodule (b126e48)
LIBGAMESTREAM_VERSION = f32e415aea6797d261d6b470dcf8bf18727341c2
LIBGAMESTREAM_SITE = https://github.com/moonlight-stream/moonlight-embedded.git
LIBGAMESTREAM_SITE_METHOD = git
LIBGAMESTREAM_GIT_SUBMODULES = YES
LIBGAMESTREAM_LICENSE = GPL-3.0+
LIBGAMESTREAM_LICENSE_FILES = LICENSE
LIBGAMESTREAM_INSTALL_STAGING = YES
LIBGAMESTREAM_INSTALL_TARGET = NO
LIBGAMESTREAM_DEPENDENCIES = openssl libcurl expat util-linux

# only the libraries are built (not moonlight-embedded's own program); static, so tvmoon is one file
define LIBGAMESTREAM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f $(LIBGAMESTREAM_PKGDIR)/Makefile.notatv \
		CC="$(TARGET_CC)" AR="$(TARGET_AR)" CFLAGS="$(TARGET_CFLAGS)"
endef

define LIBGAMESTREAM_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/libgamestream.a $(STAGING_DIR)/usr/lib/libgamestream.a
	mkdir -p $(STAGING_DIR)/usr/include/gamestream
	cp $(@D)/third_party/moonlight-common-c/src/Limelight.h $(STAGING_DIR)/usr/include/gamestream/
	cp $(@D)/libgamestream/client.h $(@D)/libgamestream/errors.h $(@D)/libgamestream/xml.h \
		$(@D)/libgamestream/mkcert.h $(STAGING_DIR)/usr/include/gamestream/
endef

$(eval $(generic-package))
