# NotaTV

## Human written notes

I got tired of Google TV and Apple TV and figured a Raspberry Pi could make a good enough smart TV box, i didnt use any of the features those devices could provide. None of the existing projects fit what I wanted, so I built my own. Menus stay locked at 60 FPS, streaming is smooth, and the system is light enough that the Pi has no trouble running it. And it works. It's smooth, fast and stable.

The project leans heavily toward self-hosting. Next up are Tesla features through TeslaMate, smart home controls through Home Assistant, and more. I'll also keep working on bug fixes, performance, smoother animations and lots more. The list is long.

For now tho, 
On to the AI slop...

Cheers!

---

**A TV box operating system built from scratch, using only open source software.**

NotaTV is its own Linux distribution (Buildroot) with its own user interface, written in C with SDL3 and drawn straight to the screen via KMS/DRM. It turns a Raspberry Pi 4 or Pi 5 into a TV box. It has no Android, no Kodi and no closed-source apps.

- **Live TV / IPTV** with a TV guide (M3U or Xtream Codes)
- **Plex** and **Jellyfin** with direct play (4K HDR)
- **YouTube** and **Twitch** with chat
- **DRTV**, and **TV 2 Play** as an opt-in (it needs a closed DRM module)
- **Seerr** and **Flimmer**: find and request movies and series
- **Music** from your phone via **AirPlay** and **Bluetooth**, and **Radio** from the whole world
- **Game streaming** from a PC with Sunshine (Moonlight protocol, ~11 ms on the box)
- **Weather**, **Electricity price** and **Tesla** (from your own TeslaMate)
- **Casting**: DLNA, YouTube cast, Jellyfin "Play On", AirPlay screen mirroring
- **Media player** for USB drives, SMB and NFS shares
- **HDMI-CEC**: your TV remote controls the box
- A **phone page** for typing, remote control and setup
- **English and Danish** user interface

Platform: **Raspberry Pi 4 and Pi 5** (one image for both; the Pi 5 support is new and not fully tested yet).

---

## Contents

