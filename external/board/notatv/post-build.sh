#!/bin/sh
# NotaTV post-build: cleans up the rootfs and sets version info
set -e
TARGET=$1
# we manage network and ssh ourselves (see /usr/libexec/notatv)
rm -f $TARGET/etc/init.d/S40network $TARGET/etc/init.d/S50dropbear $TARGET/etc/init.d/S41dhcpcd
# AirPlay (shairport-sync/nqptp) is started and paused by /usr/libexec/notatv/music (frees the audio during video)
rm -f $TARGET/etc/init.d/S99shairport-sync $TARGET/etc/init.d/S90nqptp
# the firewall is set up by /usr/libexec/notatv/firewall (with the box's own networks), not from /etc/nftables.conf
rm -f $TARGET/etc/init.d/S35nftables
# mpv, ffprobe, yt-dlp and UxPlay are started through mediarun (as the user "media" unless media_user=0): the real
# programs move to /usr/libexec/notatv/media, /usr/bin/<name> becomes a symlink to mediarun (yt-dlp itself is a
# tool from the App Store in /data/notatv/tools). Only a real file is moved, so running post-build again changes nothing.
mkdir -p $TARGET/usr/libexec/notatv/media
for p in mpv ffprobe uxplay; do
	if [ -f $TARGET/usr/bin/$p ] && [ ! -L $TARGET/usr/bin/$p ]; then mv -f $TARGET/usr/bin/$p $TARGET/usr/libexec/notatv/media/$p; fi
	[ -f $TARGET/usr/libexec/notatv/media/$p ] && ln -sfn ../libexec/notatv/mediarun $TARGET/usr/bin/$p
done
ln -sfn ../libexec/notatv/mediarun $TARGET/usr/bin/yt-dlp
# F1 step 1: yt-dlp and Node are tools on /data now. Buildroot keeps old files in the target folder, so remove the
# ones an older yt-dlp package installed (165 MB), or they would stay in the image
rm -rf $TARGET/usr/lib/yt-dlp
rm -f $TARGET/usr/bin/node
[ -x $TARGET/usr/libexec/notatv/mediarun ] || { echo "post-build: mediarun mangler" >&2; exit 1; }
# only the modules of the kernel that is built: Buildroot keeps old files in the target folder, so after a kernel
# lift the old /lib/modules/<version> would stay in the image and the update (28 MB for nothing)
KREL=$(cat $BUILD_DIR/linux-custom/include/config/kernel.release 2>/dev/null || true)
if [ -n "$KREL" ] && [ -d $TARGET/lib/modules/$KREL ]; then
	for d in $TARGET/lib/modules/*; do [ "${d##*/}" = "$KREL" ] || rm -rf "$d"; done
fi
# the alsa configuration is generated at boot
ln -sf /run/notatv/asound.conf $TARGET/etc/asound.conf
mkdir -p $TARGET/media $TARGET/data $TARGET/root
# no documentation/development files in the image (smaller = faster boot)
rm -rf $TARGET/usr/share/man $TARGET/usr/share/doc $TARGET/usr/share/info $TARGET/usr/include \
       $TARGET/usr/lib/pkgconfig $TARGET/usr/share/pkgconfig $TARGET/usr/lib/cmake
find $TARGET/usr/lib $TARGET/lib -name '*.a' -delete 2>/dev/null || true
# version
VER=$(cat $BR2_EXTERNAL_NOTATV_PATH/../VERSION 2>/dev/null || echo 1.0.0)
cat > $TARGET/etc/notatv-release <<EOR
NAME=NotaTV
VERSION=$VER ($(date +%Y-%m-%d))
BUILD=$(date -u +%Y%m%d%H%M)
BUILD_EPOCH=$(date -u +%s)
EOR
# run S05/S60 early/late; no login prompt on tty1
sed -i '/tty1/d' $TARGET/etc/inittab

