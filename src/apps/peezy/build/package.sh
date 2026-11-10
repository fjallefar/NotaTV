#!/bin/sh
# Puts everything Peezy needs next to its app.so in <out> (src/apps/out/peezy): flutter-pi, the libraries the
# image does not have (lib/), Plezy's Flutter bundle (bundle/) and a small keyboard map set (xkb/). Run after
# toolchain.sh, deps.sh, flutter-pi.sh, plezy.sh and xkb.sh (or with "all" to run them first).
set -eu
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
B=$ROOT/src/apps/peezy/build
W=$ROOT/tools/work
OUT=${1:?usage: package.sh <out dir> [all]}
if [ "${2:-}" = all ]; then "$B/toolchain.sh"; "$B/deps.sh"; "$B/flutter-pi.sh"; "$B/plezy.sh"; "$B/xkb.sh"; fi
BUNDLE=$W/plezy/build/flutter-pi/aarch64-generic
for f in "$W/flutter-pi.bin" "$BUNDLE/app.so" "$BUNDLE/libflutter_engine.so" "$W/xkb/rules/evdev"; do
	[ -e "$f" ] || { echo "peezy: $f is missing - run src/apps/peezy/build/package.sh <out> all"; exit 1; }
done
rm -rf "$OUT/lib" "$OUT/bundle" "$OUT/xkb" "$OUT/flutter-pi"
mkdir -p "$OUT/lib"
cp "$W/flutter-pi.bin" "$OUT/flutter-pi"
# real files under their sonames (packages hold only plain files)
for l in libinput.so.10 libxkbcommon.so.0 libevdev.so.2 libmtdev.so.1; do cp -L "$W/xsys/usr/lib/$l" "$OUT/lib/$l"; done
cp -r "$BUNDLE" "$OUT/bundle"
rm -f "$OUT/bundle/flutter-pi" "$OUT/bundle/.last_build_id"
# not used on NotaTV: mpv shaders (NotaTV's player renders), alternate app icons, Linux leftovers
rm -rf "$OUT/bundle/assets/shaders" "$OUT/bundle/assets/app_icons" "$OUT/bundle/linux"
cp -r "$W/xkb" "$OUT/xkb"
find "$OUT/lib" "$OUT/bundle" "$OUT/xkb" -type l -exec sh -c 'echo "peezy: symlink $1"; exit 1' _ {} \;
du -sh "$OUT"
