################################################################################
#
# yt-dlp
#
################################################################################

# F1 step 1 (audit 9/11): yt-dlp (55 MB) and Node.js (110 MB) are no longer in the image - the rootfs had too little
# room for updates. Both are tools from the App Store (src/tools/yt-dlp, src/tools/node), installed by the box in
# /data/notatv/tools/<id> (appcat.c, apppkg tool-install) and run through mediarun. The image only has QuickJS
# (a slow fallback for YouTube's JS challenge) and this config. yt-dlp skips a JS runtime that is not there.
YT_DLP_VERSION = 2
YT_DLP_SOURCE =
YT_DLP_LICENSE = Unlicense
YT_DLP_DEPENDENCIES = quickjs

define YT_DLP_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/etc
	printf -- '--js-runtimes node:/data/notatv/tools/node/node\n--js-runtimes quickjs\n--cache-dir /data/notatv/yt-dlp-cache\n' > $(TARGET_DIR)/etc/yt-dlp.conf
endef

$(eval $(generic-package))
