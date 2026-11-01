# NotaTV - build the whole system
#   make            build everything and create notatv.iso
#   make shell      build and run the user interface on the development PC (window)
#   make qemu       start the built ISO in QEMU
#   make menuconfig / linux-menuconfig   change the Buildroot/kernel configuration
#   make clean-shell  rebuild only tvshell on the next build
#   make rpi4       build the Raspberry Pi 4 version (output-rpi4/, gives notatv-rpi4.img)
#                   (fetches Raspberry Pi's ffmpeg into src/rpi-ffmpeg the first time)
.NOTPARALLEL:
ROOT := $(CURDIR)
BR := $(ROOT)/buildroot
OUT := $(ROOT)/output
BRMAKE := $(MAKE) -j1 -C $(BR) O=$(OUT) BR2_EXTERNAL=$(ROOT)/external
BR_VERSION := 2026.08.1
# Pi 4 is built next to x86 with per-package directories, so Buildroot can build packages in parallel
OUT_RPI := $(ROOT)/output-rpi4
BRMAKE_RPI := $(MAKE) -C $(BR) O=$(OUT_RPI) BR2_EXTERNAL=$(ROOT)/external
JOBS ?= $(shell nproc)
# Raspberry Pi's ffmpeg fork (not in git): fetched automatically at the pinned commit + our fixes
FF_DIR := $(ROOT)/src/rpi-ffmpeg
FF_COMMIT := $(shell cat $(ROOT)/external/board/rpi4/rpi-ffmpeg.commit)

all: $(OUT)/.config
	$(BRMAKE)
	@echo; echo "Færdig: $(ROOT)/notatv.iso"

$(BR)/Makefile:
	git clone --depth 1 --branch $(BR_VERSION) https://gitlab.com/buildroot.org/buildroot.git $(BR)

$(OUT)/.config: $(BR)/Makefile external/configs/notatv_x86_64_defconfig
	$(BRMAKE) notatv_x86_64_defconfig

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
		rm -rf $(FF_DIR)/.git; echo $(FF_COMMIT) > $(FF_DIR)/.notatv-commit; \
	elif [ "$$(cat $(FF_DIR)/.notatv-commit 2>/dev/null)" != "$(FF_COMMIT)" ]; then \
		echo "src/rpi-ffmpeg er ikke commit $(FF_COMMIT) - slet mappen, så hentes den igen"; exit 1; \
	fi

rpi4-menuconfig: $(OUT_RPI)/.config
	$(BRMAKE_RPI) menuconfig

clean-shell:
	$(BRMAKE) tvshell-dirclean

menuconfig linux-menuconfig savedefconfig: $(OUT)/.config
	$(BRMAKE) $@

shell:
	$(MAKE) -C src/shell
	cd src/shell && TVSHELL_ASSETS=$$PWD/assets TVSHELL_DATA=/tmp/notatv-dev ./tvshell

qemu:
	scripts/qemu.sh $(ROOT)/notatv.iso

.PHONY: all rpi4 rpi-ffmpeg rpi4-menuconfig clean-shell menuconfig linux-menuconfig savedefconfig shell qemu
