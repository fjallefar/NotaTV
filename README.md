# NotaTV

## Human written notes

I got tired of Google TV and Apple TV and figured a Raspberry Pi could make a good enough smart TV box, i didnt use any of the features those devices could provide. None of the existing projects fit what I wanted, so I built my own. Menus stay locked at 60 FPS, streaming is smooth, and the system is light enough that the Pi has no trouble running it. And it works. It's smooth, fast and stable.

The project leans heavily toward self-hosting. Next up are Tesla features through TeslaMate, smart home controls through Home Assistant, and more. I'll also keep working on bug fixes, performance, smoother animations and lots more. The list is long.

For now tho, 
On to the AI slop...

Cheers!

---

**A TV box operating system built from scratch, using only open source software.**

NotaTV is its own Linux distribution (Buildroot) with its own user interface, written in C with SDL3 and drawn straight to the screen via KMS/DRM. It turns a Raspberry Pi 4 into a TV box. It has no Android, no Kodi and no closed-source apps.

- **Live TV / IPTV** with program guide (M3U or Xtream Codes)
- **Plex** and **Jellyfin** clients with direct play (4K HDR)
- **YouTube** (yt-dlp) and **Twitch** with chat
- **Seerr**: find and request movies and shows (Overseerr/Jellyseerr)
- **Music** from your phone via **AirPlay** and **Bluetooth**
- **Game streaming** from a PC with Sunshine (Moonlight protocol, ~11 ms on the box)
- **Casting**: DLNA, Plex cast, YouTube cast, Jellyfin "Play On", AirPlay screen mirroring
- **Media player** for USB drives and SMB/NFS shares
- **HDMI-CEC**: your TV remote controls the box
- A **phone page** for typing, remote control and setup
- **English and Danish** user interface

Platform: **Raspberry Pi 4**.

---

## Contents

