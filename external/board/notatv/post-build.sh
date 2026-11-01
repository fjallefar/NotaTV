#!/bin/sh
# NotaTV post-build: cleans up the rootfs and sets version info
set -e
TARGET=$1
# we manage network and ssh ourselves (see /usr/libexec/notatv)
rm -f $TARGET/etc/init.d/S40network $TARGET/etc/init.d/S50dropbear $TARGET/etc/init.d/S41dhcpcd
# AirPlay (shairport-sync/nqptp) is started and paused by /usr/libexec/notatv/music (frees the audio during video)
rm -f $TARGET/etc/init.d/S99shairport-sync $TARGET/etc/init.d/S90nqptp
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
EOR
# run S05/S60 early/late; no login prompt on tty1
sed -i '/tty1/d' $TARGET/etc/inittab

# --- smaller image = faster boot (the kernel unpacks everything at boot) ---
FW=$TARGET/lib/firmware
# Intel GPU: keep Skylake..Alder Lake/N100 (Gen9-Gen12); remove Arc/Meteor Lake and newer
for p in dg1 dg2 mtl bmg lnl ptl pvc xe2 xe3 gsc arl; do rm -f $FW/i915/${p}_* 2>/dev/null; done
rm -rf $FW/xe 2>/dev/null
# iwlwifi/rtw89: keep only the newest firmware version per chip
keep_newest() { # directory
	d=$1; [ -d "$d" ] || return 0
	ls "$d" | sed -n 's/^\(.*[-_]\)\([0-9][0-9]*\)\.\(ucode\|bin\|pnvm\)$/\1 \2 \3/p' | sort -k1,1 -k2,2n |
	awk '{k=$1" "$3; last[k]=$1$2"."$3; all[k]=all[k]" "$1$2"."$3} END {for (k in all) {n=split(all[k],a," "); for (i=1;i<=n;i++) if (a[i]!=last[k]) print a[i]}}' |
	while read -r f; do rm -f "$d/$f"; done
}
keep_newest $FW/intel/iwlwifi
keep_newest $FW/rtw89
rm -f $FW/rtw89/rtw8922* 2>/dev/null   # Wi-Fi 7 cards (not in mini PCs yet)
# dead symlinks after the cleanup
find $FW -xtype l -delete 2>/dev/null || true
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

# file list for the updater: a file that disappears in a later version is only removed from a box if it is
# listed here (same exclusions as the update package in board/rpi4/post-image.sh). Must be the last step.
mkdir -p $TARGET/usr/share/notatv
{ (cd $TARGET && find . ! -type d) | sed 's,^\./,,' |
	grep -vE '^(dev|proc|sys|tmp|run|data|media|mnt)/|^THIS_IS_NOT_YOUR_ROOT_FILESYSTEM$|^usr/share/notatv/files\.list$'
  echo usr/share/notatv/files.list; } | LC_ALL=C sort > $TARGET/usr/share/notatv/files.list
