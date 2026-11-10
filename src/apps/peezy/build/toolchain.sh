#!/bin/sh
# The Flutter toolchain Peezy is built with, in tools/ (git-ignored, kept between builds): Flutter SDK and
# flutterpi_tool at pinned versions, the SDK checked against Google's published SHA256. Writes tools/env.sh.
set -eu
FLUTTER_VER=3.47.9
FLUTTER_SHA=25ed8c3c29d51a7353f2a944310b478af8214920639524b19c50a19e6c044c5a
FLUTTERPI_TOOL_VER=0.13.0
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
T=$ROOT/tools
mkdir -p "$T"
cat > "$T/env.sh" <<ENV
# source this: the Flutter toolchain for Peezy (made by src/apps/peezy/build/toolchain.sh)
export PATH=$T/flutter/bin:$T/pub-cache/bin:\$PATH
export PUB_CACHE=$T/pub-cache FLUTTER_SUPPRESS_ANALYTICS=true
ENV
if [ "$(cat "$T/flutter.pinned" 2>/dev/null || true)" != "$FLUTTER_VER" ] || [ ! -x "$T/flutter/bin/flutter" ]; then
	f=$T/flutter_linux_$FLUTTER_VER-stable.tar.xz
	[ -f "$f" ] || curl -fsSL -o "$f" "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_$FLUTTER_VER-stable.tar.xz"
	echo "$FLUTTER_SHA  $f" | sha256sum -c --quiet
	rm -rf "$T/flutter"
	tar xf "$f" -C "$T"
	rm -f "$f"
	echo "$FLUTTER_VER" > "$T/flutter.pinned"
fi
. "$T/env.sh"
flutter config --no-analytics --no-cli-animations >/dev/null 2>&1 || true
if ! dart pub global list 2>/dev/null | grep -qx "flutterpi_tool $FLUTTERPI_TOOL_VER"; then
	dart pub global activate flutterpi_tool $FLUTTERPI_TOOL_VER >/dev/null
fi
flutter --version | head -1
