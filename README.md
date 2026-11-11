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

## What it does

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
- A **phone page** for typing, remote control and setup, connected with a QR code and encrypted
- **English and Danish** user interface

Platform: **Raspberry Pi 4 and Pi 5** (one image for both; the Pi 5 support is new and not fully tested yet).

**Not supported:** Netflix, Disney+, HBO Max and other services that need closed DRM, and there is no Spotify app (play it through AirPlay or Bluetooth). See [Limitations](GUIDE.md#limitations).

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

**First start:** a setup guide walks you through six short steps:
1. **language**
2. **network** (cable or Wi-Fi - with both, the cable is used while it is plugged in, and Wi-Fi takes over when you pull it out)
3. **country and region** for the clock (daylight saving time follows the region)
4. the box's **name** (NotaTV, Living room, Bedroom … or your own)
5. which **apps** to install
6. the **home screen**: a grid (the default) or a carousel

At the end it shows a summary with the phone page address and code. Everything is chosen on the box and nothing is looked up online. You can run the guide again under *Settings → System*.

## Documentation

| | |
|---|---|
| [Guide](GUIDE.md) | Remote control, phone page, every app, casting, picture and sound, updating, troubleshooting |
| [Self-hosting](SELF-HOSTING.md) | The server software the apps talk to |
| [Privacy](PRIVACY.md) | What is open source, and what the box contacts and when |
| [Development](DEVELOPMENT.md) | Building, releases, the code, adding a language |
| [Changelog](CHANGELOG.md) | What is new in each version |

## License

NotaTV's own code is open source. The licenses declared in the Buildroot packages are:
- the user interface (`src/shell`, tvshell) and `rpi-tools`: **MIT**
- the game streaming client (`src/moon`, tvmoon): **GPL-3.0-or-later**, because it links to libgamestream (GPL-3.0+)

Third-party components keep their own licenses. Examples are the Inter font (SIL OFL, `src/shell/assets/Inter-LICENSE.txt`), the time zone names from iso-codes (LGPL-2.1) and tzdata (public domain).
