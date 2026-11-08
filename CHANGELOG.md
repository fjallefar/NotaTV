# Changelog

What is new in each NotaTV version. The section for a version becomes the text of its GitHub release
(`.github/workflows/release.yml` takes everything from `## <version>` to the next `## `).

Start each section with a short summary as a quote: one line "In short" and a few points. The box shows it on the
update card and before installing. A translation for the box can follow as an HTML comment (not shown on GitHub):

```
> **In short:** One sentence about the update.
> - a point
> - another point

<!-- tldr:da
Kort: Én sætning om opdateringen.
- et punkt
- et andet punkt
-->
```

## 1.0.15

> **In short:** New apps - DRTV, Weather and Electricity price - plus automatic sleep and power buttons on the phone page.
> - New in the App Store: DRTV (DR's channels and programmes, free), Weather and Electricity price
> - Live TV: reminders - Yellow on a coming program in the TV guide
> - Automatic sleep: after 4 hours without a key press the box and the TV go to sleep (after a warning) - not while music or radio plays
> - Phone page: Turn on (the TV turns on and switches to the box), Sleep and Restart
> - Games: Xbox controllers over Bluetooth work fully, games show the right buttons, and the controller vibrates

<!-- tldr:da
Kort: Nye apps - DRTV, Vejr og Elpris - plus automatisk dvale og strømknapper på telefonsiden.
- Nyt i App Store: DRTV (DR's kanaler og programmer, gratis), Vejr og Elpris
- Live TV: påmindelser - Gul på et kommende program i TV-guiden
- Automatisk dvale: efter 4 timer uden tastetryk går boksen og TV'et i dvale (efter en advarsel) - ikke mens der spilles musik eller radio
- Telefonsiden: Tænd (TV'et tænder og skifter til boksen), Dvale og Genstart
- Spil: Xbox-controllere over Bluetooth virker helt, spillene viser de rigtige knapper, og controlleren vibrerer
-->

- DRTV (new app): DR's live channels with now and next, the DRTV front page, categories, A-Z, kids and search, series with seasons, resume, skip the end credits and the next episode. Plain HLS, no DRM - no account needed; signing in with a DR account adds DR's own *Continue watching* and *My list*. A *Continue watching* row on the home screen
- Weather (new app): now, the next 24 hours and 7 days from Open-Meteo, the temperature in the home screen's top bar, the town from the time zone until you choose one
- Electricity price (new app): the Danish spot price with VAT hour by hour, today and tomorrow, from Energi Data Service - the price now in green/yellow/red, the cheapest hours and a chart, West or East Denmark
- Live TV 1.0.8: reminders - Yellow on a coming program in the TV guide (or choose it in the channel's program schedule); a minute before it starts the TV offers to switch, and after a video if one was playing
- Automatic sleep (*Settings → System*, off / 2 / 4 / 8 hours, 4 by default): a warning, then sleep a minute later unless a key is pressed. Not while music or radio plays; game streaming counts as use
- Phone page: a *Power* box - *Turn on* sends HDMI-CEC power-on and switches the TV to the box (and wakes the screen), *Sleep* and *Restart* ask first. A sleeping box cannot be reached from the phone; the remote wakes it
- Game streaming: Xbox controllers paired over Bluetooth report the triggers and the right stick on other axes - detected per controller, so both work; the PC is told whether it is an Xbox, PlayStation or Nintendo controller (the right button symbols in games); rumble from the PC is played on the controller
- New tile pictures for DRTV, Weather and Electricity price

## 1.0.14

> **In short:** NFS network drives work, controllers and keyboards can be paired from the TV, and apps can have their own picture.
> - Bluetooth: *Pair a controller or keyboard* finds the controller, keyboard or mouse and pairs it
> - NFS network drives mount (NFSv4, otherwise NFSv3)
> - Pi: the menus are drawn straight into the picture the TV shows - one less full-screen copy per frame
> - Samsung TVs: the TV remote works over HDMI-CEC (Anynet+)
> - Safer: only the local network can reach the box, DLNA asks before a new device plays, and Wi-Fi names with æ, ø and å work

<!-- tldr:da
Kort: NFS-netværksdrev virker, controllere og tastaturer kan parres fra TV'et, og apps kan have deres eget billede.
- Bluetooth: Par en controller eller et tastatur finder controlleren, tastaturet eller musen og parrer den
- NFS-netværksdrev monteres (NFSv4, ellers NFSv3)
- Pi: menuerne tegnes direkte i det billede, TV'et viser - én fuldskærms-kopi mindre pr. billede
- Samsung-TV: TV'ets fjernbetjening virker over HDMI-CEC (Anynet+)
- Mere sikker: kun det lokale net kan nå boksen, DLNA spørger før en ny enhed afspiller, og Wi-Fi-navne med æ, ø og å virker
-->

- Bluetooth: new *Settings → Bluetooth → Pair a controller or keyboard*. The box searches while it is open and lists the input devices nearby (strongest signal first); OK pairs, trusts and connects the chosen one. A keyboard that needs a code gets it shown on the TV. Only the device you chose is approved
- HDMI-CEC on Samsung TVs (Anynet+): a new helper, `cecd`, answers what the TV asks and the kernel did not - above all *Menu Request*, which Samsung needs before it sends the remote's keys, and also the power status and which input is active. Keys, switching to the box's input and "sleep when the TV turns off" go through it on every TV. Tip: skip the Samsung guide that asks which box is connected (Apple, Nvidia, Roku ...) - it sets up Samsung's universal remote, not CEC - and turn on *Anynet+ (HDMI-CEC)*
- NFS network drives: the address lookup gave the kernel the DNS server's address for an IP share, so NFS never mounted. BusyBox now resolves the name itself; NFSv4 first, then NFSv3, with a timeout so a dead server does not hang
- Pi display: the UI is drawn straight into the display buffer instead of a texture that was copied over every frame (about 16 MB less memory traffic per frame on a Pi 4). `kms_direct=0` in notatv.conf goes back to the old way
- tvshell no longer can crash while quitting (OpenSSL's cleanup ran under background downloads)
- Apps can have their own `icon.png`: it is packed with the app and published next to it in the App Store catalog (with SHA256), so the App Store shows it before the app is installed
- Firewall: the box's services only answer the local network (private and link-local addresses, and the networks the box is on); `firewall=0` turns it off for troubleshooting
- DLNA: the first time a phone or PC wants to play or control playback, the TV asks "Allow ... to play on the TV?" (like YouTube cast); a denial holds for 10 minutes
- Wi-Fi: networks with æ, ø, å, emoji or quotes in the name are shown correctly and can be joined
- Settings: a value longer than 4 KB could become a setting of its own after a restart; the file is now read without a line limit, and the phone page refuses fields over 2 KB. The App Store address from the settings must be https
- Developer mode: after an update or "Undo last update", the box's own SSH password is set again at once (before, the image's public one worked until the restart)
- TV 2 Play: the login token is no longer on the helper's command line, and the log bundle masks tokens shaped like a JWT
- yt-dlp can be updated through the App Store, without a new NotaTV version (it is not shown in the store; the box takes a newer one by itself)
- mpv, ffprobe, yt-dlp and UxPlay can run as their own user instead of root (`media_user=1`, off for now while it is tested)
- Smaller fixes: an https request is never redirected to http, the data partition is checked after a power cut, unanswered YouTube logins sent from the phone are deleted, reproducible app packages, and the App Store upload replaces one file at a time

## 1.0.13

> **In short:** Smoother on the Raspberry Pi 4, a softer channel change in Live TV, and network drives with a login.
> - Pi 4: text, images and screen transitions are prepared in a way that does not stall the graphics chip - about a quarter of the stutter is left, and the worst frames are half as long
> - Live TV: the channel banner fades in and out when you change channel, and the raw channel name in the top corner is gone
> - Network drive: asks for a username and password (empty = guest), with a *Network drive login* item to change it

<!-- tldr:da
Kort: Mere glat på Raspberry Pi 4, et blødere kanalskift i Live TV og netværksdrev med login.
- Pi 4: tekst, billeder og skærmskift forberedes, så grafikchippen ikke går i stå - omkring en fjerdedel af hakkene er tilbage, og de værste billeder er halvt så lange
- Live TV: kanalbanneret toner ind og ud, når du skifter kanal, og det rå kanalnavn i hjørnet er væk
- Netværksdrev: spørger efter brugernavn og adgangskode (tomt = gæst), med punktet Login til netværksdrev til at ændre det
-->

- Pi 4 smoothness: new glyphs are uploaded at the start of the next frame, text atlases start small and grow, learned text sizes are prepared in idle time, images are decoded at the size they are drawn (and again if drawn bigger), big images are uploaded in strips, and the transition textures are made in idle time. Measured on a Pi 4: seconds with stutter 10-11 -> 2-3, worst frame 67 -> 34 ms
- After playback the graphics are set up once instead of twice
- Live TV: the banner and channel logo fade in and out on a channel change; mpv's own title in the corner is no longer shown
- Network drive: username and password (saved apart from the address), empty = guest
- Sign out wipes more: rotated and helper logs, images in memory, late replies from Plex/YouTube/Twitch; the open troubleshooting log is emptied instead of deleted, so it keeps working
- The phone page code never ends up in the log
- Screensaver glides in over a second and fades quickly back out
- Setup guide: the app list scrolls so Continue can always be reached
- Fixes: no crash after playback (transition textures), keyboard pointer cleared with its screen
- Apps in the App Store (they also work on 1.0.12): Live TV 1.0.6 (TV guide without overlapping programmes, fading banner), Jellyfin 1.0.4 and Seerr 1.0.5 (https by default, warning for unencrypted sign-in), Media 1.0.2 (slow drives no longer freeze the screen), Music 1.0.3, Plex 1.0.5, Twitch 1.0.6, YouTube 1.0.6 (a new device must be allowed on the TV before it can cast)

## 1.0.12

> **In short:** Wi-Fi connects faster when the box starts, and a box set up in English stays in English.
> - Wi-Fi: after connecting, the box asks for an address again after 3 s instead of 20 s, and now knows your country's Wi-Fi rules (the regulatory database was missing, so all channels were treated by the strictest rules). Start-up steps and wpa_supplicant's messages are in the troubleshooting log
> - Language: choosing English in the setup guide was not saved, so the box switched to Danish after the first restart (e.g. after an update). If that happened to you, choose English again in *Settings*
> - New page [SELF-HOSTING.md](https://github.com/fjallefar/NotaTV/blob/main/SELF-HOSTING.md): the server software behind each app, with links

<!-- tldr:da
Kort: Wi-Fi forbinder hurtigere, når boksen starter, og en boks sat op på engelsk forbliver på engelsk.
- Wi-Fi: efter forbindelsen spørger boksen igen efter en adresse efter 3 s i stedet for 20 s, og kender nu dit lands Wi-Fi-regler (databasen manglede, så alle kanaler fulgte de strengeste regler). Opstartens trin og wpa_supplicants beskeder står i fejlsøgningsloggen
- Sprog: valgte man engelsk i opsætningsguiden, blev det ikke gemt, så boksen skiftede til dansk efter første genstart (fx efter en opdatering). Er det sket for dig, så vælg engelsk igen i Indstillinger
- Ny side SELF-HOSTING.md: serversoftwaren bag hver app, med links
-->

## 1.0.11

> **In short:** A Danish keyboard when the box is set to Denmark, and pairing a PC for games can be cancelled.
> - Keyboard: the on-screen keyboard and physical keyboards use the Danish layout when the country is Denmark, Greenland or the Faroe Islands, also with English menus
> - Games: Back (or Esc) cancels pairing while the code is shown

<!-- tldr:da
Kort: Dansk tastatur når boksen er sat til Danmark, og parring af en PC til spil kan afbrydes.
- Tastatur: skærmtastaturet og fysiske tastaturer bruger dansk layout, når landet er Danmark, Grønland eller Færøerne - også med engelske menuer
- Spil: Tilbage (eller Esc) afbryder parringen, mens koden vises
-->

## 1.0.10

> **In short:** TV 2 Play lets you choose who is watching, and apps with closed software only show up where you chose them.
> - TV 2 Play 1.0.1: choose your profile, and switch profile on My page
> - Setup guide: TV 2 Play is no longer offered there
> - App Store: apps that need closed software are listed below the check box

<!-- tldr:da
Kort: TV 2 Play lader dig vælge, hvem der ser med, og apps med lukket software dukker kun op, hvor du har valgt dem.
- TV 2 Play 1.0.1: vælg din profil, og skift profil på Min side
- Setup-guiden: TV 2 Play bliver ikke længere tilbudt dér
- App Store: apps, der kræver lukket software, står under fluebenet
-->

- Setup guide: apps that need closed software (TV 2 Play) are no longer offered - only through the App Store's check box.
- App Store: when the check box is ticked, those apps are listed below it, apart from the other apps.
- TV 2 Play 1.0.1: choose who is watching when the account has several profiles, and switch profile on My page.

## 1.0.9

> **In short:** TV 2 Play comes to the App Store as an opt-in app, behind a new check box for apps that need closed software.
> - App Store: tick *Show apps that need closed software* at the bottom to see apps that need a DRM module
> - TV 2 Play 1.0.0: sign in on the TV, front page, search, series, continue watching and subtitles

<!-- tldr:da
Kort: TV 2 Play kommer i App Store som en app, du selv vælger, bag et nyt flueben for apps, der kræver lukket software.
- App Store: sæt flueben i *Vis apps, der kræver lukket software* nederst for at se apps, der kræver et DRM-modul
- TV 2 Play 1.0.0: log ind på TV'et, forside, søgning, serier, fortsæt med at se og undertekster
-->

- App Store: a check box at the bottom, *Show apps that need closed software*. Apps that need a closed DRM module (TV 2 Play) are hidden until it is ticked.
- New app: TV 2 Play 1.0.0 (opt-in, see above). The first time you play, it downloads Google's Widevine module from Raspberry Pi's archive (about 8 MB).

## 1.0.8

> **In short:** Remove things from the row on the home screen, forget one Bluetooth device at a time, and Live TV shows the right channel name when you start from the home screen.
> - Home screen: press Green on something in the row below the apps to remove it - it comes back when you watch it again
> - Bluetooth: forget one phone, controller or keyboard instead of all of them
> - Live TV: the right channel name in the banner when you start a channel from the home screen

<!-- tldr:da
Kort: Fjern ting fra rækken på forsiden, glem én Bluetooth-enhed ad gangen, og Live TV viser det rigtige kanalnavn, når du starter fra forsiden.
- Forsiden: tryk Grøn på noget i rækken under apps for at fjerne det - det kommer tilbage, når du ser det igen
- Bluetooth: glem én telefon, controller eller ét tastatur i stedet for dem alle
- Live TV: det rigtige kanalnavn i banneret, når du starter en kanal fra forsiden
-->

**Home screen and settings**
- Remove something from the row below the apps (Continue watching, Recent channels, Radio, YouTube): press Green on it and confirm. It comes back by itself when you watch it again. Works for every app; what is removed is forgotten when you sign out of an app or uninstall it. It is only removed on the box - Plex and Jellyfin still have it in their own Continue watching.
- Bluetooth: *Forget paired devices* now lists the paired phones, controllers and keyboards, so you can forget one of them - or all, as before.

**Playback**
- Live TV from the home screen: the channel banner showed the wrong name (often the channel watched last) when the channel list was not loaded yet or the provider had given the channel a new address. The channel is now also found by name, and otherwise it gets a banner of its own. Starting a channel also no longer puts the first channel of the list in *Recently watched*.

**Apps**
- Live TV 1.0.5: the right channel name in the banner when a channel is started from the home screen.

## 1.0.7

> **In short:** NotaTV now also runs on the Raspberry Pi 5, there is a new Radio app, and movies start faster on displays that cannot change to the movie's frame rate.
> - Raspberry Pi 5: the same image starts on a Pi 4 and a Pi 5 (new and not fully tested yet)
> - New app: Radio, with stations from your country and the whole world (in the App Store)
> - The box checks which frame rates your display has before a movie starts

<!-- tldr:da
Kort: NotaTV kører nu også på Raspberry Pi 5, der er en ny Radio-app, og film starter hurtigere på skærme, der ikke kan skifte til filmens billedfrekvens.
- Raspberry Pi 5: det samme image starter på en Pi 4 og en Pi 5 (nyt og ikke fuldt testet endnu)
- Ny app: Radio med kanaler fra dit land og hele verden (i App Store)
- Boksen tjekker, hvilke billedfrekvenser skærmen har, før en film starter
-->

**Raspberry Pi 5**
- One image for both: the same `notatv-rpi4.img.xz` starts on a Raspberry Pi 4 and a Raspberry Pi 5 (also the Pi 400 and Pi 500), and the same update package updates both. The box finds out which board it runs on by itself and adapts. New and not fully tested on a Pi 5 yet.
- The Pi 5 decodes only HEVC in hardware (4K, 10-bit HDR). H.264 and VP9 up to 1080p are decoded in software by its faster CPU.
- Overclock has its own choices on a Pi 5: 2400 MHz (standard), 2600, 2800 and 3000 MHz; above 2400 the voltage is raised a little. Needs the active cooler and a 5 V/5 A power supply. The choice is saved per board, and an SD card moved to the other board starts at its standard frequency.
- Game streaming always uses HEVC on a Pi 5 (it has no H.264 decoder).

**Playback**
- Frame rate match reads the display's own list of modes first and only asks for one it has. Before, a 24 fps movie on a display without 24 Hz (e.g. many PC monitors) failed once and started again, about a second slower. When the exact rate is missing, an exact multiple (e.g. 48 Hz for 24 fps) or an almost equal rate (24 Hz for 23.976 fps) is used; otherwise the display stays as it is.
- Logos in the .ico format (many radio stations, e.g. DR) are shown. A card on the home screen without a logo shows its name instead of a TV icon.
- Background audio: an app that plays while you use the rest of the box (Radio) is stopped by itself when a video or game starts, since the HDMI audio takes one player at a time.

**Apps**
- New: Radio 1.0.0. Stations from radio-browser.info (free and open, no login). The country follows the one chosen in the setup guide, and another can be chosen in the app. Popular, genres, search, recently played and favourites (Yellow; the first nine get a number you press on the remote). It plays in the background with a *Now playing* box in the top bar, shows the station on the home screen, and the home row *Radio* starts a recent station with one press. Burn-in protection for OLED screens (on by default, like Music): the screen glides very slowly, the key hints go away after 30 seconds, and after a minute of only listening everything but *Now playing* dims. Stopping by itself before video needs NotaTV 1.0.7.
- YouTube 1.0.5: on a Raspberry Pi 5 the quality also stops at 1080p (YouTube has 1440p and 4K only as VP9/AV1, which the Pi 5 cannot decode in hardware).
- Games 1.0.2: on a Raspberry Pi 5 games always stream in HEVC, and the Codec choice is locked there.

## 1.0.6

> **In short:** Wi-Fi is ready sooner after starting, and Music gets a Disconnect button and smoother burn-in protection.
> - Wi-Fi connects early while the box starts, and gets its address the moment it is connected
> - Music: a Disconnect button below the player
> - Burn-in protection in Music glides instead of moving a pixel at a time
> - Music's Now playing box and Tesla's cars no longer lie on top of each other

<!-- tldr:da
Kort: Wi-Fi er klar hurtigere efter opstart, og Musik får en knap til at afbryde og en blødere beskyttelse mod indbrænding.
- Wi-Fi forbinder tidligt, mens boksen starter, og får sin adresse, så snart den er forbundet
- Musik: en knap til at afbryde forbindelsen under afspilleren
- Beskyttelsen mod indbrænding i Musik glider i stedet for at flytte sig en pixel ad gangen
- Musiks Spiller nu-boks og Teslas biler ligger ikke længere oven i hinanden
-->

**System**
- Wi-Fi starts at the very beginning of booting instead of after the other services: the driver is loaded at once and the box connects as soon as the adapter is there. It asks for an address the moment Wi-Fi is connected, instead of waiting up to 20 seconds between attempts.
- Overlays in the top bar (Music's *Now playing*, Tesla's cars) are placed side by side and never on top of each other. When there is no room, *Now playing* shrinks to the cover, or moves just below the bar.
- Burn-in protection can move things by fractions of a pixel, so it glides smoothly.

**Apps** (update in the App Store; the fixes in the top bar and the smooth movement need NotaTV 1.0.6)
- Music 1.0.2: a *Disconnect* button below the player (Down, then OK) ends AirPlay or Bluetooth. Burn-in protection glides instead of jumping a pixel at a time.
- Tesla 1.0.1: the cars on the home screen make room for Music's *Now playing* box.

## 1.0.5

> **In short:** A new, calmer look across the whole box, a quick panel with a sleep timer, sharper pictures and the new Tesla app.
> - What you choose lifts gently toward you, and apps open out of their tile
> - Our own app icons, glass menus and the same player bar in every app
> - Quick panel (Red on the home screen): sleep timer, shortcuts and power
> - Sharper pictures on the home screen and channel logos without a white box
> - New app: Tesla, your car's battery and charging from your own TeslaMate

<!-- tldr:da
Kort: Et nyt, roligere udseende i hele boksen, et hurtigpanel med sluk-timer, skarpere billeder og den nye Tesla-app.
- Det, du vælger, løfter sig blødt frem, og apps åbner ud af deres flise
- Egne app-ikoner, menuer af glas og samme afspillerbjælke i alle apps
- Hurtigpanel (Rød på forsiden): sluk-timer, genveje og sluk
- Skarpere billeder på forsiden og kanallogoer uden hvid boks
- Ny app: Tesla, bilens batteri og opladning fra din egen TeslaMate
-->

**New look**
- Home screen: one left edge for everything, calmer tiles with our own app icons, a soft glow in the color of the app in focus, and the name of every app under its tile. *Move app* (Green) shows after a few seconds on a tile.
- Focus is the same everywhere: the chosen poster, tile or button lifts toward you with a small spring and a soft glow, instead of a white frame (Plex, Jellyfin, YouTube, Twitch, Seerr, the Live TV guide, the App Store).
- An app grows out of its tile when you open it, and shrinks back into it on Back. What is underneath is drawn once instead of every frame, so the animations run smoothly on the Pi.
- Dialogs, lists, the keyboard and the apps' side menus are dark glass, with a white pill that glides to the chosen row. Settings has the same gliding pill.
- The App Store only shows a status when it matters (*Install*, *Update*, *Installing*).

**Quick panel**
- Red on the home screen (or the Power key) opens a panel from the right: the time, the box's name and address, a **sleep timer** (15 minutes to 2 hours, with a message one minute before), shortcuts to Bluetooth, Picture and sound, Network and all settings, and Sleep, Restart and Power off.

**Player**
- A soft fade at the bottom instead of dark bands (they drew lines across the picture), a rounded timeline, and LIVE as a red pill. The Live TV channel banner and channel list look the same as the rest.
- Short messages (volume, subtitles, audio, chat on/off) are a pill at the top, the same as the menus' own messages.
- The Red menu is a glass panel from the right. Game streaming's menu has the same look.

**Pictures**
- The home screen's row and top get pictures in twice the resolution from Plex and Jellyfin, and pictures shown smaller than they are stay smooth.
- Channel logos stand on their own: a white background is removed, dark text is made light, the empty frame around them is cut away, and small logos are enlarged smoothly. No white box any more, also not while changing channel.

**System**
- The clock starts from the last known time (or the build time) instead of 1970 until a time server answers.
- Bluetooth: a phone pairing for the first time is noticed, and the pairing screen closes when it is done.

**Apps** (update in the App Store; the new look needs NotaTV 1.0.5)
- Tesla 1.0.0 (new): battery, range, charging, climate, tyres, locks and doors, the car seen from above, route and arrival, read only from your own TeslaMate over MQTT. Up to 3 cars small at the top of the home screen.
- Music 1.0.1: a cover in the album's colors while the real one loads, covers from Apple or MusicBrainz (your choice), protection against burn-in on OLED, and a small *Now playing* box in the top bar.
- Plex 1.0.4, Jellyfin 1.0.3, YouTube 1.0.4, Twitch 1.0.4, Seerr 1.0.4 and Live TV 1.0.4: the new look, sharper pictures and channel logos without a box.
- Plex 1.0.4: *Plex cast* is removed for now. The new Plex app for phones only casts to Chromecast, and app.plex.tv cannot reach the box, so it did not work from anything in use today. It will come back in a form that works.

## 1.0.4

> **In short:** Series pages in Plex and Jellyfin, a TV guide in Live TV, a start-up animation, and many fixes.
> - Plex and Jellyfin: seasons and episodes on one page, and *Play* picks the right episode
> - Live TV: a TV guide with your favourites, and a smooth fade when you change channel
> - Bluetooth and AirPlay work again on boxes that were updated
> - YouTube opens video info at once, and Twitch lets you choose the quality

<!-- tldr:da
Kort: Seriesider i Plex og Jellyfin, TV-guide i Live TV, en opstartsanimation og mange rettelser.
- Plex og Jellyfin: sæsoner og afsnit på én side, og *Afspil* vælger det rigtige afsnit
- Live TV: TV-guide med dine favoritter og en blød overgang, når du skifter kanal
- Bluetooth og AirPlay virker igen på bokse, der er blevet opdateret
- YouTube åbner info om en video med det samme, og på Twitch kan du vælge kvaliteten
-->

**System**
- A start-up animation shows until the menus appear.
- Bluetooth, AirPlay and the PC search in Games work again on boxes that were updated, and those boxes start about 20 seconds faster.
- The clock keeps trying until a time server answers. A late network could leave the box in 1970, so the App Store and updates failed.
- Home screen: move an app with the Green key, the App Store tile shows how many app updates there are, and Jellyfin has a *Continue watching* row.
- The row below the apps only shows for the app in focus (it followed along all the way to Settings).
- The App Store shows the real size of each app (it said 0.0 MB).
- Player: short names for audio tracks (YouTube showed a long address), and seeking speeds up a little more gently.

**Apps** (update in the App Store)
- Plex 1.0.3 and Jellyfin 1.0.2: a series page with seasons and episodes. Jellyfin can also search for subtitles.
- Live TV 1.0.3: a TV guide replaces *All channels*, the picture fades on a channel change, and it says when the provider's guide is down.
- YouTube 1.0.3: video info opens at once, the chosen quality is also used for the next video, and 1440p/4K are greyed out on a Raspberry Pi 4.
- Twitch 1.0.3: choose the quality in the player menu.
- Seerr 1.0.3: Netflix top lists on *Discover*.
- Games 1.0.1: finds your PC even when its firewall blocks the normal search, and keeps searching while the list is open.

## 1.0.3

> **In short:** Kernel updates finish again, the App Store updates all apps at once, and the box is safer and smoother.
> - Fixed: updates that change the kernel could never finish
> - *Update all* in the App Store, with a short note on what is new in each app
> - Plex cast, SSH and AirPlay mirroring now need your account or the box's own code
> - Updates switch the kernel in one step, so a power cut cannot leave the box unable to start
> - Smoother menus and playback: less waiting on the network and on the SD card

<!-- tldr:da
Kort: Kerneopdateringer bliver færdige igen, App Store opdaterer alle apps på én gang, og boksen er mere sikker og mere flydende.
- Rettet: opdateringer, der skifter kernen, kunne aldrig blive færdige
- *Opdatér alle* i App Store, med en kort note om hvad der er nyt i hver app
- Plex-cast, SSH og AirPlay-spejling kræver nu din konto eller boksens egen kode
- Opdateringer skifter kernen i ét trin, så et strømsvigt ikke kan efterlade en boks, der ikke starter
- Mere flydende menuer og afspilning: mindre ventetid på netværket og SD-kortet
-->

**Important fix for updates**
- Updates that change the kernel could never finish: the box said "Restart the box to finish the (previous) update" again and again, also after a restart. This affected updating from 1.0.0 to 1.0.1 and from 1.0.1 to 1.0.2.
- A box installed from the 1.0.1 or 1.0.2 image updates straight to 1.0.3. It may say "Restart the box to finish the update" once - restart, and it finishes by itself (it restarts one extra time to start the new kernel).
- A box that got 1.0.1 or 1.0.2 as an update is stuck and needs a one-time fix: see "A box stuck on an update" in the README.
- *Undo last update* works again for the kernel after such an update.

**App Store**
- The version an app shows is now the one it really has. Before, every app said 1.0.0 and offered the same update again and again, also right after it was installed, and asked for a restart each time.
- When apps have updates, a bar at the top of the App Store updates them all at once (*Update all*).
- Before an update, a card shows what is new in it. An app with an update says "New: ..." under its name.
- An app updated while it is open shows "1.0.2 after a restart" until the user interface is restarted.

**Updates**
- The update card and *Settings → System → Update NotaTV* show a short summary of what is new in the version, in your language.
- The kernel and its files are switched in one step: the new set is put next to the old one, and one small file says which to use. A power cut during an update leaves the old or the new version, never a box that cannot start. *Undo last update* switches back to the old set. (From the next update after 1.0.3: this version is still installed by the updater already on the box.)
- A version that is installed but waiting for a restart is not offered again.

**Security**
- Plex cast: playing, pausing and the remote in the Plex app only work from the Plex account the box is signed in with (or its Plex Home). Before, anyone on the network could use the box's remote control this way. Not signed in to Plex: nobody can.
- *Developer mode (SSH)*: every box makes its own random password, shown right below the setting. Before, the password was `notatv` on every box.
- AirPlay screen mirroring: the first time a phone mirrors, it asks for the box's 4-digit code, which the TV shows (and *Settings → Cast and sharing*).
- Plex and YouTube cast accept a limited number of connections at a time, so the box cannot be overloaded from the network.
- The phone page's code: at most 10 wrong codes in 10 minutes also when many are sent at the same time.
- Casting to the box (DLNA) only plays addresses on the network (http, https, rtsp), never files on the box.
- A USB stick can no longer be taken for the box's own data or boot partition, drives are mounted without running anything from them, and a stick named ".." is mounted safely.
- Bluetooth: new devices can only pair while *Pair* is open (before: also while a phone was connected).
- Signing out of Seerr also removes the session from memory. Signing out of Live TV also removes the channel list with the stream addresses from the box's temporary files.
- The troubleshooting log and the log download hide more: `user:password@`, the Live TV login in short stream addresses, every password and token saved in the settings, and the AirPlay code.
- The Twitch chat is read over an encrypted connection.

**Smoother**
- Text is drawn with much less work (kerning remembered, lines measured once), shadows are one texture, and images no longer stay in memory twice after they are on screen.
- Saving a setting no longer waits for the SD card on the user interface.
- While a video plays, the phone page, Bluetooth pairing and App Store installs keep working; the music pause, subtitle search and Plex's intro/credits markers no longer hold up playback.
- Plex: a slow server no longer freezes the box before and after a video.
- The home screen fetches its rows when it opens, after playback and then every 5 minutes (before every 8 seconds, also with the screensaver on), and the network address is read in the background.
- Live TV: big channel lists and program guides load much faster, and the guide is only downloaded again when it changed.

**Fixes**
- The update screens say "Everything you have set up stays as it is" instead of listing what is kept.
- *Next episode* no longer starts twice when another menu (e.g. *Power*) was opened during the countdown.
- The Plex sign-in screen could open new Plex screens again and again when the server list was opened while signing in.
- The waiting spinner in Settings (e.g. connecting to Wi-Fi) can no longer lock the box: after 30 seconds *Back* closes it.
- After *Back* during AirPlay mirroring, it does not start again until the phone stops mirroring.
- The poster on the pause screen and the channel logo in Live TV are shown again for images that were no longer in memory.
- A physical keyboard uses one layout: Danish (with æ, ø, å) when NotaTV is in Danish, otherwise US.
- Live TV: *Frame rate for live TV* can be *Do not change* (for channel lists that mix 50 and 60 Hz).
- The setup guide can go on while apps download (*Back*); they finish in the background.
- Password fields are hidden while you type, and network drive addresses are shown without the password.
- Wi-Fi uses the rules of the country chosen in the setup guide (before: always Denmark).

**Apps** (updated in the App Store): Live TV 1.0.2, Plex 1.0.2, YouTube 1.0.2, Twitch 1.0.2, Seerr 1.0.2, Media 1.0.1.

## 1.0.2

**Updates**
- Installing an update shows a progress bar: percent and megabytes while it downloads, then checking, unpacking and installing.
- When the update is installed, you choose *Restart now* or *Restart later*. If something goes wrong, the card says what.
- You see the new update screens from the next update after this one (the version installed on the box shows the update).

**Settings**
- *Sign out* is only shown when you are signed in (Plex, Jellyfin, Twitch, Seerr, YouTube, Live TV and OpenSubtitles). Apps with nothing to set up while signed out are left out of *Settings → Apps*.
- Live TV only shows *Switch account* when you have two or more accounts.
- Left on a setting that cannot be changed (e.g. *Always HDR* on a screen without HDR) no longer changes it and makes the screen go black for a moment.

**Playback**
- Jellyfin: when a video could not start, it no longer drops out of *Continue watching*, and it is not counted as watched.
- Jellyfin and Plex: a video that could not start no longer jumps to the next episode.
- When a server does not answer, the TV says so ("The server did not send ... in time") after about 20 seconds, instead of waiting almost a minute.

**Network**
- Wi-Fi power saving is turned off. On the Raspberry Pi 4 it made videos load slowly or not at all, even with a strong signal.
- Cable and Wi-Fi: when you pull out the cable, Wi-Fi takes over right away. When you plug it in, the box gets an address at once and uses the cable.
- Renewing the network address no longer interrupts a video that is playing.
- The address shown on the TV and for the phone page is the one of the connection in use.

**Troubleshooting**
- *Settings → System → Troubleshooting log* logs more details (every network request) and keeps the logs across restarts.
- *Settings → System → Get the logs*: download all logs as one file from the phone page, with passwords, tokens and logins removed. Nothing is sent anywhere by itself.

**Apps** (updated in the App Store): Live TV, Jellyfin, Plex, Seerr, Twitch and YouTube 1.0.1.
