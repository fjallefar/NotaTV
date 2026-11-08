# Self-hosting

Many NotaTV apps are clients for software you run yourself, on a PC, a NAS or a small home server.
This page says, per app, what it talks to and where that software lives, so you can host it yourself.
NotaTV only looks for servers on your own network (see [Apps](README.md#apps)).

| App | What you host | Project | Notes |
|---|---|---|---|
| **Jellyfin** | Jellyfin server - your own movies and shows | [jellyfin/jellyfin](https://github.com/jellyfin/jellyfin) | Open source. Default port 8096. Found on the network by itself (UDP 7359). |
| **Plex** | Plex Media Server | [plex.tv/media-server-downloads](https://www.plex.tv/media-server-downloads/) | Free, but not open source, and sign-in goes through plex.tv. Default port 32400. Found by itself (GDM). |
| **Seerr** | A request server for movies and shows | [seerr-team/seerr](https://github.com/seerr-team/seerr) | Open source. Overseerr and Jellyseerr work too. Default port 5055. You sign in with your Plex account. |
| **Flimmer** | Flimmertv - requests, movies and series in one server | - | Default port 7676, found on the network by itself. You sign in with your Flimmertv email and password (accounts that only use Plex or Jellyfin sign-in cannot). |
| **Games** | Sunshine on the PC you play on | [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) | Open source. The fork [Apollo](https://github.com/ClassicOldSong/Apollo) should work too (not tested). The firewall must allow TCP 47984-48010 and UDP 47998-48000. When pairing, type the PIN on `https://localhost:47990` on the PC itself - Sunshine rejects it when the page is opened on the PC's IP address (CSRF protection). |
| **Tesla** | TeslaMate - logs your Tesla's data | [teslamate-org/teslamate](https://github.com/teslamate-org/teslamate) | Open source. NotaTV reads TeslaMate's MQTT server (mosquitto, port 1883), read only. In docker-compose, mosquitto needs `ports: - 1883:1883`, or the box only finds the web page on port 4000. |
| **Live TV** | An M3U playlist (and XMLTV guide) or an Xtream Codes login | [tvheadend/tvheadend](https://github.com/tvheadend/tvheadend), [Threadfin/Threadfin](https://github.com/Threadfin/Threadfin), [Dispatcharr/Dispatcharr](https://github.com/Dispatcharr/Dispatcharr) | Tvheadend turns a TV tuner (DVB-T/C/S) into a playlist; Threadfin and Dispatcharr tidy and proxy playlists you already have. Free public channels: [iptv-org/iptv](https://github.com/iptv-org/iptv), guides: [iptv-org/epg](https://github.com/iptv-org/epg). |
| **Media** | A network drive (SMB) | [Samba](https://www.samba.org/) | Any NAS or PC sharing a folder works, e.g. `smb://user:password@server/share`. USB drives need nothing. |
| **Radio** | Nothing - uses the public radio-browser.info | [radiobrowser-api-rust](https://gitlab.com/radiobrowser/radiobrowser-api-rust) | The station directory is open source and run by volunteers. |
| **Music** | Nothing - the box is the AirPlay/Bluetooth receiver | [shairport-sync](https://github.com/mikebrady/shairport-sync) | Runs on the box itself. |

**Not self-hosted:** YouTube, Twitch and TV 2 Play are online services; NotaTV is only a client for them.
Subtitle search in Plex/Jellyfin uses opensubtitles.com (free API key, see [Apps](README.md#apps)).