- [Open source only](#open-source-only)
- [Installation](#installation)
- [Using NotaTV](#using-notatv)
- [Apps](#apps)
- [Casting from your phone](#casting-from-your-phone)
- [Picture and sound](#picture-and-sound)
- [Updating](#updating)
- [Languages](#languages)
- [Building from source](#building-from-source)
- [Development](#development)
- [Project layout](#project-layout)
- [Limitations](#limitations)
- [License](#license)

---

## Open source only

Everything that runs in Linux on the box is open source, so you can always check what data the box collects and sends. Closed software such as Spotify's SDK, Widevine or Google Cast is not used.

**Network monitor** (*Settings → Network → Network monitor*) shows live which programs on the box have connections and where they go: the internet (orange), the local network (blue), and ports the box listens on (gray).

The box only contacts outside services when you use a feature:

| Service | When |
|---|---|
| plex.tv and your Plex servers | Plex app |
| Your Jellyfin server | Jellyfin app, "Play On" |
| YouTube (via yt-dlp) | YouTube app |
| api.twitch.tv, id.twitch.tv, irc.chat.twitch.tv | Twitch app (login, lists, anonymous chat) |
| Your Seerr server, TMDB images | Seerr app |
| netflix.com (public top 10 data file) | Seerr → Netflix top 10 |
| Your IPTV provider | Live TV |
| musicbrainz.org, coverartarchive.org | Cover art for Bluetooth music (can be turned off) |
| api.github.com, github.com | Settings → System → Update NotaTV |
| pool.ntp.org | Setting the clock at startup |

---

## Installation

### Raspberry Pi 4

1. Download `notatv-rpi4.img.xz` from the [latest release](https://github.com/fjallefar/NotaTV/releases/latest).
2. Write it to an SD card with Raspberry Pi Imager ("Use custom") or balenaEtcher.
   Or on Linux with `dd` (replace `/dev/sdX` with your SD card - **everything on it is erased**):
   ```sh
   xz -dc notatv-rpi4.img.xz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
   ```
3. Connect the TV to **HDMI 0**, the port next to the USB-C power input.

On first boot the data partition grows to fill the SD card.

### First start

A setup guide walks you through five short steps:
1. **language**
2. **network** (cable or Wi-Fi)
3. **country and region** for the clock (daylight saving time follows the region)
4. the box's **name** (NotaTV, Living room, Bedroom … or your own)
5. which **apps** to install

At the end it shows a summary with the phone page address and code. Everything is chosen on the box and nothing is looked up online. You can run the guide again under *Settings → System*.

---

## Using NotaTV

### Remote control

The TV's own remote works over HDMI-CEC. Many TVs (LG in particular) only pass on the arrows, OK, Back, CH+/CH- and the four color keys. That is why the color keys have the same meaning everywhere:

| Key | Action |
|---|---|
| Arrows / OK / Back | Navigate / select / back |
| **Hold OK** | Extra options (YouTube: info, Live TV: channel menu) |
| **Hold Back** | Go to the home screen |
| 🔴 Red | **Menu, always.** Actions such as quit, refresh, aspect ratio and technical info are found here |
| 🟢 Green | Info |
| 🟡 Yellow | Act on the selected item (favorite, remove, show password, Twitch chat on/off) |
| 🔵 Blue | Search (YouTube, Plex, Seerr, Jellyfin, Twitch) |
| CH+ / CH- | Page up/down in lists (Live TV: change channel) |

The bar at the bottom of the screen shows the same actions as buttons, in the order OK · Green · Yellow · Blue · Red. Press ↓ from the last row to reach the buttons. This means you can do everything with only the arrows and OK.

**During playback:**
- OK pauses and resumes.
- ←/→ seek, and seeking speeds up while you hold the key, from 10 s up to 10 min per step.
- ↓ or 🔴 opens the player menu: subtitles, audio, quality, speed, subtitle search and aspect ratio.
- 🟢 shows info, and Back stops.
- In Live TV, ↑/↓ or CH± changes channel.

**Keyboard (for development):** F2-F5 are the color keys, Esc is Back and Enter is OK.

### Phone page

Open `http://<box IP>` on your phone. From there you can:
- use the remote control
- type straight into a text field on the TV
- set up IPTV, Xtream Codes, YouTube and network drives

The page is protected:
- It is locked with a code. You set the code in the setup guide and can change it under *Settings → Network → Phone page code*.
- It only accepts POST requests of at most 512 KB.
- 10 wrong codes in 10 minutes lock the page.

Keys and text work right away. **Playlists, logins, network drives, names and YouTube accounts are only saved after you approve them on the TV.**

---

## Apps

Everything on the home screen is an app, and NotaTV comes without any: you pick them in the setup guide, and they are downloaded from the **App Store**. The App Store is the GitHub release `apps` of this repository, with one package per app and a catalog (version, SHA256). A package is only installed when its SHA256 matches the catalog.

In the App Store on the home screen you can install more apps, update apps (*Update* shows when the catalog has a newer version) and uninstall them. Uninstalling an app deletes everything it has saved on the box, including your sign-in, and stops its background services (e.g. Plex cast). Each app's settings are under *Settings → Apps*. Boxes updated from NotaTV 1.0 download the apps they had by themselves.

| App | Details |
|---|---|
| **Live TV** | M3U playlist or Xtream Codes login (live channels only). Channels are grouped by country. Favorites can be sorted. Channel numbers and now/next information come from XMLTV or Xtream's short EPG. |
| **Plex** | Sign in via `plex.tv/link`. Supports several servers, hubs, libraries, "Continue watching" and direct play. Search covers all servers. The home screen has two styles: *hero* or *focus*. |
| **Jellyfin** | Server address, username and password. Libraries, "Continue watching" and "Next episode". Always plays the original file (no server transcoding), with the audio and subtitles chosen for your Jellyfin user. Playback progress is reported to the server, and the server sees the box under its own name. |
| **YouTube** | yt-dlp with Node.js and mpv. Videos are resolved while they are in focus, so playback starts almost instantly. Optional account via cookies from the phone page. Subscriptions can be imported from Google Takeout. |
| **Twitch** | Sign in with a code or QR code at `twitch.tv/activate`. Following, Popular, Categories and Search. Chat is read anonymously and shown as a sidebar. |
| **Seerr** | Find and request movies and shows (Overseerr/Jellyseerr) with Plex login. Season picker, trailers, "More like this" and Netflix top 10 for your country. |
| **Music** | Receives music from your phone over **AirPlay** (iPhone/iPad/Mac) or **Bluetooth** (any phone). "Now playing" shows cover art, title and progress. |
| **Games** | Game streaming from a PC running Sunshine with our own client `tvmoon`. HEVC is decoded in hardware straight to the screen, at 1080p 120 Hz. |
| **Media** | USB drives are mounted automatically. SMB/NFS network drives are supported. Plays movies, music and pictures. |

**Find servers on the network** (Plex and Jellyfin) only searches the local network: Plex GDM, Jellyfin UDP 7359 and a scan of your own subnet.

---

## Casting from your phone

Google Cast/Chromecast cannot be built as open source, because phones require a Google-signed certificate in the receiver. NotaTV offers these instead:

| Method | From |
|---|---|
| **AirPlay** audio (shairport-sync) | iPhone, iPad, Mac: any app, including Spotify |
| **Bluetooth** audio (BlueZ + bluez-alsa) | Any phone: any app, including Spotify |
| **DLNA** receiver (libupnp) | BubbleUPnP, "Cast to Device" in Windows, VLC … |
| **Jellyfin "Play On"** | The Jellyfin app |
| **Plex cast** (GDM + Plex Companion) | The Plex app's cast button |
| **YouTube cast** (DIAL + YouTube lounge) | The YouTube app: Cast, or "Link with TV code" |
| **AirPlay screen mirroring** (UxPlay) | iPhone, iPad, Mac (shown as "NotaTV (screen)") |

YouTube cast goes through YouTube's servers because the protocol requires it. Plex cast and DLNA stay on the local network.

**Bluetooth pairing:**
- *Settings → Bluetooth → Pair a phone* makes the TV visible for 3 minutes.
- The phone and the TV show the same code, and you confirm it on the TV. Nobody can pair without that confirmation.
- By default the TV is only visible while Music is open. Paired controllers and keyboards can always connect.

---

## Picture and sound

- **HDR10** output. On the Pi it uses 12-bit YCbCr 4:2:2 and BT.2020.
- **Frame rate matching:** the TV switches to the video's frame rate (23.976/24/25/50/59.94/60 Hz).
- The box reads the display's EDID. On a display without HDR the HDR settings (also for game streaming) are locked and HDR is never sent. Surround passthrough only uses the audio formats the TV or receiver lists (AC-3, E-AC-3, DTS, DTS-HD and TrueHD are checked one by one).
- **Always HDR** (Pi) keeps the TV in HDR all the time. The menus are drawn in PQ and SDR video is converted on the GPU.
- **Audio:** HDMI is chosen automatically.
  - Dolby/DTS and TrueHD/DTS-HD can be passed through, and lip sync can be adjusted.
  - For surround over ARC (not eARC), turn on *Surround passthrough* and turn off *TrueHD / DTS-HD*. Audio that cannot go over ARC is re-encoded to Dolby Digital 5.1.
- **Player OSD:** loading, buffering and seek indicators, an info bar, and a pause screen with poster.

---

## Updating

*Settings → System → Update NotaTV* downloads the newest release from GitHub, checks its SHA256 and installs it. Your settings and logins on `/data` are kept.
- Nothing is changed until the whole package is downloaded and checked. Only files that differ are written, each with a single rename, so a power cut leaves either the old or the new version (an interrupted update is finished at the next boot).
- **Update reminders:** the box looks for a new release on GitHub twice a day and shows a reminder on the home screen at most every other day (*Update now*, *Later* or *Don't remind me*). Turn it off under *Settings → System → Update reminders*; then the box never contacts GitHub by itself.
- *Undo last update* (same menu) puts the previous version back. This also happens by itself if the user interface keeps crashing right after an update.
- The repository is set in `/etc/notatv-update-repo` and can be overridden with the config key `update_repo`.
- The release must include `notatv-rpi4-update.tar.gz` and `notatv-rpi4-update.tar.gz.sha256`. `make rpi4` creates both.

---

## Languages

The user interface is available in **English** (the default) and **Danish**. Change it under *Settings → System → Language* or in the first step of the setup guide.

- **The source text is English.** Other languages are standard gettext `.po` files: [`src/shell/assets/lang/`](src/shell/assets/lang/) for the system, and `src/apps/<id>/lang/` for each app (an app brings its own translations).
- **The system file covers** the UI, the game streaming client, the player OSD, the phone page and the helper scripts' messages.
- **The rest follows the language too:** the default subtitle and audio languages, Seerr's metadata language, the keyboard layout and the country list.

**Adding a language:**
1. Run `python3 scripts/i18n-check.py --pot` to write the template `src/shell/assets/lang/notatv.pot`.
2. Copy it to `<code>.po` (e.g. `sv.po`) and fill in every `msgstr`. Do the same for each app: copy its `lang/da.po` to `lang/<code>.po` and translate it. Any editor that handles `.po` files works, e.g. Poedit.
3. Add the language to `i18n_langs[]` in `src/shell/i18n.c` and in its copy `src/moon/i18n.c`.
4. Run `python3 scripts/i18n-check.py`. It reports missing texts and placeholders (`%s`, `%d`) that do not match.

---

## Building from source

You need a Linux machine with the usual Buildroot dependencies (gcc, make, git, rsync, bc, cpio, unzip, wget, python3 …) and about 25 GB of free disk space. The first build takes a few hours.

```sh
git clone https://github.com/fjallefar/NotaTV.git
cd NotaTV
make         # -> notatv-rpi4.img + notatv-rpi4-update.tar.gz (same as make rpi4)
```

The Makefile downloads Buildroot 2026.08.1 automatically. `make rpi4` also fetches Raspberry Pi's ffmpeg fork at the commit pinned in `external/board/rpi4/rpi-ffmpeg.commit` and applies `external/board/rpi4/rpi-ffmpeg-patches/`.

**Twitch** needs a Client ID from your own app at [dev.twitch.tv](https://dev.twitch.tv/console). Use the "Public" client type and enable device code flow. Put the ID in `secrets/twitch-client-id`, which is not in git, before building. Without it the Twitch app cannot sign in.

Other targets:

```sh
make shell             # run the user interface in a window on your PC
make menuconfig        # change the Buildroot configuration
```

### Making a release

Releases are built by GitHub Actions (`.github/workflows/release.yml`) on a self-hosted runner with the label `notatv`, so nobody has to build by hand:

1. Raise the version in `VERSION` (e.g. `1.0.2`) and push.
2. Tag it: `git tag v1.0.2 && git push origin v1.0.2`. The tag must match `VERSION`.
3. The runner builds in `/opt/notatv` and publishes a release with `notatv-rpi4.img.xz`, `notatv-rpi4-update.tar.gz` and its `.sha256`. Boxes then find it under *Update NotaTV*.
4. The same run packs the apps (`scripts/apps-catalog.py`) and uploads them, with the catalog `apps-api<N>.json`, to the pre-release `apps` (the App Store). Files with the same name are replaced. Raise an app's `version` in its `manifest.json` when it changes, so boxes are offered the update.

The build folder is kept between releases. Normally only NotaTV's own packages are rebuilt, which takes minutes. Changes to the Makefile, defconfig, packages, kernel fragments, patches or the rpi-ffmpeg commit trigger a full build. The Twitch Client ID comes from the repository secret `TWITCH_CLIENT_ID`.

---

## Development

Code and comments are written in English. User-facing text goes through `T("…")` and the language files.

**Live updates on a running Pi:**
1. Turn on *Settings → System → Developer mode (SSH)*, then log in with `ssh root@<ip>`. The password is `notatv`.
2. Build with `make rpi4` and copy `output-rpi4/target/usr/bin/tvshell` to `/usr/bin/tvshell.new` on the box.
3. Check that the file is not empty, `mv` it into place and run `killall tvshell`. It restarts by itself.
4. Before you replace a shared library, compare its dependencies (`readelf -d … | grep NEEDED`) with what is on the box.

**UI tests without a TV:**
`scripts/uitest.sh <dir> "<script>"` runs the UI headless and saves screenshots. Example script: `w:60;s:home;k:right;k:ok;w:30;s:app;q`.
Build the app packages first (`make -C src/apps`) and run with `TVSHELL_APPS=$PWD/src/apps/out` to include them.

**Making an app:** see [src/apps/README.md](src/apps/README.md).

**Getting the logs without SSH:** *Settings → System → Get the logs* shows the box's address and code. Open it on a phone or computer and press *Download logs* at the bottom of the page: one `.tar.gz` with all logs, system info and the settings, with passwords, tokens and logins removed. Nothing is sent anywhere by itself. For more detail, turn on *Settings → System → Troubleshooting log* first: every network request is logged, and the logs of the user interface and playback are kept in `/data/notatv/logs` (also across restarts) until you turn it off.

**Logs on the box** (readable by root only; tokens and passwords are masked):

| File | Contents |
|---|---|
| `/tmp/tvshell.log` | User interface (including Bluetooth, DLNA, Jellyfin, Plex and YouTube cast) |
| `/tmp/mpv.log` | Playback (the previous playback in `mpv.log.1`) |
| `/tmp/tvmoon.log` | Game streaming, with statistics every second |
| `/tmp/uxplay.log` | AirPlay screen mirroring |
| `/tmp/shairport-sync.log`, `/tmp/bluealsa.log` | AirPlay audio, Bluetooth audio |
| `/tmp/notatv.log` | Helper scripts |
| `/var/log/messages` | bluetoothd, dbus, avahi |
| `tvmon` (command) | Live temperature, clocks, voltage, throttling and CPU |

**Debug inbox** (read by tvshell):
- `echo ok > /tmp/notatv-inbox/_key` sends a key (up/down/left/right/ok/back/home/menu/playpause).
- `echo /tmp/x.bmp > /tmp/notatv-inbox/_shot` takes a screenshot.

---

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
  i18n.c         translations (.po loader)
  gfx.c          drawing, text, images, icons, HDR conversion
  input.c        evdev (keyboard, CEC, IR, gamepads), color keys, hold detection
  player.c       mpv control over IPC, frame rate matching
  widgets.c      on-screen keyboard, dialogs, lists
  apps.c         the app register: loads the app packages, install/uninstall
  ui_*.c         the system's screens: home, App Store, settings, setup guide, network monitor
  bt.c dlna.c mirror.c netdisc.c moon.c   Bluetooth, DLNA, AirPlay mirroring, server discovery, game streaming
  subs.c         OpenSubtitles (shared by the apps' players)
  tz.c           time zones (tzlist.h is generated by scripts/gen-tzlist.py)
  assets/        fonts (Inter) and language files (lang/*.po)
src/apps/                       the apps, one package per folder (iptv, plex, jellyfin, youtube, twitch, seerr,
                                music, games, media), installed in /usr/lib/notatv/apps
src/moon/                       tvmoon: game streaming client (Moonlight protocol)
scripts/                        i18n check, UI tests, time zone list generator
.github/workflows/release.yml   builds and publishes a release when a v* tag is pushed
```

---

## Limitations

- **Netflix, Disney+, HBO Max etc. are not supported.** They require Google's Widevine DRM module, which is closed source. The software version (Widevine L3) can technically run on a Pi, but only up to about 720p, and including it would break the open source principle. Full HD/4K requires Widevine L1 on certified hardware.
- **Spotify has no app.** The open source client librespot is broken for many accounts and the official SDK is closed. Play Spotify via AirPlay or Bluetooth instead.
- **Raspberry Pi 4 hardware:**
  - It decodes HEVC (4K, 10-bit) and H.264 (up to 1080p) in hardware, but not VP9 or AV1. That is why YouTube prefers H.264 up to 1080p.
  - 4K60 HEVC peaks at about 42 fps.
  - Dolby Vision plays as HDR10.
- **TV without HDR (or HDR turned off):** HDR video is tone-mapped to SDR on the Pi's GPU with the simplest curve (`clip`), bright enough to look like an SDR version, so the brightest highlights are clipped. 4K is drawn at 900p and scaled up by the display hardware to keep a full 24 fps. For Plex, the server can convert HDR instead (Apps → Plex; 1080p, stereo sound for now).
- **Always HDR:** SDR video at 50/60 fps reaches only about 30 fps in this mode.
- Keyboard, mouse and controller input during game streaming is not finished yet.

---

## License

NotaTV's own code is open source. The licenses declared in the Buildroot packages are:
- the user interface (`src/shell`, tvshell) and `rpi-tools`: **MIT**
- the game streaming client (`src/moon`, tvmoon): **GPL-3.0-or-later**, because it links to libgamestream (GPL-3.0+)

Third-party components keep their own licenses. Examples are the Inter font (SIL OFL, `src/shell/assets/Inter-LICENSE.txt`), the time zone names from iso-codes (LGPL-2.1) and tzdata (public domain).
