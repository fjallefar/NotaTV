# Tools from the App Store

Programs the box uses that are not in the image. They are not shown in the App Store: the box fetches them by itself
with the catalog (at start and every 6 hours) and installs them in `/data/notatv/tools/<id>` (`apppkg tool-install`,
SHA256 from the catalog, like an app).

- **yt-dlp** - YouTube changes often, and an old yt-dlp stops working. `mediarun` runs it.
- **node** - Node.js, yt-dlp's fast JavaScript runtime for YouTube's challenge (about 5 s on a Pi 4; QuickJS, which
  stays in the image as a fallback, takes about 25 s). `/etc/yt-dlp.conf` points at `/data/notatv/tools/node/node`;
  yt-dlp skips it while it is not there.

Since F1 step 1 (audit 9/11, NotaTV 1.0.25) neither is in the image: together they took 165 MB of the 512 MB rootfs,
which left too little room for system updates. A box installs a tool when it has none or an older one (a box with an
older image that still has its own yt-dlp in `/usr/lib/yt-dlp` only takes a newer one). Until yt-dlp is downloaded,
YouTube says "yt-dlp is still being downloaded".

## Updating a tool without a system release

1. **yt-dlp:** pick the release on https://github.com/yt-dlp/yt-dlp/releases and take the SHA256 of
   `yt-dlp_linux_aarch64.zip` from its `SHA2-256SUMS`. **node:** take the SHA256 of `node-v<version>-linux-arm64.tar.xz`
   from `https://nodejs.org/dist/v<version>/SHASUMS256.txt`, and change the version in the two paths under `files`.
2. Put `version`, `url` and `sha256` in `<id>/manifest.json`.
3. Commit and push an `apps-*` tag. `scripts/apps-catalog.py` downloads it, checks the SHA256, packs it as
   `<id>-<version>-tool.tar.gz` (Node: only `bin/node` and `LICENSE`, stripped - `"strip": true`) and lists it under
   `"tools"` in the catalog (with its own SHA256, like the apps).
