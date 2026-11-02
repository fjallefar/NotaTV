# Changelog

What is new in each NotaTV version. The section for a version becomes the text of its GitHub release
(`.github/workflows/release.yml` takes everything from `## <version>` to the next `## `).

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
