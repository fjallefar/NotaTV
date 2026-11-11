# Building and developing NotaTV

[Back to the README](README.md).

## Building

You need a Linux machine with the usual Buildroot dependencies (gcc, make, git, rsync, bc, cpio, unzip, wget, python3 …) and about 25 GB of free disk space. The first build takes a few hours.

```sh
git clone https://github.com/fjallefar/NotaTV.git
cd NotaTV
make               # -> notatv-rpi4.img + notatv-rpi4-update.tar.gz (same as make rpi4)
make shell         # run the user interface in a window on your PC
make menuconfig    # change the Buildroot configuration
```

The Makefile downloads Buildroot 2026.08.1 by itself, and Raspberry Pi's ffmpeg fork at the commit pinned in `external/board/rpi4/rpi-ffmpeg.commit` with the patches in `external/board/rpi4/rpi-ffmpeg-patches/`.

**Twitch** needs a Client ID from your own app at [dev.twitch.tv](https://dev.twitch.tv/console) ("Public" client type, device code flow enabled). Put it in `secrets/twitch-client-id` (not in git) before building.

## Making a release

Releases are built by GitHub Actions (`.github/workflows/release.yml`), either on GitHub's own runner or on a self-hosted runner (the build machine, label `notatv`). A tag push builds where the repository variable `BUILD_ON` says (`github` or `build-machine`; not set = the build machine). **Run workflow** in the Actions tab builds any existing tag on the runner you pick. GitHub's runner always does a full build (about 1.5 hours; `dl/` and `.ccache/` are kept in the Actions cache), the build machine keeps its build folder (see below).


1. Raise `VERSION` (e.g. `1.0.2`), write what is new under `## 1.0.2` in [CHANGELOG.md](CHANGELOG.md) (it becomes the release text; start it with the short summary the boxes show, see the top of the file), and push.
2. Tag it: `git tag v1.0.2 && git push origin v1.0.2`. The tag must match `VERSION`.
3. The runner publishes `notatv-rpi4.img.xz`, `notatv-rpi4-update.tar.gz` and its `.sha256`, and packs the apps (`scripts/apps-catalog.py`) into the pre-release `apps`, the App Store. Raise an app's `version` in its `manifest.json` when it changes, and write what is new in its `changes` (see [src/apps/README.md](src/apps/README.md)).
4. Only apps changed? Push a tag `apps-YYYY-MM-DD` instead (`.github/workflows/apps.yml`): no new system version is needed.

On the build machine the build folder is kept between releases, so normally only NotaTV's own packages are rebuilt (minutes). Changes to the Makefile, defconfig, packages, kernel fragments, patches, `config.txt`/`cmdline.txt` or the rpi-ffmpeg commit trigger a full build. After a kernel lift, `post-build.sh` removes the old `/lib/modules/<version>` (Buildroot keeps old files in the target folder). The tvshell package is copied from `src/shell` only once, so run `make clean-shell` before a local build that must include shell changes. The Twitch Client ID comes from the repository secret `TWITCH_CLIENT_ID`.

## Working on the code

Code and comments are in English, and user-facing text goes through `T("…")` and the language files. **Making an app:** see [src/apps/README.md](src/apps/README.md).

**Live updates on a running Pi:** turn on *Settings → Advanced → Developer mode (SSH)* and log in with `ssh root@<ip>` (the password is shown right below the setting). Copy a new `tvshell` to `/usr/bin/tvshell.new`, check that it is not empty, `mv` it into place and run `killall tvshell`; it restarts by itself. Before you replace a shared library, compare its dependencies (`readelf -d … | grep NEEDED`) with what is on the box.

**UI tests without a TV:** `scripts/uitest.sh <dir> "<script>"` runs the UI headless with the app packages from `src/apps/out` and saves screenshots. Example: `w:60;s:home;k:right;k:ok;w:30;s:app;q`.

**Sanitizer test (A6):** `scripts/ci-uitest.sh [outdir]` builds the UI and all apps with AddressSanitizer and UBSan and
walks the home screen, every app and every Settings section headless (about 2 minutes). It fails on the first memory
error or undefined behaviour. GitHub runs it on every push to main (`.github/workflows/test.yml`), and Release/Apps
wait for it. It rebuilds `src/shell` and `src/apps` with the sanitizers: run `make clean && make` there afterwards.

**Debug inbox:** `echo ok > /tmp/notatv-inbox/_key` sends a key (up/down/left/right/ok/back/home/menu/playpause), and `echo /tmp/x.bmp > /tmp/notatv-inbox/_shot` takes a screenshot.

**The look** (keep it the same everywhere):
- One left edge: 96 px from the screen edge (1920x1080 UI), also in the player OSD.
- Focus is a lift, not a frame: `ui_focus()` + `ui_focus_under()/ui_focus_over()`. Lists use a white pill that glides (`slide_to()`), dialogs and panels are dark glass (`draw_panel()`), and short messages are a pill at the top (`app_toast()`, `script-message notatv-toast` in the player).
- Choices for an item are a sheet (`choice_open()`), never a button bar at the bottom.
- Animations use springs (`spring_to()`) and stay short. On the Pi the GPU is the limit, so avoid extra full-screen layers.
- App icons are our own drawings (no logo files from the services): `scripts/gen-icons.py` renders them with `rsvg-convert`.

## Project layout

```
external/                       Buildroot BR2_EXTERNAL tree
  configs/                      notatv_rpi4_defconfig
  board/notatv/                 shared: rootfs overlay, post-build/post-image scripts
    rootfs-overlay/usr/libexec/notatv/   helper scripts (network, audio, CEC, Bluetooth, update …)
    rootfs-overlay/usr/share/notatv/mpv/ player OSD (Lua)
    rootfs-overlay/usr/share/notatv/www/ phone page
  board/rpi4/                   Pi: config.txt, kernel/mpv/ffmpeg patches, overclock, updater
  package/                      tvshell, tvmoon, libgamestream, yt-dlp (+ Node.js), uxplay, rpi-tools
src/shell/                      the user interface (C11 + SDL3)
  main.c         screen stack, main loop, inbox
  gfx.c          drawing, text, images, icons, HDR conversion
  input.c        evdev (keyboard, CEC, IR, gamepads), color keys, hold detection
  player.c       mpv control over IPC, frame rate matching
  widgets.c      on-screen keyboard, dialogs, sheets, lists
  apps.c         the app register: loads the app packages, install/uninstall
  ui_*.c         the system's screens: home, App Store, settings, setup guide, network monitor
  bt.c dlna.c mirror.c netdisc.c moon.c   Bluetooth, DLNA, AirPlay mirroring, server discovery, game streaming
  i18n.c subs.c tz.c quick.c   translations, OpenSubtitles, time zones, the control center
  assets/        fonts (Inter), app icons and language files
src/apps/                       the apps, one package per folder, downloaded from the App Store into /data/notatv/apps-pkg
src/moon/                       tvmoon: game streaming client (Moonlight protocol)
scripts/                        i18n check, UI tests, app catalog, app icons, time zone list
.github/workflows/              release.yml (v* tags) and apps.yml (apps-* tags)
```

## Adding a language

the source text is English, and the other languages are gettext `.po` files: [`src/shell/assets/lang/`](src/shell/assets/lang/) for the system, and `src/apps/<id>/lang/` for each app.
1. Run `python3 scripts/i18n-check.py --pot` to write the template `src/shell/assets/lang/notatv.pot`.
2. Copy it to `<code>.po` (e.g. `sv.po`) and fill in every `msgstr`. Do the same for each app: copy its `lang/da.po` to `lang/<code>.po` and translate it (e.g. with Poedit).
3. Add the language to `i18n_langs[]` in `src/shell/i18n.c` and in its copy `src/moon/i18n.c`.
4. Run `python3 scripts/i18n-check.py`. It reports missing texts and placeholders (`%s`, `%d`) that do not match.
