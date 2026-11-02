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
