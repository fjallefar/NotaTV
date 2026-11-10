#!/bin/sh
# The keyboard maps flutter-pi needs (xkbcommon reads them at start): xkeyboard-config built as data, and only the
# parts US and Danish keyboards (and the TV remote's keys) use. About 640 KB.
set -eu
V=2.44
SHA=54d2c33eeebb031d48fa590c543e54c9bcbd0f00386ebc6489b2f47a0da4342a
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
W=$ROOT/tools/work
PATH=$ROOT/output-rpi4/host/bin:$PATH; export PATH
f=$W/src/xkeyboard-config-$V.tar.xz
mkdir -p "$W/src"
[ -f "$f" ] || curl -fsSL -o "$f" https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-$V.tar.xz
echo "$SHA  $f" | sha256sum -c --quiet
s=$W/src/xkbc; rm -rf "$s"; mkdir -p "$s"; tar xf "$f" -C "$s" --strip-components=1
meson setup "$s/b" "$s" --prefix="$s/inst" >/dev/null
ninja -C "$s/b" install >/dev/null
X=$s/inst/share/X11/xkb
rm -rf "$W/xkb"; mkdir -p "$W/xkb/rules" "$W/xkb/symbols"
cp -r "$X/keycodes" "$X/types" "$X/compat" "$W/xkb/"
cp "$X/rules/evdev" "$W/xkb/rules/"
for s in pc us dk latin inet srvr_ctrl keypad altwin level3 level5 kpdl capslock ctrl group compose nbsp shift eurosign terminate; do
	cp "$X/symbols/$s" "$W/xkb/symbols/"
done
