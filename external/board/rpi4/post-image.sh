#!/bin/sh
# NotaTV Pi 4: build the SD card image (boot + rootfs + data)
set -e
# which kernel set the firmware loads (see libexec/notatv/update): the root one; always 128 bytes, so the updater
# can switch it with one write
{ printf '# kernel set: the root of the boot partition\n# written by the NotaTV updater - do not edit\n'; } |
	awk '{ s = s $0 "\n" } END { printf "%s", s; for (i = length(s); i < 127; i++) printf "#"; printf "\n" }' \
	> "$BINARIES_DIR/rpi-firmware/notatv-slot.txt"
support/scripts/genimage.sh -c "$BR2_EXTERNAL_NOTATV_PATH/board/rpi4/genimage.cfg"
cp "$BINARIES_DIR/notatv-rpi4.img" "$BR2_EXTERNAL_NOTATV_PATH/../notatv-rpi4.img"
echo "NotaTV Pi 4-image: $BINARIES_DIR/notatv-rpi4.img ($(du -h "$BINARIES_DIR/notatv-rpi4.img" | cut -f1))"

# update package for "Check for updates" (put on the GitHub release together with .sha256)
U="$BR2_EXTERNAL_NOTATV_PATH/../notatv-rpi4-update.tar.gz"
S=$(mktemp -d)
mkdir -p "$S/boot"
cp -a "$BINARIES_DIR/Image" "$BINARIES_DIR"/bcm2711-rpi-4-b.dtb "$BINARIES_DIR"/bcm2711-rpi-400.dtb \
	"$BINARIES_DIR/rpi-firmware/start4.elf" "$BINARIES_DIR/rpi-firmware/fixup4.dat" "$BINARIES_DIR/rpi-firmware/overlays" "$S/boot/"
# users and groups: Buildroot adds the packages' users (dbus, avahi ...) only inside the rootfs image, not in
# $TARGET_DIR. An update with $TARGET_DIR's /etc/passwd removed them, so dbus-daemon and avahi-daemon could not
# start (no Bluetooth, AirPlay or PC search). Take the three files from the finished rootfs instead.
mkdir -p "$S/users/etc"
for f in passwd group shadow; do
	"$HOST_DIR/sbin/debugfs" -R "dump -p /etc/$f $S/users/etc/$f" "$BINARIES_DIR/rootfs.ext4" >/dev/null 2>&1
	[ -s "$S/users/etc/$f" ] || { echo "post-image: could not read /etc/$f from rootfs.ext4" >&2; exit 1; }
done
for u in dbus avahi; do
	grep -q "^$u:" "$S/users/etc/passwd" || { echo "post-image: user $u missing from /etc/passwd" >&2; exit 1; }
done
# (added as "etc/..." without "./", so the excludes below do not drop them; the 2nd transform puts them in rootfs/)
tar -czf "$U" --owner=0 --group=0 -C "$S" boot --transform 's,^\./,rootfs/,' --transform 's,^etc/,rootfs/etc/,' -C "$TARGET_DIR" \
	--exclude=./dev --exclude=./proc --exclude=./sys --exclude=./tmp --exclude=./run --exclude=./data \
	--exclude=./media --exclude=./mnt --exclude=./THIS_IS_NOT_YOUR_ROOT_FILESYSTEM \
	--exclude=./etc/passwd --exclude=./etc/group --exclude=./etc/shadow . \
	-C "$S/users" etc/passwd etc/group etc/shadow
rm -rf "$S"
(cd "${U%/*}" && sha256sum "${U##*/}" > "${U##*/}.sha256")
echo "Opdateringspakke: $U ($(du -h "$U" | cut -f1))"
