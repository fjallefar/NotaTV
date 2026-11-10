#!/bin/sh
# Builds Plezy (pinned tag) for flutter-pi with NotaTV's changes: plezy/plezy.patch (TV mode, no Plezy-run services,
# NotaTV's player and look), the name "Peezy" in every text and as client name, our icon, and NotaTV's font.
# Output: tools/work/plezy/build/flutter-pi/<target>/ (app.so = the Dart code, flutter_assets, libflutter_engine.so).
set -eu
PLEZY_TAG=2.26.0
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
APP=$ROOT/src/apps/peezy
W=$ROOT/tools/work
SRC=$W/plezy
. "$ROOT/tools/env.sh"
[ -d "$SRC/.git" ] || git clone -q --depth 1 --branch $PLEZY_TAG https://github.com/edde746/plezy.git "$SRC"
git -C "$SRC" fetch -q --depth 1 origin tag $PLEZY_TAG 2>/dev/null || true
git -C "$SRC" checkout -q -f $PLEZY_TAG
git -C "$SRC" clean -qfdx -e build -e .dart_tool
git -C "$SRC" apply "$APP/plezy/plezy.patch"
# the name: every UI text, and what servers see as the client (Plex product, Jellyfin client)
python3 - "$SRC" <<'PY'
import glob, re, sys
root = sys.argv[1]
for f in glob.glob(root + '/lib/i18n/strings_*.g.dart'):
    s = open(f).read(); open(f, 'w').write(re.sub(r'\bPlezy\b', 'Peezy', s))
for f in ['lib/main.dart', 'lib/models/jellyfin/jellyfin_display_preferences.dart', 'lib/navigation/profile_session_screen.dart',
          'lib/models/plex/plex_config.dart', 'lib/services/jellyfin_auth_header.dart', 'lib/services/plex_discover_client.dart',
          'lib/services/jellyfin_client/parts/playback.dart', 'lib/services/plex_auth_service.dart', 'lib/services/plex_client.dart',
          'lib/watch_together/providers/watch_together_provider.dart']:
    p = root + '/' + f; s = open(p).read()
    t = re.sub(r"'Plezy( |')", lambda m: "'Peezy" + m.group(1), s)
    assert t != s, f
    open(p, 'w').write(t)
PY
# our icon instead of Plezy's logo (same file names, so no code changes), NotaTV's font
cp "$APP/plezy/assets/peezy.png" "$SRC/assets/plezy.png"
cp "$APP/plezy/assets/peezy_foreground.svg" "$SRC/assets/plezy_adaptive_foreground.svg"
mkdir -p "$SRC/assets/fonts"
cp "$ROOT"/src/shell/assets/Inter-Regular.ttf "$ROOT"/src/shell/assets/Inter-SemiBold.ttf "$ROOT"/src/shell/assets/Inter-Bold.ttf "$SRC/assets/fonts/"
cd "$SRC"
flutter pub get >/dev/null
# one generic arm64 engine for both Pi 4 and Pi 5
flutterpi_tool build --release --arch=arm64 --dart-define=NOTATV=true --tree-shake-icons
ls -d "$SRC"/build/flutter-pi/*/
