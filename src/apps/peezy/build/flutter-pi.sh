#!/bin/sh
# Cross-builds flutter-pi (pinned commit) for the peezy package: no GStreamer, no Vulkan, no libseat, and our own
# small sd-event instead of libsystemd. Our built-in plugins (src/apps/peezy/flutter-pi/plugins/*.c) are added.
set -eu
FPI_COMMIT=edda7f9a216a2fd60586ef350aac84ebaa847bb0
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
APP=$ROOT/src/apps/peezy
HOST=$ROOT/output-rpi4/host
W=$ROOT/tools/work
S=$HOST/aarch64-buildroot-linux-gnu/sysroot
XPFX=$S/../../../../tools/work/xsys/usr
SRC=$W/flutter-pi
[ -d "$SRC/.git" ] || git clone -q https://github.com/ardera/flutter-pi.git "$SRC"
git -C "$SRC" fetch -q --depth 1 origin $FPI_COMMIT 2>/dev/null || true
git -C "$SRC" checkout -q -f $FPI_COMMIT
git -C "$SRC" clean -qfdx -e build
git -C "$SRC" apply "$APP/flutter-pi/flutter-pi.patch"   # lend the display to tvshell's player and take it back
# libsystemd -> our sd-event; our plugins compiled in
sed -i -e 's|^pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)|add_library(nota_sdevent STATIC '"$APP"'/flutter-pi/sdevent/sd-event.c)\ntarget_include_directories(nota_sdevent PUBLIC '"$APP"'/flutter-pi/sdevent)\nset_target_properties(nota_sdevent PROPERTIES POSITION_INDEPENDENT_CODE ON)|' \
       -e 's|^  PkgConfig::LIBSYSTEMD$|  nota_sdevent|' "$SRC/CMakeLists.txt"
grep -q nota_sdevent "$SRC/CMakeLists.txt" || { echo "patch failed"; exit 1; }
PLUG=$(ls "$APP"/flutter-pi/plugins/*.c 2>/dev/null | tr "\n" " " || true)
if [ -n "$PLUG" ]; then
	printf '\ntarget_sources(flutterpi_module PRIVATE %s %s)\ntarget_include_directories(flutterpi_module PRIVATE %s)\ntarget_link_libraries(flutterpi_module PUBLIC mpv)\n' \
		"$PLUG" "$ROOT/src/shell/vendor/cJSON.c" "$ROOT/src/shell/vendor" >> "$SRC/CMakeLists.txt"
fi
export PKG_CONFIG_LIBDIR=$XPFX/lib/pkgconfig:$S/usr/lib/pkgconfig:$S/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$S
B=$SRC/build
cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_MAKE_PROGRAM="$HOST/bin/ninja" -DCMAKE_TOOLCHAIN_FILE="$HOST/share/buildroot/toolchainfile.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DPKG_CONFIG_EXECUTABLE="$HOST/bin/pkgconf" \
	-DBUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=OFF -DTRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=OFF \
	-DBUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN=OFF -DTRY_BUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN=OFF \
	-DENABLE_VULKAN=OFF -DENABLE_SESSION_SWITCHING=OFF -DTRY_ENABLE_SESSION_SWITCHING=OFF -DENABLE_SOFTWARE=OFF \
	-DBUILD_SENTRY_PLUGIN=OFF -DLTO=ON -DCMAKE_EXE_LINKER_FLAGS="-L$W/xsys/usr/lib -Wl,-rpath-link,$W/xsys/usr/lib -Wl,-rpath,'\$ORIGIN/lib'" >/dev/null
"$HOST/bin/ninja" -C "$B" flutter-pi | grep -E "error|FAILED" || true
[ -f "$B/flutter-pi" ] || exit 1
"$HOST/bin/aarch64-linux-strip" -o "$W/flutter-pi.bin" "$B/flutter-pi"
echo "$W/flutter-pi.bin"
