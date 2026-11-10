#!/bin/sh
# Cross-builds the libraries flutter-pi needs that NotaTV's image does not have (libevdev, mtdev, libinput,
# libxkbcommon) into $XSYS. They are shipped inside the peezy package only, never in the image.
# Sources are checked against Buildroot's own hashes.
set -eu
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
BR=$ROOT/buildroot
HOST=$ROOT/output-rpi4/host
S=$HOST/aarch64-buildroot-linux-gnu/sysroot
W=${PEEZY_WORK:-$ROOT/tools/work}
XSYS=$W/xsys
# pkgconf only leaves a path alone when it starts with the sysroot, so the prefix is spelled from there
XPFX=$S/../../../../tools/work/xsys/usr
[ "$W" = "$ROOT/tools/work" ] || { echo "PEEZY_WORK must be tools/work"; exit 1; }
mkdir -p "$W/src" "$XSYS"
PATH=$HOST/bin:$PATH; export PATH
CROSS=$W/meson-cross.conf
sed -e "s|^pkg_config_libdir = '|pkg_config_libdir = '$XPFX/lib/pkgconfig:$XPFX/share/pkgconfig:|" \
    -e "s|^cpu = .*|cpu = 'cortex-a72'|" "$HOST/etc/meson/cross-compilation.conf" > "$CROSS"

fetch() { # name file url
	f=$W/src/$2
	[ -f "$f" ] || curl -fsSL -o "$f" "$3"
	want=$(awk -v f="$2" '$1=="sha256" && $3==f {print $2}' "$BR/package/$1/$1.hash")
	[ -n "$want" ] || { echo "no hash for $2"; exit 1; }
	echo "$want  $f" | sha256sum -c --quiet
	d=$W/src/$1; rm -rf "$d"; mkdir -p "$d"; tar xf "$f" -C "$d" --strip-components=1
}
mesonbuild() { # dir options...
	d=$1; shift
	meson setup --wipe "$d/b" "$d" --cross-file "$CROSS" --prefix="$XPFX" --libdir=lib --buildtype=release \
		-Ddefault_library=shared "$@" >/dev/null
	ninja -C "$d/b" install >/dev/null
}
v() { sed -n "s/^$1_VERSION = //p" "$BR/package/$2/$2.mk"; }

V=$(v LIBEVDEV libevdev); fetch libevdev libevdev-$V.tar.xz http://www.freedesktop.org/software/libevdev/libevdev-$V.tar.xz
mesonbuild "$W/src/libevdev" -Dtests=disabled -Ddocumentation=disabled -Dtools=disabled
V=$(v MTDEV mtdev); fetch mtdev mtdev-$V.tar.bz2 http://bitmath.org/code/mtdev/mtdev-$V.tar.bz2
( cd "$W/src/mtdev" && ./configure -q --host=aarch64-linux --prefix="$XPFX" --disable-static CC=aarch64-linux-gcc >/dev/null && make -s install >/dev/null )
V=$(v LIBINPUT libinput); fetch libinput libinput-$V.tar.bz2 https://gitlab.freedesktop.org/libinput/libinput/-/archive/$V/libinput-$V.tar.bz2
mesonbuild "$W/src/libinput" -Dlibwacom=false -Ddebug-gui=false -Dtests=false -Ddocumentation=false -Dinstall-tests=false -Dzshcompletiondir=no -Dmtdev=true
V=$(v LIBXKBCOMMON libxkbcommon); fetch libxkbcommon libxkbcommon-$V.tar.gz https://github.com/xkbcommon/libxkbcommon/archive/xkbcommon-$V/libxkbcommon-$V.tar.gz
mesonbuild "$W/src/libxkbcommon" -Denable-x11=false -Denable-wayland=false -Denable-docs=false -Denable-tools=false -Denable-xkbregistry=false -Denable-bash-completion=false -Dxkb-config-root=/usr/share/X11/xkb
# meson writes the resolved path into the .pc files; put the sysroot-relative spelling back
R=$(cd "$XSYS/usr" && pwd)
sed -i "s|$R|$XPFX|g" "$XSYS"/usr/lib/pkgconfig/*.pc
ls "$XSYS/usr/lib"/*.so.*
