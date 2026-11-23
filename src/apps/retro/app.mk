# Retro games: RetroArch, the free cores and the controller profiles come from Buildroot's staging
# (external/package/retroarch, libretro-*, retroarch-joypad-autoconfig; tvshell.mk passes RETRO_STAGE).
# On a PC build RETRO_STAGE is empty: the package has only app.so and the app says RetroArch is missing.
ifneq ($(RETRO_STAGE),)
APP_EXTRA += $(OUT)/retro/retroarch
$(OUT)/retro/retroarch: $(wildcard $(RETRO_STAGE)/retroarch) $(wildcard $(RETRO_STAGE)/cores/*.so)
	mkdir -p $(OUT)/retro/cores
	install -m 0755 $(RETRO_STAGE)/retroarch $@
	cp -a $(RETRO_STAGE)/cores/. $(OUT)/retro/cores/
	if [ -d $(RETRO_STAGE)/autoconfig ]; then rm -rf $(OUT)/retro/autoconfig; mkdir -p $(OUT)/retro/autoconfig; cp -a $(RETRO_STAGE)/autoconfig/udev $(OUT)/retro/autoconfig/udev; cp $(RETRO_STAGE)/autoconfig/COPYING $(OUT)/retro/autoconfig/; fi
endif
