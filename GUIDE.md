# NotaTV guide

How to use the box. [Back to the README](README.md).

- [Using NotaTV](#using-notatv)
- [Apps](#apps)
- [Casting from your phone](#casting-from-your-phone)
- [Picture and sound](#picture-and-sound)
- [Languages](#languages)
- [Updating](#updating)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)

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

**Home screen:** the apps are a grid of seven per row (the default) or a **carousel**: one row where the app in focus is the biggest. Choose under *Settings → Home screen → Layout*. In the grid, a banner at the top shows what you can continue in the app in focus. *Edit home screen* is in the control center: the tile in focus shakes a little, and OK picks it up.

**Control center:** the button by the clock (↑ from the first row of apps), 🔴 Red on the home screen or the Power key. It shows the time, the box's name and address, a **sleep timer** (OK steps through off, 15, 30, 60, 90 and 120 minutes; a message one minute before, then the box goes to sleep), shortcuts to Bluetooth, Picture, Network, *Edit home screen* and All settings, and Sleep, Restart and Power off.

**Sleep** (from 1.0.26): the Raspberry Pi cannot suspend, so sleep stops what is playing and turns the TV off over HDMI-CEC; the box itself stays on. A key on the remote, *Turn on* on the phone page or switching the TV to the box's input wakes it again. Turning the TV off with its own remote puts the box to sleep (*Settings → Remotes and devices → Sleep when the TV turns off*), and *Automatic sleep* (*Settings → System*) does it after a few hours without a key press.

**Settings** has ten sections, the everyday ones first: Home screen, Network, Picture, Sound, Language and subtitles, Remotes and devices (HDMI-CEC and Bluetooth), Cast and sharing, Apps, System and Advanced (overclock, HDR metadata, beta updates, troubleshooting log, developer mode).

### Phone page

Scan the QR code under *Settings → Network → Connect a phone* with the phone's camera (or open `http://<box IP>`). From there you can:
- use the remote control
- type straight into a text field on the TV
- set up IPTV, Xtream Codes, YouTube and network drives

Keys and text work right away. **Playlists, logins, network drives, names and YouTube accounts are only saved after you approve them on the TV.**

**Encrypted (from 1.0.25):** the QR code holds a key in the part of the address after `#`, which the browser never sends over the network. The page encrypts everything it sends with it (XSalsa20-Poly1305), so logins, passwords and YouTube cookies cannot be read by others on the network, and the key is also the login - no code to type. A message can only be used once and only within 10 minutes. *New QR code* on the same screen makes a new key; phones with the old one must scan again. The key is kept only in that browser tab.

Without the QR code the page asks for the code (set in the setup guide, *Settings → Network → Phone page code*, 6-8 digits). With the code you can use the remote and type text, but **logins, playlists, network drives and YouTube accounts are only accepted encrypted**. 10 wrong codes in 10 minutes lock the page, and it only accepts POST requests of at most 512 KB.

The web server runs as its own user without access to settings or logins; it only hands requests to a small service that checks them (from 1.0.25).

---

## Apps

Everything on the home screen is an app, and NotaTV comes without any: you pick them in the setup guide, and they are downloaded from the **App Store** on the home screen. There you can install more apps, update them (*Update all* when several have updates, with a short card about what is new) and uninstall them. Uninstalling an app deletes everything it has saved on the box, including your sign-in. Each app's settings are in the app's own menu and under *Settings → Apps*.

### Live TV

M3U playlist or Xtream Codes login (live channels only). Channels are grouped by country, and favorites can be sorted. The **TV guide** shows your favorites on a timeline (left/right moves between programs). Channel numbers and now/next come from XMLTV or Xtream's short EPG. **Reminders:** a minute before a program starts, the TV offers to switch (after the video, if something is playing).

### Plex

Sign in via `plex.tv/link`. Several servers, hubs, libraries, "Continue watching" and direct play. Search covers all servers. Two home styles: *hero* or *focus*.

### Jellyfin

Server address, username and password. Libraries, a series page with seasons and episodes, "Continue watching" and "Next episode". Always plays the original file (no server transcoding), with the audio and subtitles chosen for your Jellyfin user. Progress is reported to the server, which sees the box under its own name.

### YouTube

yt-dlp with Node.js and mpv. Videos are resolved while they are in focus, so playback starts almost at once. Up to 1080p on a Raspberry Pi (1440p and 4K are greyed out: YouTube only has them as VP9/AV1). Optional account via cookies from the phone page; subscriptions can be imported from Google Takeout.

### Twitch

Sign in with a code or QR code at `twitch.tv/activate`. Following, Popular, Categories and Search. Chat is read anonymously and shown as a sidebar. Quality from 1080p to 360p.

### DRTV

DR's live channels (DR1, DR2, DR Ramasjang, DRTV Ekstra, TVA Live) with now and next, the front page, categories, A-Z, kids and search. Series with seasons, resume, skip the end credits and next episode. Free and without DRM. Signing in with a DR account adds DR's own *Continue watching* and *My list*. Programmes marked *Denmark only* need a Danish internet connection.

### TV 2 Play

Opt-in (closed software, see [Open source and privacy](PRIVACY.md)). Sign in with a code on the TV. Front page, channel list, categories, search, series and episodes, continue watching and subtitles. Needs your own TV 2 Play subscription. Live playback is not supported yet.

### Seerr

Find and request movies and shows (Seerr, Overseerr, Jellyseerr) with your Plex login. Season picker, trailers, "More like this" and Netflix top 10 for your country.

### Flimmer

Find and request movies and series on a Flimmertv server (requests, movies and series in one server). Finds the server on the network (port 7676); sign in with your Flimmertv email and password. Discover, search, a season picker, trailers, "More like this", and *My requests*, where a request still waiting can be withdrawn.

### Music

Receives music from your phone over **AirPlay** (iPhone/iPad/Mac) or **Bluetooth** (any phone). "Now playing" shows cover art, title and progress.

### Radio

Stations from radio-browser.info (free, no login), from your own country or any other. Popular, genres, search, recently played and favourites (the first nine get a number you press on the remote). Keeps playing while you use the rest of the box, with *Now playing* in the top bar; it stops by itself when a video or game starts.

### Games

Game streaming from a PC running Sunshine with our own client `tvmoon`: HEVC decoded in hardware straight to the screen, at 1080p 120 Hz. To pair, type the PIN shown on the TV on `https://localhost:47990` on the PC (Sunshine rejects it on the PC's IP address); Back cancels.

### Weather

Now, the next 24 hours and 7 days from Open-Meteo (open data, no account). The temperature is shown in the home screen's top bar. The town comes from the time zone until you choose one at the top of the app.

### Electricity price

The Danish spot price with VAT hour by hour, today and tomorrow (from about 13:00), from Energi Data Service. The price now in green, yellow or red, the cheapest hours and a chart. The buttons at the top choose West or East Denmark, your grid company (for the full price with tariffs and tax) and your supplier's surcharge. The price right now is also shown in the home screen's top bar (it can be turned off at the top of the app). The arrows step through the chart hour by hour. A line marks every krone. The prices are remembered, and fetched at startup, every 6 hours and at 13:15.

### Tesla

Reads your own TeslaMate over its MQTT server (port 1883), read only: nothing is sent to the car and nothing is saved on disk. Battery, range, charging, climate, tyres, locks and doors, the car seen from above, where it is, route with arrival time, model and software. The side menu chooses the car, turns each part on or off, and puts up to 3 cars small at the top of the home screen.

### Media

USB drives are mounted automatically. SMB network drives (with a login or as a guest) and NFS drives (`nfs://server/path`, NFSv4 or v3). Plays movies, music and pictures.

**Host it yourself:** [SELF-HOSTING.md](SELF-HOSTING.md) lists, per app, the server software it talks to (Jellyfin, Seerr, Sunshine, TeslaMate, Live TV playlists …) with links to each project.

**Finding servers** (Plex, Jellyfin, Seerr, Flimmer, TeslaMate) only searches your local network: Plex GDM, Jellyfin UDP 7359, and a scan of your own subnet for the other servers' ports.

**Subtitles from OpenSubtitles** (Plex and Jellyfin): choose *Search subtitles* in the player menu. It needs a free API key: create an account on opensubtitles.com, make a key under *Consumers* and enter it in *Settings → Language and subtitles*, together with your OpenSubtitles username and password (OpenSubtitles may refuse downloads without a login). Movies are found by their TMDB id, episodes by show, season and episode; the subtitle you pick is turned on at once.

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

**Bluetooth pairing:** *Settings → Remotes and devices → Pair a phone* makes the TV visible for 3 minutes. The phone and the TV show the same code, and you confirm it on the TV, so nobody can pair without that confirmation. Otherwise the TV is only visible while Music is open. *Pair a controller or keyboard* finds controllers, keyboards and mice in pairing mode; a keyboard that needs a code shows it on the TV.

---

## Picture and sound

- **HDR10** output, on the Pi with 12-bit YCbCr 4:2:2 and BT.2020.
- **Frame rate matching:** the TV switches to the video's frame rate (23.976/24/25/50/59.94/60 Hz).
- The box reads the display's EDID. On a display without HDR the HDR settings (also for game streaming) are locked and HDR is never sent. Surround passthrough only uses the audio formats the TV or receiver lists.
- **Always HDR** keeps the TV in HDR all the time: the menus are drawn in PQ and SDR video is converted on the GPU.
- **Audio** goes to HDMI by itself. Dolby/DTS and TrueHD/DTS-HD can be passed through, and lip sync can be adjusted. For surround over ARC (not eARC), turn on *Surround passthrough* and turn off *TrueHD / DTS-HD*: audio that cannot go over ARC is re-encoded to Dolby Digital 5.1.

---

## Languages

The user interface is in **English** (the default) and **Danish**. Change it under *Settings → Language and subtitles → Language* or in the first step of the setup guide. The default subtitle and audio languages, Seerr's metadata language and the country list follow it. The keyboard layout (on-screen and physical) is Danish when the language is Danish or the country is Denmark, Greenland or the Faroe Islands, otherwise US.

---

## Updating

*Settings → System → Update NotaTV* downloads the newest release from GitHub, checks its SHA256 and installs it. Your settings and sign-ins are kept.

- **Safe against power cuts:** nothing is changed until the whole package is downloaded and checked, and each file is written with a single rename, so the box has either the old or the new version. An interrupted update is finished at the next boot. The kernel is kept as two complete sets on the boot partition, and an update switches between them with one small write.
- **Undo last update** (same menu) puts the previous version back. This also happens by itself if the user interface keeps crashing right after an update.
- **An app that crashes** NotaTV twice in a row right after start is turned off, and the box says which one. It stays installed: a new version from the App Store is loaded again, and *Install* in the App Store tries it once more.
- **Update reminders:** the box looks for a new release twice a day and shows a reminder on the home screen at most every other day. Turn it off under *Settings → System → Update reminders*; then the box never contacts GitHub by itself.
- **Anonymous usage count** (from 1.0.27): once a day the box sends only its version number, so we can see how many boxes are in use. No box id, account, settings or address is sent or stored. Turn it off under *Settings → System → Send anonymous usage count*. Details in [Privacy](PRIVACY.md); the server's code is in [`server/stats`](server/stats).
- The update card shows a short summary of what is new (from [CHANGELOG.md](CHANGELOG.md)).
- **A new kernel is tried once** (from 1.0.25): if it does not start, the box goes back to the previous version by itself at the next start - no new flash.
- **Beta updates** (*Settings → Advanced*): get new versions and apps before everyone else, to test them. They can have errors.
- **yt-dlp and Node.js** (for YouTube) are not part of the system from 1.0.25: the box fetches them from the App Store into `/data`, so they can be updated without a system update. Right after updating to 1.0.25, YouTube says so while they are downloaded.

---

## Troubleshooting

**Get the logs without SSH:** *Settings → System → Get the logs* shows the box's address and code. Open it on a phone or computer and press *Download logs*: one `.tar.gz` with all logs, system info and settings, with passwords, tokens and logins removed. Nothing is sent anywhere by itself. For more detail, turn on *Settings → Advanced → Troubleshooting log* first: every network request is logged, and the logs are kept across restarts until you turn it off.

**The box restarted by itself?** From 1.0.25 the log is kept on the SD card (only new lines, every 15 minutes - a few KB a day), and at every start the box notes whether the last run ended normally, when it was last alive, whether it had lost power, and any crash message from the kernel. Get the logs as above and look at `kept/notatv.log` and `kept/crash-*`. A start nobody asked for does not turn the TV on.

**A box stuck on an update** ("Restart the box to finish the (previous) update" again and again): the updater in 1.0.0-1.0.2 could not finish an update that changes the kernel. Turn on *Settings → Advanced → Developer mode (SSH)*, log in with `ssh root@<ip>` (password `notatv` on those versions) and run the fixed updater once. It finishes the update and restarts the box:
```
curl -fsSL https://raw.githubusercontent.com/fjallefar/NotaTV/main/external/board/rpi4/overlay/usr/libexec/notatv/update -o /tmp/update-fix && sh /tmp/update-fix resume
```
Then update under *Settings → System → Update NotaTV*. If it says "Restart the box to finish the update", restart once more.

**Torn frames?** Set `kms_finish=1` in `/data/notatv/notatv.conf` and restart.

**Playback, YouTube or AirPlay stopped working after 1.0.21?** Since 1.0.21 they run as the user `media` instead of root. Set `media_user=0` in `/data/notatv/notatv.conf` and restart to check whether that is the cause (and tell us). Nothing but TV 2 Play played after a fresh 1.0.21 install on Wi-Fi: update to 1.0.22, which fixes it.

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

## Limitations

- **Netflix, Disney+, HBO Max etc. are not supported.** They require Google's Widevine DRM module, which is closed source. The software version (Widevine L3) can run on a Pi, but only up to about 720p, and including it would break the open source principle. Full HD/4K requires Widevine L1 on certified hardware.
- **Spotify has no app.** The open source client librespot is broken for many accounts and the official SDK is closed. Play Spotify via AirPlay or Bluetooth instead.
- **Raspberry Pi 4:** decodes HEVC (4K, 10-bit) and H.264 (up to 1080p) in hardware, but not VP9 or AV1 - that is why YouTube uses H.264 up to 1080p. 4K60 HEVC peaks at about 42 fps, and Dolby Vision plays as HDR10.
- **Raspberry Pi 5:** decodes only HEVC in hardware; H.264 and VP9 up to 1080p are decoded in software by the faster CPU. Game streaming always uses HEVC.
- **TV without HDR (or HDR turned off):** HDR video is tone-mapped to SDR on the GPU with the simplest curve, so the brightest highlights are clipped, and 4K is drawn at 900p and scaled up by the display hardware to keep a full 24 fps. Plex can let the server convert HDR instead (*Apps → Plex*; 1080p, stereo sound for now).
- **Always HDR:** SDR video at 50/60 fps reaches only about 30 fps in this mode.
- Keyboard, mouse and controller input during game streaming is not finished yet.
