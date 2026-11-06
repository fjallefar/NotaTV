# Tools from the App Store

Programs the box uses that change more often than NotaTV itself. They are not shown in the App Store: the box
fetches a newer one by itself (with the catalog, every 6 hours) and `mediarun` uses it instead of the one in the image.

Today only **yt-dlp** (YouTube changes often, and an old yt-dlp stops working).

## Updating yt-dlp without a system release

1. Pick the release on https://github.com/yt-dlp/yt-dlp/releases and take the SHA256 of `yt-dlp_linux_aarch64.zip`
   from its `SHA2-256SUMS`.
2. Put `version`, `url` and `sha256` in `yt-dlp/manifest.json`.
3. Commit and push an `apps-*` tag. `scripts/apps-catalog.py` downloads the zip, checks the SHA256, packs it as
   `yt-dlp-<version>-tool.tar.gz` and lists it under `"tools"` in the catalog (with its own SHA256, like the apps).

A box takes it only if it is newer than the yt-dlp in its image (`/usr/lib/yt-dlp/notatv-version`), and installs it in
`/data/notatv/tools/yt-dlp`. Node.js and QuickJS (yt-dlp's JavaScript) and `/etc/yt-dlp.conf` stay the image's.
When the image gets the same or a newer yt-dlp (`external/package/yt-dlp`), the one in the image is used again.