- [Installation](#installation)
- [Using NotaTV](#using-notatv): remote control, phone page
- [Apps](#apps) (and [self-hosting](SELF-HOSTING.md) the servers they use)
- [Casting from your phone](#casting-from-your-phone)
- [Picture and sound](#picture-and-sound)
- [Updating](#updating)
- [Troubleshooting](#troubleshooting)
- [Open source and privacy](#open-source-and-privacy)
- [Languages](#languages)
- [Limitations](#limitations)
- [Building and development](#building-and-development)
- [License](#license)

---

## Installation

1. Download `notatv-rpi4.img.xz` (the same image works on a Pi 5) from the [latest release](https://github.com/fjallefar/NotaTV/releases/latest).
2. Write it to an SD card with Raspberry Pi Imager ("Use custom") or balenaEtcher.
   Or on Linux with `dd` (replace `/dev/sdX` with your SD card - **everything on it is erased**):
   ```sh
   xz -dc notatv-rpi4.img.xz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
   ```
3. Connect the TV to **HDMI 0**, the port next to the USB-C power input.
   A Pi 5 needs the official 5 V/5 A power supply and active cooling (e.g. the Active Cooler).

On first boot the data partition grows to fill the SD card. While the box starts, the TV shows the NotaTV name with three pulsing dots until the menus appear.

**First start:** a setup guide walks you through five short steps:
1. **language**
2. **network** (cable or Wi-Fi - with both, the cable is used while it is plugged in, and Wi-Fi takes over when you pull it out)
3. **country and region** for the clock (daylight saving time follows the region)
4. the box's **name** (NotaTV, Living room, Bedroom … or your own)
5. which **apps** to install

At the end it shows a summary with the phone page address and code. Everything is chosen on the box and nothing is looked up online. You can run the guide again under *Settings → System*.

---

## Using NotaTV

### Remote control

The TV's own remote works over HDMI-CEC. Everything can be done with **the arrows, OK and Back**, so a remote without color keys, a controller or the phone page is enough.

- **OK on something you can watch or hear** (a channel, a video, a station, an episode) opens a sheet with its choices. The first choice plays it, so **OK, OK plays**.
- **OK on anything else** (a folder, a category, a menu item, a button) acts at once.
- **Movies and series** in Plex, Jellyfin, Seerr and Flimmer open their own page with the buttons on it.
- **← (Left)** from the content opens the app's menu: Search, the app's lists and its Settings. In Live TV it is ← from the groups.
- **Hold Back** goes to the home screen.
- **CH+ / CH-** pages up and down in lists (in Live TV it changes channel).

| Where | OK opens |
|---|---|
| Live TV, a channel | *Watch channel*, favorite, program schedule, move favorite. In the TV guide a coming program also offers *Remind me* |
| YouTube, a video | *Play*, *Info and comments*, *Go to channel* |
| Radio, a station | *Listen* (or *Stop*), *Favourite* |
| Plex and Jellyfin, an episode | *Play*, mark as watched or unwatched, remove from *Continue watching* |
| Plex and Jellyfin, a season | *Episodes*, mark the season as watched |
| Flimmer, a request | *Open*, and *Withdraw request* while it is waiting |

**The color keys are shortcuts** to things that are also on the screen: 🔴 Red opens the menu, 🟢 Green info, 🟡 Yellow acts on the selected item (e.g. favorite), 🔵 Blue search. On a PC keyboard, F2-F5 are the color keys, Esc is Back and Enter is OK.

**During playback:**
- OK pauses and resumes, and Back stops.
- ←/→ seek, and seeking speeds up while you hold the key, from 10 s up to 10 min per step.
- ↓ opens the player menu: the app's own choices at the top (YouTube: *Info and comments*, Live TV: favorite), then subtitles, audio, quality, speed, subtitle search and aspect ratio (Twitch: chat on/off).
- In Live TV, ↑/↓ or CH± changes channel, and → opens the menu.
- Game streaming is the exception: the arrows, OK and Back go to the game, so 🔴 Red opens the menu (a controller quits with Select + Start + LB + RB).

**Home screen:** *Edit home screen* (↑ from the first row of apps) moves the apps around.

**Quick panel:** 🔴 Red on the home screen (or the Power key) opens a panel from the right with the time, the box's name and address, a **sleep timer** (off, 15, 30, 60, 90 or 120 minutes; a message one minute before, then the box goes to sleep), shortcuts to Bluetooth, Picture and sound, Network and all settings, and Sleep, Restart and Power off.

### Phone page

Open `http://<box IP>` on your phone. From there you can:
- use the remote control
- type straight into a text field on the TV
- set up IPTV, Xtream Codes, YouTube and network drives

Keys and text work right away. **Playlists, logins, network drives, names and YouTube accounts are only saved after you approve them on the TV.**

The page is locked with a code. You set it in the setup guide and can change it under *Settings → Network → Phone page code*. 10 wrong codes in 10 minutes lock the page, and it only accepts POST requests of at most 512 KB. The page uses plain HTTP inside your home network, so the code is sent unencrypted: it keeps others on the network out, but do not reuse a password you use elsewhere.

---

## Apps

Everything on the home screen is an app, and NotaTV comes without any: you pick them in the setup guide, and they are downloaded from the **App Store** on the home screen. There you can install more apps, update them (*Update all* when several have updates, with a short card about what is new) and uninstall them. Uninstalling an app deletes everything it has saved on the box, including your sign-in. Each app's settings are in the app's own menu and under *Settings → Apps*.

| App | Details |
|---|---|
| **Live TV** | M3U playlist or Xtream Codes login (live channels only). Channels are grouped by country, and favorites can be sorted. The **TV guide** shows your favorites on a timeline (left/right moves between programs). Channel numbers and now/next come from XMLTV or Xtream's short EPG. **Reminders:** a minute before a program starts, the TV offers to switch (after the video, if something is playing). |
| **Plex** | Sign in via `plex.tv/link`. Several servers, hubs, libraries, "Continue watching" and direct play. Search covers all servers. Two home styles: *hero* or *focus*. |
| **Jellyfin** | Server address, username and password. Libraries, a series page with seasons and episodes, "Continue watching" and "Next episode". Always plays the original file (no server transcoding), with the audio and subtitles chosen for your Jellyfin user. Progress is reported to the server, which sees the box under its own name. |
| **YouTube** | yt-dlp with Node.js and mpv. Videos are resolved while they are in focus, so playback starts almost at once. Up to 1080p on a Raspberry Pi (1440p and 4K are greyed out: YouTube only has them as VP9/AV1). Optional account via cookies from the phone page; subscriptions can be imported from Google Takeout. |
| **Twitch** | Sign in with a code or QR code at `twitch.tv/activate`. Following, Popular, Categories and Search. Chat is read anonymously and shown as a sidebar. Quality from 1080p to 360p. |
| **DRTV** | DR's live channels (DR1, DR2, DR Ramasjang, DRTV Ekstra, TVA Live) with now and next, the front page, categories, A-Z, kids and search. Series with seasons, resume, skip the end credits and next episode. Free and without DRM. Signing in with a DR account adds DR's own *Continue watching* and *My list*. Programmes marked *Denmark only* need a Danish internet connection. |
| **TV 2 Play** | Opt-in (closed software, see [Open source and privacy](#open-source-and-privacy)). Sign in with a code on the TV. Front page, channel list, categories, search, series and episodes, continue watching and subtitles. Needs your own TV 2 Play subscription. Live playback is not supported yet. |
| **Seerr** | Find and request movies and shows (Seerr, Overseerr, Jellyseerr) with your Plex login. Season picker, trailers, "More like this" and Netflix top 10 for your country. |
| **Flimmer** | Find and request movies and series on a Flimmertv server (requests, movies and series in one server). Finds the server on the network (port 7676); sign in with your Flimmertv email and password. Discover, search, a season picker, trailers, "More like this", and *My requests*, where a request still waiting can be withdrawn. |
| **Music** | Receives music from your phone over **AirPlay** (iPhone/iPad/Mac) or **Bluetooth** (any phone). "Now playing" shows cover art, title and progress. |
| **Radio** | Stations from radio-browser.info (free, no login), from your own country or any other. Popular, genres, search, recently played and favourites (the first nine get a number you press on the remote). Keeps playing while you use the rest of the box, with *Now playing* in the top bar; it stops by itself when a video or game starts. |
| **Games** | Game streaming from a PC running Sunshine with our own client `tvmoon`: HEVC decoded in hardware straight to the screen, at 1080p 120 Hz. To pair, type the PIN shown on the TV on `https://localhost:47990` on the PC (Sunshine rejects it on the PC's IP address); Back cancels. |
| **Weather** | Now, the next 24 hours and 7 days from Open-Meteo (open data, no account). The temperature is shown in the home screen's top bar. The town comes from the time zone until you choose one at the top of the app. |
| **Electricity price** | The Danish spot price with VAT hour by hour, today and tomorrow (from about 13:00), from Energi Data Service. The price now in green, yellow or red, the cheapest hours and a chart. The buttons at the top choose West or East Denmark, your grid company (for the full price with tariffs and tax) and your supplier's surcharge. |
| **Tesla** | Reads your own TeslaMate over its MQTT server (port 1883), read only: nothing is sent to the car and nothing is saved on disk. Battery, range, charging, climate, tyres, locks and doors, the car seen from above, where it is, route with arrival time, model and software. The side menu chooses the car, turns each part on or off, and puts up to 3 cars small at the top of the home screen. |
| **Media** | USB drives are mounted automatically. SMB network drives (with a login or as a guest) and NFS drives (`nfs://server/path`, NFSv4 or v3). Plays movies, music and pictures. |

**Host it yourself:** [SELF-HOSTING.md](SELF-HOSTING.md) lists, per app, the server software it talks to (Jellyfin, Seerr, Sunshine, TeslaMate, Live TV playlists …) with links to each project.

**Finding servers** (Plex, Jellyfin, Seerr, Flimmer, TeslaMate) only searches your local network: Plex GDM, Jellyfin UDP 7359, and a scan of your own subnet for the other servers' ports.

**Subtitles from OpenSubtitles** (Plex and Jellyfin): choose *Search subtitles* in the player menu. It needs a free API key: create an account on opensubtitles.com, make a key under *Consumers* and enter it in *Settings → Picture and sound*, together with your OpenSubtitles username and password (OpenSubtitles may refuse downloads without a login). Movies are found by their TMDB id, episodes by show, season and episode; the subtitle you pick is turned on at once.

---

## Casting from your phone

Google Cast/Chromecast cannot be built as open source, because phones require a Google-signed certificate in the receiver. NotaTV offers these instead:

| Method | From |
|---|---|
| **AirPlay** audio (shairport-sync) | iPhone, iPad, Mac: any app, including Spotify |
| **Bluetooth** audio (BlueZ + bluez-alsa) | Any phone: any app, including Spotify |
| **DLNA** receiver (libupnp) | BubbleUPnP, "Cast to Device" in Windows, VLC … |
| **Jellyfin "Play On"** | The Jellyfin app |
| **YouTube cast** (DIAL + YouTube lounge) | The YouTube app: Cast, or "Link with TV code" |
| **AirPlay screen mirroring** (UxPlay) | iPhone, iPad, Mac (shown as "NotaTV (screen)") |

- **YouTube cast** goes through YouTube's servers because the protocol requires it. **DLNA** stays on the local network and only plays addresses there (http, https, rtsp).
- The first time a phone or PC wants to play through DLNA or YouTube cast, the TV asks whether it may. **AirPlay screen mirroring** asks for the box's 4-digit code the first time (shown on the TV and under *Settings → Cast and sharing*).
- **Firewall:** the box's services (phone page, AirPlay, cast, DLNA, SSH) only answer devices on the local network.

**Bluetooth pairing:** *Settings → Bluetooth → Pair a phone* makes the TV visible for 3 minutes. The phone and the TV show the same code, and you confirm it on the TV, so nobody can pair without that confirmation. Otherwise the TV is only visible while Music is open. *Pair a controller or keyboard* finds controllers, keyboards and mice in pairing mode; a keyboard that needs a code shows it on the TV.

---

## Picture and sound

- **HDR10** output, on the Pi with 12-bit YCbCr 4:2:2 and BT.2020.
- **Frame rate matching:** the TV switches to the video's frame rate (23.976/24/25/50/59.94/60 Hz).
- The box reads the display's EDID. On a display without HDR the HDR settings (also for game streaming) are locked and HDR is never sent. Surround passthrough only uses the audio formats the TV or receiver lists.
- **Always HDR** keeps the TV in HDR all the time: the menus are drawn in PQ and SDR video is converted on the GPU.
- **Audio** goes to HDMI by itself. Dolby/DTS and TrueHD/DTS-HD can be passed through, and lip sync can be adjusted. For surround over ARC (not eARC), turn on *Surround passthrough* and turn off *TrueHD / DTS-HD*: audio that cannot go over ARC is re-encoded to Dolby Digital 5.1.

---

## Updating

*Settings → System → Update NotaTV* downloads the newest release from GitHub, checks its SHA256 and installs it. Your settings and sign-ins are kept.

- **Safe against power cuts:** nothing is changed until the whole package is downloaded and checked, and each file is written with a single rename, so the box has either the old or the new version. An interrupted update is finished at the next boot. The kernel is kept as two complete sets on the boot partition, and an update switches between them with one small write.
- **Undo last update** (same menu) puts the previous version back. This also happens by itself if the user interface keeps crashing right after an update.
- **Update reminders:** the box looks for a new release twice a day and shows a reminder on the home screen at most every other day. Turn it off under *Settings → System → Update reminders*; then the box never contacts GitHub by itself.
- The update card shows a short summary of what is new (from [CHANGELOG.md](CHANGELOG.md)).

---

## Troubleshooting

**Get the logs without SSH:** *Settings → System → Get the logs* shows the box's address and code. Open it on a phone or computer and press *Download logs*: one `.tar.gz` with all logs, system info and settings, with passwords, tokens and logins removed. Nothing is sent anywhere by itself. For more detail, turn on *Settings → System → Troubleshooting log* first: every network request is logged, and the logs are kept across restarts until you turn it off.

**A box stuck on an update** ("Restart the box to finish the (previous) update" again and again): the updater in 1.0.0-1.0.2 could not finish an update that changes the kernel. Turn on *Settings → System → Developer mode (SSH)*, log in with `ssh root@<ip>` (password `notatv` on those versions) and run the fixed updater once. It finishes the update and restarts the box:
```
curl -fsSL https://raw.githubusercontent.com/fjallefar/NotaTV/main/external/board/rpi4/overlay/usr/libexec/notatv/update -o /tmp/update-fix && sh /tmp/update-fix resume
```
Then update under *Settings → System → Update NotaTV*. If it says "Restart the box to finish the update", restart once more.

**Torn frames?** Set `kms_finish=1` in `/data/notatv/notatv.conf` and restart.

**Logs on the box** (with *Developer mode (SSH)*; readable by root only, tokens and passwords masked):

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

---

## Open source and privacy

Everything that runs in Linux on the box is open source, so you can always check what data the box collects and sends. Closed software such as Spotify's SDK, Widevine or Google Cast is not used.

The one opt-in exception: apps that need a closed module to play (DRM, e.g. TV 2 Play with Google's Widevine) are hidden in the App Store until you tick *Show apps that need closed software* at the bottom of the store. The Widevine module is then downloaded from Raspberry Pi's official archive the first time you play, and deleted when you uninstall the app.

**Network monitor** (*Settings → Network → Network monitor*) shows live which programs on the box have connections and where they go: the internet (orange), the local network (blue), and ports the box listens on (gray).

The box only contacts outside services when you use a feature. Apps only do so when they are installed:

| Service | When |
|---|---|
| plex.tv and your Plex servers | Plex |
| Your Jellyfin server | Jellyfin, "Play On" |
| YouTube (via yt-dlp) | YouTube, YouTube cast |
| api.twitch.tv, id.twitch.tv, irc.chat.twitch.tv | Twitch (login, lists, anonymous chat) |
| production.dr-massive.com, login.dr.dk + DR's image and video servers | DRTV (login only if you sign in) |
| auth.tv2.dk, play.tv2api.dk, pages-graph.discovery.tv2api.dk + TV 2's video and license servers | TV 2 Play (login, pages, playback, Widevine license) |
| archive.raspberrypi.com, deb.debian.org, snapshot.debian.org | TV 2 Play: the Widevine module and NSPR, once, the first time you play |
| Your Seerr server, TMDB images | Seerr |
| netflix.com (public top 10 data file) | Seerr → Netflix top 10 |
| Your Flimmertv server (local network), TMDB images | Flimmer |
| Your IPTV provider | Live TV |
| radio-browser.info and the radio stations' own servers | Radio (station lists, logos, the stream) |
| api.open-meteo.com, geocoding-api.open-meteo.com | Weather (every 20 minutes; the town search) |
| api.energidataservice.dk | Electricity price (prices every 15 minutes, grid tariffs every 6 hours) |
| Your TeslaMate's MQTT server (local network) | Tesla |
| tile.openstreetmap.org | Tesla → Map (off until you turn it on) |
| musicbrainz.org, coverartarchive.org | Cover art for Bluetooth music (can be turned off) |
| api.github.com, github.com | Updates and the App Store |
| pool.ntp.org | Setting the clock at startup |

---

## Languages

The user interface is in **English** (the default) and **Danish**. Change it under *Settings → System → Language* or in the first step of the setup guide. The default subtitle and audio languages, Seerr's metadata language and the country list follow it. The keyboard layout (on-screen and physical) is Danish when the language is Danish or the country is Denmark, Greenland or the Faroe Islands, otherwise US.

**Adding a language:** the source text is English, and the other languages are gettext `.po` files: [`src/shell/assets/lang/`](src/shell/assets/lang/) for the system, and `src/apps/<id>/lang/` for each app.
1. Run `python3 scripts/i18n-check.py --pot` to write the template `src/shell/assets/lang/notatv.pot`.
2. Copy it to `<code>.po` (e.g. `sv.po`) and fill in every `msgstr`. Do the same for each app: copy its `lang/da.po` to `lang/<code>.po` and translate it (e.g. with Poedit).
3. Add the language to `i18n_langs[]` in `src/shell/i18n.c` and in its copy `src/moon/i18n.c`.
4. Run `python3 scripts/i18n-check.py`. It reports missing texts and placeholders (`%s`, `%d`) that do not match.

---

## Limitations

- **Netflix, Disney+, HBO Max etc. are not supported.** They require Google's Widevine DRM module, which is closed source. The software version (Widevine L3) can run on a Pi, but only up to about 720p, and including it would break the open source principle. Full HD/4K requires Widevine L1 on certified hardware.
- **Spotify has no app.** The open source client librespot is broken for many accounts and the official SDK is closed. Play Spotify via AirPlay or Bluetooth instead.
- **Raspberry Pi 4:** decodes HEVC (4K, 10-bit) and H.264 (up to 1080p) in hardware, but not VP9 or AV1 - that is why YouTube uses H.264 up to 1080p. 4K60 HEVC peaks at about 42 fps, and Dolby Vision plays as HDR10.
- **Raspberry Pi 5:** decodes only HEVC in hardware; H.264 and VP9 up to 1080p are decoded in software by the faster CPU. Game streaming always uses HEVC.
- **TV without HDR (or HDR turned off):** HDR video is tone-mapped to SDR on the GPU with the simplest curve, so the brightest highlights are clipped, and 4K is drawn at 900p and scaled up by the display hardware to keep a full 24 fps. Plex can let the server convert HDR instead (*Apps → Plex*; 1080p, stereo sound for now).
- **Always HDR:** SDR video at 50/60 fps reaches only about 30 fps in this mode.
- Keyboard, mouse and controller input during game streaming is not finished yet.

---

## Building and development

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

### Making a release

Releases are built by GitHub Actions (`.github/workflows/release.yml`) on a self-hosted runner with the label `notatv`:

1. Raise `VERSION` (e.g. `1.0.2`), write what is new under `## 1.0.2` in [CHANGELOG.md](CHANGELOG.md) (it becomes the release text; start it with the short summary the boxes show, see the top of the file), and push.
2. Tag it: `git tag v1.0.2 && git push origin v1.0.2`. The tag must match `VERSION`.
3. The runner publishes `notatv-rpi4.img.xz`, `notatv-rpi4-update.tar.gz` and its `.sha256`, and packs the apps (`scripts/apps-catalog.py`) into the pre-release `apps`, the App Store. Raise an app's `version` in its `manifest.json` when it changes, and write what is new in its `changes` (see [src/apps/README.md](src/apps/README.md)).
4. Only apps changed? Push a tag `apps-YYYY-MM-DD` instead (`.github/workflows/apps.yml`): no new system version is needed.

The build folder is kept between releases, so normally only NotaTV's own packages are rebuilt (minutes). Changes to the Makefile, defconfig, packages, kernel fragments, patches, `config.txt`/`cmdline.txt` or the rpi-ffmpeg commit trigger a full build. The Twitch Client ID comes from the repository secret `TWITCH_CLIENT_ID`.

### Working on the code

Code and comments are in English, and user-facing text goes through `T("…")` and the language files. **Making an app:** see [src/apps/README.md](src/apps/README.md).

**Live updates on a running Pi:** turn on *Settings → System → Developer mode (SSH)* and log in with `ssh root@<ip>` (the password is shown right below the setting). Copy a new `tvshell` to `/usr/bin/tvshell.new`, check that it is not empty, `mv` it into place and run `killall tvshell`; it restarts by itself. Before you replace a shared library, compare its dependencies (`readelf -d … | grep NEEDED`) with what is on the box.

**UI tests without a TV:** `scripts/uitest.sh <dir> "<script>"` runs the UI headless with the app packages from `src/apps/out` and saves screenshots. Example: `w:60;s:home;k:right;k:ok;w:30;s:app;q`.

**Debug inbox:** `echo ok > /tmp/notatv-inbox/_key` sends a key (up/down/left/right/ok/back/home/menu/playpause), and `echo /tmp/x.bmp > /tmp/notatv-inbox/_shot` takes a screenshot.

**The look** (keep it the same everywhere):
- One left edge: 96 px from the screen edge (1920x1080 UI), also in the player OSD.
- Focus is a lift, not a frame: `ui_focus()` + `ui_focus_under()/ui_focus_over()`. Lists use a white pill that glides (`slide_to()`), dialogs and panels are dark glass (`draw_panel()`), and short messages are a pill at the top (`app_toast()`, `script-message notatv-toast` in the player).
- Choices for an item are a sheet (`choice_open()`), never a button bar at the bottom.
- Animations use springs (`spring_to()`) and stay short. On the Pi the GPU is the limit, so avoid extra full-screen layers.
- App icons are our own drawings (no logo files from the services): `scripts/gen-icons.py` renders them with `rsvg-convert`.

### Project layout

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
  i18n.c subs.c tz.c quick.c   translations, OpenSubtitles, time zones, the quick panel
  assets/        fonts (Inter), app icons and language files
src/apps/                       the apps, one package per folder, downloaded from the App Store into /data/notatv/apps-pkg
src/moon/                       tvmoon: game streaming client (Moonlight protocol)
scripts/                        i18n check, UI tests, app catalog, app icons, time zone list
.github/workflows/              release.yml (v* tags) and apps.yml (apps-* tags)
```

---

## License

NotaTV's own code is open source. The licenses declared in the Buildroot packages are:
- the user interface (`src/shell`, tvshell) and `rpi-tools`: **MIT**
- the game streaming client (`src/moon`, tvmoon): **GPL-3.0-or-later**, because it links to libgamestream (GPL-3.0+)

Third-party components keep their own licenses. Examples are the Inter font (SIL OFL, `src/shell/assets/Inter-LICENSE.txt`), the time zone names from iso-codes (LGPL-2.1) and tzdata (public domain).
