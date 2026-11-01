#!/bin/sh
# Run tvshell headless on the development PC and save screenshots as PNG.
# Usage: scripts/uitest.sh <outdir> "<script>"   (e.g. "w:200;s:home;k:right;w:100;s:plex;q")
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
mkdir -p "$OUT/data"
SCRIPT=$(printf '%s' "$2" | sed "s#s:\([A-Za-z0-9_-]*\)#s:$OUT/\1.bmp#g")
cd "$ROOT/src/shell"
SDL_VIDEO_DRIVER=offscreen TVSHELL_ASSETS=$PWD/assets TVSHELL_DATA=$OUT/data \
TVSHELL_HELPERS=${TVSHELL_HELPERS:-$ROOT/external/board/notatv/rootfs-overlay/usr/libexec/notatv} \
TVSHELL_SIZE=${TVSHELL_SIZE:-1920x1080} TVSHELL_SCRIPT="$SCRIPT" timeout ${TIMEOUT:-60} ./tvshell > "$OUT/log.txt" 2>&1 || true
for f in "$OUT"/*.bmp; do
  [ -f "$f" ] || continue
  python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[1][:-4]+'.png')" "$f" && rm "$f"
done
ls "$OUT"