# --- smaller image = faster boot (the kernel unpacks everything at boot) ---
# fonts: only DejaVu Sans (subtitles)
find $TARGET/usr/share/fonts/dejavu -type f ! -name 'DejaVuSans.ttf' ! -name 'DejaVuSans-Bold.ttf' -delete 2>/dev/null || true
# time zones: all zones (country/region is chosen in Settings), but without "right/" (leap seconds, ~1.4 MB).
# Europe/, America/ etc. are symlinks into posix/, so posix/ must stay.
if [ -d $TARGET/usr/share/zoneinfo ]; then
	rm -rf $TARGET/usr/share/zoneinfo/right
	find $TARGET/usr/share/zoneinfo -maxdepth 1 -xtype l -delete
	if [ ! -e $TARGET/usr/share/zoneinfo/Europe/Copenhagen ]; then
		echo "post-build: zoneinfo er ufuldstændig - kør 'make ... tzdata-reinstall' først" >&2
		exit 1
	fi
fi
# /tmp is in RAM: at most 256 MB (image cache, logs, yt-dlp), so a runaway file cannot eat the memory
grep -q '/tmp.*size=256m' $TARGET/etc/fstab || sed -i 's|^tmpfs\([[:space:]]*\)/tmp\([[:space:]]*\)tmpfs\([[:space:]]*\)mode=1777\b|tmpfs\1/tmp\2tmpfs\3mode=1777,size=256m|' $TARGET/etc/fstab
grep -q '/tmp.*size=256m' $TARGET/etc/fstab || { echo "post-build: /tmp-grænsen kom ikke i fstab" >&2; exit 1; }
# the rootfs must not change at runtime (preparation for a read-only rootfs):
# DNS is written to /run, SSH keys to /data, and crond (no jobs) is not started
ln -sfn ../run/resolv.conf $TARGET/etc/resolv.conf
rm -rf $TARGET/etc/dropbear; ln -s /data/notatv/dropbear $TARGET/etc/dropbear
rm -f $TARGET/etc/init.d/S50crond
# data that changes at runtime lives on the data partition (S05notatv creates the directories):
# Bluetooth pairings, bluealsa, Mesa's shader cache and mpv's config directory
for d in var/lib/bluetooth:bluetooth var/lib/bluealsa:bluealsa root/.cache:cache root/.config:config; do
	src=${d%%:*}; dst=${d##*:}
	rm -rf $TARGET/$src
	mkdir -p $(dirname $TARGET/$src)
	ln -s /data/notatv/$dst $TARGET/$src
done
# the clock: tvshell writes the chosen zone as a symlink in /data (the rootfs may become read-only later)
ln -sfn /data/notatv/localtime $TARGET/etc/localtime
rm -f $TARGET/etc/timezone

# Let's Encrypt's new chain (YR2/YE2 -> Root YR/YE) is not in Mozilla's list yet, and some servers (shared Plex servers
# on plex.direct) do not send the certificate linking it to ISRG Root X1/X2. These two are that link: signed by roots
# already in the list, so they add no new trust. Remove them when Root YR/YE are in ca-certificates.
CA_BUNDLE=$TARGET/etc/ssl/certs/ca-certificates.crt
if [ -f "$CA_BUNDLE" ] && ! grep -q 'NotaTV: Root YR by X1' "$CA_BUNDLE"; then
	{ echo; echo '# NotaTV: Root YR by X1'; cat "$BR2_EXTERNAL_NOTATV_PATH/board/notatv/ca-extra/root-yr-by-x1.pem"
	  echo '# NotaTV: Root YE by X2'; cat "$BR2_EXTERNAL_NOTATV_PATH/board/notatv/ca-extra/root-ye-by-x2.pem"; } >> "$CA_BUNDLE"
fi

# file list for the updater: a file that disappears in a later version is only removed from a box if it is
# listed here (same exclusions as the update package in board/rpi4/post-image.sh). Must be the last step.
mkdir -p $TARGET/usr/share/notatv
{ (cd $TARGET && find . ! -type d) | sed 's,^\./,,' |
	grep -vE '^(dev|proc|sys|tmp|run|data|media|mnt)/|^THIS_IS_NOT_YOUR_ROOT_FILESYSTEM$|^usr/share/notatv/files\.list$'
  echo usr/share/notatv/files.list; } | LC_ALL=C sort > $TARGET/usr/share/notatv/files.list
