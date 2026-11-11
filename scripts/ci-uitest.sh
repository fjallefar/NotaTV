#!/bin/sh
# A6 (audit 9/11): the user interface and every app run headless with AddressSanitizer and UBSan, and the run
# fails on the first memory error or undefined behaviour. Used by .github/workflows/test.yml before a release, and
# by hand: scripts/ci-uitest.sh [outdir]   (the screenshots and logs end up in outdir, default /tmp/notatv-uitest)
# It walks: the home screen, every app on it (open, a few keys, Back), every section of Settings, the App Store and
# the control center. Network calls from the apps are real (GitHub's runner is online); errors there are fine,
# a crash is not. Leaks are not checked (some memory is kept on purpose, see cfg_retire in util.c).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-/tmp/notatv-uitest}
SAN="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
mkdir -p "$OUT"

echo "== build with sanitizers"
make -s -C "$ROOT/src/shell" clean >/dev/null
make -s -C "$ROOT/src/shell" tvshell CFLAGS="$SAN" LDFLAGS="-fsanitize=address,undefined" || exit 1
make -s -C "$ROOT/src/apps" clean >/dev/null
make -s -C "$ROOT/src/apps" CFLAGS="$SAN" LDFLAGS="-fsanitize=address,undefined" || exit 1

# no helper scripts: keys in Settings must never run e.g. "power reboot" or "wifi" on the machine running the test
mkdir -p "$OUT/no-helpers"
export TVSHELL_HELPERS="$OUT/no-helpers"
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
fail=0
run() { # name script
	d="$OUT/$1"
	mkdir -p "$d/data"
	printf 'setup_done=1\n' > "$d/data/notatv.conf"
	TIMEOUT=${TIMEOUT:-90} "$ROOT/scripts/uitest.sh" "$d" "$2" >/dev/null 2>&1
	if grep -qE 'ERROR: AddressSanitizer|runtime error:|SUMMARY: (Address|UndefinedBehavior)Sanitizer' "$d/log.txt"; then
		echo "FAIL $1"
		grep -E -A30 'ERROR: AddressSanitizer|runtime error:' "$d/log.txt" | head -60
		fail=1
	else
		echo "ok   $1"
	fi
}

KEYS="k:down;w:10;k:right;w:10;k:right;w:10;k:down;w:10;k:ok;w:60;k:back;w:20;k:left;w:10;k:left;w:10;k:left;w:10;k:down;w:10;k:ok;w:40;k:back;w:20;k:back;w:20;k:back;w:20"
run home "w:90;s:home;k:right;w:10;k:down;w:10;k:up;w:10;k:up;w:20;k:ok;w:40;s:cc;k:back;w:20;q"
# every app on the home screen (rows of seven)
for i in $(seq 0 20); do
	nav="w:90"
	r=$((i / 7)) c=$((i % 7))
	while [ $r -gt 0 ]; do nav="$nav;k:down;w:5"; r=$((r - 1)); done
	while [ $c -gt 0 ]; do nav="$nav;k:right;w:5"; c=$((c - 1)); done
	run "app$i" "$nav;k:ok;w:240;s:open;$KEYS;s:after;k:home;w:30;q"
done
# every section of Settings (the last tile), each opened and walked
for s in $(seq 0 9); do
	nav="w:90;k:down;w:5;k:down;w:5;k:down;w:5"
	for _ in 1 2 3 4 5 6 7; do nav="$nav;k:right;w:3"; done
	nav="$nav;k:ok;w:40"
	k=$s; while [ $k -gt 0 ]; do nav="$nav;k:down;w:3"; k=$((k - 1)); done
	run "settings$s" "$nav;k:right;w:20;k:down;w:5;k:down;w:5;k:down;w:5;k:down;w:5;s:sec;k:back;w:10;k:back;w:20;q"
done
exit $fail
