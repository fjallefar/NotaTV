# NotaTV - build the whole system (one image for Raspberry Pi 4 and Pi 5)
#   make / make rpi4  build everything: output-rpi4/, gives notatv-rpi4.img and the update package
#                     (fetches Raspberry Pi's ffmpeg into src/rpi-ffmpeg the first time)
#   make shell        build and run the user interface on the development PC (window)
#   make menuconfig / linux-menuconfig   change the Buildroot/kernel configuration
#   make clean-shell  rebuild only tvshell on the next build
.NOTPARALLEL:
ROOT := $(CURDIR)
BR := $(ROOT)/buildroot
BR_VERSION := 2026.08.1
# per-package directories, so Buildroot can build packages in parallel
OUT_RPI := $(ROOT)/output-rpi4
BRMAKE_RPI := $(MAKE) -C $(BR) O=$(OUT_RPI) BR2_EXTERNAL=$(ROOT)/external
JOBS ?= $(shell nproc)
# Raspberry Pi's ffmpeg fork (not in git): fetched automatically at the pinned commit + our fixes
FF_DIR := $(ROOT)/src/rpi-ffmpeg
FF_COMMIT := $(shell cat $(ROOT)/external/board/rpi4/rpi-ffmpeg.commit)
# commit + hash of our patches: src/rpi-ffmpeg is out of date when either changed
FF_STAMP := $(FF_COMMIT) $(shell cat $(ROOT)/external/board/rpi4/rpi-ffmpeg-patches/*.patch | sha256sum | cut -c1-16)

all: rpi4

$(BR)/Makefile:
	git clone --depth 1 --branch $(BR_VERSION) https://gitlab.com/buildroot.org/buildroot.git $(BR)

rpi4: $(OUT_RPI)/.config rpi-ffmpeg
	$(BRMAKE_RPI) -j$(JOBS)
	@echo; echo "Færdig: $(ROOT)/notatv-rpi4.img"

$(OUT_RPI)/.config: $(BR)/Makefile external/configs/notatv_rpi4_defconfig
	$(BRMAKE_RPI) notatv_rpi4_defconfig

rpi-ffmpeg:
	@if [ ! -d $(FF_DIR) ]; then \
		set -e; git init -q $(FF_DIR); \
		git -C $(FF_DIR) fetch --depth 1 https://github.com/jc-kynesim/rpi-ffmpeg $(FF_COMMIT); \
		git -C $(FF_DIR) checkout -q FETCH_HEAD; \
		for p in $(ROOT)/external/board/rpi4/rpi-ffmpeg-patches/*.patch; do git -C $(FF_DIR) apply $$p; done; \
		rm -rf $(FF_DIR)/.git; echo '$(FF_STAMP)' > $(FF_DIR)/.notatv-commit; \
	elif [ "$$(cat $(FF_DIR)/.notatv-commit 2>/dev/null)" != '$(FF_STAMP)' ]; then \
		echo "src/rpi-ffmpeg er ikke commit $(FF_COMMIT) med de nuværende patches - slet mappen, så hentes den igen"; exit 1; \
	fi

rpi-ffmpeg-stamp:
	@echo '$(FF_STAMP)'

clean-shell: $(OUT_RPI)/.config
	$(BRMAKE_RPI) tvshell-dirclean

menuconfig linux-menuconfig savedefconfig: $(OUT_RPI)/.config
	$(BRMAKE_RPI) $@

shell:
	$(MAKE) -C src/shell
	$(MAKE) -C src/apps
	cd src/shell && TVSHELL_ASSETS=$$PWD/assets TVSHELL_DATA=/tmp/notatv-dev TVSHELL_APPS=$(ROOT)/src/apps/out ./tvshell

.PHONY: all rpi4 rpi-ffmpeg rpi-ffmpeg-stamp clean-shell menuconfig linux-menuconfig savedefconfig shell
