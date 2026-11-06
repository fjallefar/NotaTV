################################################################################
#
# yt-dlp
#
################################################################################

# the "onedir" version: starts in ~0.3 s (the onefile version unpacks 40 MB on every start)
YT_DLP_VERSION = 2026.10.26
YT_DLP_BIN = yt-dlp_linux_aarch64
YT_DLP_SOURCE = $(YT_DLP_BIN).zip
YT_DLP_SITE = https://github.com/yt-dlp/yt-dlp/releases/download/$(YT_DLP_VERSION)
YT_DLP_LICENSE = Unlicense
YT_DLP_DEPENDENCIES = quickjs

# QuickJS spends ~25 s on YouTube's JS challenge on the Pi, Node (V8) ~5 s. Prebuilt official binary.
YT_DLP_NODE_VERSION = v26.13.0
YT_DLP_NODE_SOURCE = node-$(YT_DLP_NODE_VERSION)-linux-arm64.tar.xz
YT_DLP_EXTRA_DOWNLOADS = https://nodejs.org/dist/$(YT_DLP_NODE_VERSION)/$(YT_DLP_NODE_SOURCE)
YT_DLP_JS_RUNTIMES = --js-runtimes node\n--js-runtimes quickjs\n
define YT_DLP_INSTALL_NODE
	$(XZCAT) $(YT_DLP_DL_DIR)/$(YT_DLP_NODE_SOURCE) | tar -xO -f - node-$(YT_DLP_NODE_VERSION)-linux-arm64/bin/node > $(TARGET_DIR)/usr/bin/node
	chmod 0755 $(TARGET_DIR)/usr/bin/node
endef

define YT_DLP_EXTRACT_CMDS
	unzip -q -o $(YT_DLP_DL_DIR)/$(YT_DLP_SOURCE) -d $(@D)/dist
endef

define YT_DLP_INSTALL_TARGET_CMDS
	rm -rf $(TARGET_DIR)/usr/lib/yt-dlp
	mkdir -p $(TARGET_DIR)/usr/lib/yt-dlp
	cp -a $(@D)/dist/. $(TARGET_DIR)/usr/lib/yt-dlp/
	chmod 0755 $(TARGET_DIR)/usr/lib/yt-dlp/$(YT_DLP_BIN)
	rm -f $(TARGET_DIR)/usr/bin/yt-dlp
	ln -sf ../lib/yt-dlp/$(YT_DLP_BIN) $(TARGET_DIR)/usr/bin/yt-dlp
	# the image's version: mediarun uses a yt-dlp from the App Store only if it is newer
	echo $(YT_DLP_VERSION) > $(TARGET_DIR)/usr/lib/yt-dlp/notatv-version
	mkdir -p $(TARGET_DIR)/etc
	printf -- '$(YT_DLP_JS_RUNTIMES)--cache-dir /data/notatv/yt-dlp-cache\n' > $(TARGET_DIR)/etc/yt-dlp.conf
	$(YT_DLP_INSTALL_NODE)
endef

$(eval $(generic-package))
