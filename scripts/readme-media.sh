#!/bin/bash
# Makes the screenshots, the demo video and the animated preview for README.md in .github/media/.
# Usage: scripts/readme-media.sh [workdir]   (needs ffmpeg with libx264/libwebp and python3-pillow)
# Runs tvshell headless on this PC (TVSHELL_RECORD, see main.c). Only demo data may end up in the pictures:
# the helpers are stubs (a made-up IP, no network scans, nothing that can restart or change this PC), Tesla is not
# scanned for, and no app that finds servers on the LAN (Seerr, Plex, Jellyfin, Games) is opened.
# All files must stay under 2 MB (export-public.sh stops on larger files).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-/tmp/notatv-readme}
OUT=$ROOT/.github/media
rm -rf "$W"; mkdir -p "$W/helpers" "$OUT"
# helpers: "net ip" answers a made-up address; apppkg is the real one (App Store catalog, read only)
printf '#!/bin/sh\n[ "$1" = ip ] && echo 192.168.1.20\nexit 0\n' > "$W/helpers/net"
chmod +x "$W/helpers/net"
ln -s "$ROOT/external/board/notatv/rootfs-overlay/usr/libexec/notatv/apppkg" "$W/helpers/apppkg"
export TVSHELL_HELPERS=$W/helpers TVSHELL_TESLA_NOSCAN=1 TIMEOUT=180
make -s -C "$ROOT/src/shell" tvshell

run() { # name script [record.mp4]
	d=$W/$1
	mkdir -p "$d/data"
	printf 'setup_done=1\nel_grid=5790000705184\nupdate_repo=fjallefar/NotaTV\n' > "$d/data/notatv.conf"
	TVSHELL_RECORD=${3:-} "$ROOT/scripts/uitest.sh" "$d" "$2" >/dev/null
}

# home screen: Live TV 0 ... Music 6, Radio 7, Weather 8, Electricity price 9, Games 10, Media 11, App Store 12,
# Settings 13. Without recording "w:" counts frames at full speed (hundreds per second), so the waits are long.
R5="k:right;w:5;k:right;w:5;k:right;w:5;k:right;w:5;k:right;w:5"
run home "w:150;s:a;q"
run control-center "w:120;k:up;w:20;k:ok;w:600;s:a;q"
run weather "w:90;k:down;w:5;k:right;w:5;k:ok;w:240;s:a;q"
run electricity "w:90;k:down;w:5;k:right;w:5;k:right;w:5;k:ok;w:240;s:a;q"
run radio "w:90;k:down;w:5;k:ok;w:600;s:a;q"
run drtv "w:90;k:right;w:5;k:right;w:5;k:ok;w:1500;s:a;q"
run appstore "w:90;k:down;w:5;$R5;w:10;k:ok;w:900;k:down;w:20;k:down;w:20;k:down;w:20;k:down;w:60;s:a;q"
for n in home control-center weather electricity radio drtv appstore; do
	python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGB').resize((1600, 900), Image.LANCZOS).save(sys.argv[2], 'WEBP', quality=85, method=6)" \
		"$W/$n/a.png" "$OUT/$n.webp"
done

# the demo: recorded at 30 fps, here "w:30" is one second
P="w:60;k:right;w:14;k:right;w:14;k:right;w:16;k:down;w:14;k:left;w:14;k:left;w:20"           # home -> Weather
P="$P;k:ok;w:90;k:back;w:20"                                                                # Weather
P="$P;k:right;w:16;k:ok;w:60;k:left;w:7;k:left;w:7;k:left;w:7;k:left;w:25;k:back;w:20"       # Electricity price
P="$P;k:left;w:10;k:left;w:16;k:ok;w:70;k:down;w:15;k:down;w:30;k:back;w:20"               # Radio
P="$P;k:up;w:12;k:right;w:12;k:right;w:16;k:ok;w:110;k:down;w:22;k:down;w:40;k:back;w:20"  # DRTV
P="$P;k:up;w:16;k:ok;w:60;k:back;w:20;q"                                                    # control center
run demo "$P" "$W/demo-full.mp4"
ffmpeg -loglevel error -y -i "$W/demo-full.mp4" -vf scale=1280:720:flags=lanczos -c:v libx264 -preset veryslow -crf 28 \
	-tune animation -pix_fmt yuv420p -movflags +faststart -an "$OUT/demo.mp4"
ffmpeg -loglevel error -y -t 13 -i "$W/demo-full.mp4" -vf "fps=12,scale=800:-1:flags=lanczos" -c:v libwebp -lossless 0 \
	-q:v 45 -compression_level 6 -loop 0 -an "$OUT/hero.webp"
make -s -C "$ROOT/src/shell" clean
BIG=$(find "$OUT" -type f -size +2M)
[ -z "$BIG" ] || { echo "too big (max 2 MB): $BIG"; exit 1; }
ls -la "$OUT"
