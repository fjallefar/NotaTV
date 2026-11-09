# Open source and privacy

[Back to the README](README.md).

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
| api.energidataservice.dk | Electricity price (prices at startup, every 6 hours and at 13:15; grid tariffs every 6 hours) |
| Your TeslaMate's MQTT server (local network) | Tesla |
| tile.openstreetmap.org | Tesla → Map (off until you turn it on) |
| api.opensubtitles.com | Subtitle search in the player (the title or TMDB id and your languages; your login when you download) |
| itunes.apple.com, musicbrainz.org, coverartarchive.org | Cover art for Bluetooth music (artist and album or track; can be turned off) |
| api.github.com, github.com | Updates and the App Store |
| pool.ntp.org | Setting the clock at startup |
