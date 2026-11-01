#!/bin/sh
# NotaTV Pi 4: build the SD card image (boot + rootfs + data)
set -e
support/scripts/genimage.sh -c "$BR2_EXTERNAL_NOTATV_PATH/board/rpi4/genimage.cfg"
cp "$BINARIES_DIR/notatv-rpi4.img" "$BR2_EXTERNAL_NOTATV_PATH/../notatv-rpi4.img"
echo "NotaTV Pi 4-image: $BINARIES_DIR/notatv-rpi4.img ($(du -h "$BINARIES_DIR/notatv-rpi4.img" | cut -f1))"

# update package for "Check for updates" (put on the GitHub release together with .sha256)
U="$BR2_EXTERNAL_NOTATV_PATH/../notatv-rpi4-update.tar.gz"
S=$(mktemp -d)
mkdir -p "$S/boot"
cp -a "$BINARIES_DIR/Image" "$BINARIES_DIR"/bcm2711-rpi-4-b.dtb "$BINARIES_DIR"/bcm2711-rpi-400.dtb \
	"$BINARIES_DIR/rpi-firmware/start4.elf" "$BINARIES_DIR/rpi-firmware/fixup4.dat" "$BINARIES_DIR/rpi-firmware/overlays" "$S/boot/"
tar -czf "$U" --owner=0 --group=0 -C "$S" boot --transform 's,^\./,rootfs/,' -C "$TARGET_DIR" \
	--exclude=./dev --exclude=./proc --exclude=./sys --exclude=./tmp --exclude=./run --exclude=./data \
	--exclude=./media --exclude=./mnt --exclude=./THIS_IS_NOT_YOUR_ROOT_FILESYSTEM .
rm -rf "$S"
(cd "${U%/*}" && sha256sum "${U##*/}" > "${U##*/}.sha256")
echo "Opdateringspakke: $U ($(du -h "$U" | cut -f1))"
