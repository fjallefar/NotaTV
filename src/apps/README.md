# NotaTV apps

Everything on the NotaTV home screen is an app. The system (`src/shell`) has the basic parts that every app uses:
drawing, the player, the on-screen keyboard and dialogs, network calls, settings, jobs and translations. An app only
adds its own screens and logic.

Every app is a package, i.e. a folder here in `src/apps/`: Live TV (`iptv`), Plex, Jellyfin, YouTube, Twitch,
Seerr, Music, Games, Tesla and Media. Each one is built into its own `app.so`. Only Settings and the App Store are part of
`tvshell` itself. The image comes without apps: they are downloaded from the App Store (the GitHub release `apps`)
into `/data/notatv/apps-pkg/<id>/`.

Every app describes itself with a `NotaApp` (see `src/shell/tv.h`). The app register (`src/shell/apps.c`) uses it
for the home screen, the App Store, *Settings → Apps*, background services, phone-page messages and uninstalling.

## A package

```
src/apps/<id>/
  manifest.json     id, version, api + what the App Store shows before the app is installed:
                    name, desc, icon (tv, plex, youtube, play, star, music, film, folder ...), c1/c2/fg colors,
                    order and default (was on by default in NotaTV 1.0; only used to move
                    1.0 boxes to App Store apps - nothing is ticked beforehand in the setup guide),
                    and changes: what is new in this version (see below)
  *.c, <id>.h       the app (any number of files; <id>.h has what the files share)
  lang/<code>.po    the app's translations (e.g. lang/da.po)
  app.mk            optional: extra compiler flags as CFLAGS_<id>
```

The installed package is `manifest.json`, `app.so` and `lang/`.

- `id` contains only lowercase letters, digits, `-` and `_`. It must match the folder name.
- `version` is the app's own version. The code gets it as `APP_VERSION` (src/apps/Makefile reads it from the
  manifest), so write `APP_VERSION` in `nota_app` - never a version number.
- `api` is `NOTA_API` from `tv.h`. If a package is built for another API version, it is skipped when NotaTV starts.
- Make every function `static` except the descriptor. The package then exports only `nota_app`.

## The descriptor

```c
#include "tv.h"

static Screen *my_screen(void) { ... }
static void my_forget(void) { ... }          /* delete everything saved: tokens, files, cfg keys */

static void act_logout(void) { app_ask_signout("My app", my_forget); }
static const char *signed_out(void) { return *cfg_get("myapp_token", "") ? NULL : ITEM_HIDDEN; }
static const Item my_settings[] = {
    {N_("Sign out of My app"), IT_ACTION, .action = act_logout, .locked = signed_out},
};

const NotaApp nota_app = {
    NOTA_API, "myapp", "My app", N_("One line about what it does"), APP_VERSION, 55, 0,
    ICON_PLAY, {145, 70, 255, 255}, {88, 38, 170, 255}, {255, 255, 255, 255},
    .open = my_screen, .forget = my_forget,
    .settings = my_settings, .n_settings = sizeof my_settings / sizeof my_settings[0],
};
```

| Field | Purpose |
|---|---|
| `order` | Position on the home screen. Live TV is 10, Plex 20, Jellyfin 30, YouTube 40, Twitch 50, Seerr 60, Music 70, Games 80, Tesla 85, Media 90. |
| `flags` | `APP_DEFAULT_OFF`: not installed from the start. `APP_ROW_LOGOS`: the home row shows logos. |
| `init` | Runs once per run of NotaTV: when it starts, or when the app is installed. Not again if the app is uninstalled and installed again before a restart. |
| `tick` | Runs every frame while the app is installed. Use it for background services. |
| `apply` | Runs when the app is installed or uninstalled. Start or stop background services here, and check `app_on("<id>")`. |
| `forget` | Called on sign-out and on uninstall. It must delete everything saved for the account, and leave the app ready to be used again (what `init` made stays). |
| `settings` | Shown under *Settings → Apps → name*. When the app is uninstalled, the keys are reset to their defaults. An item's `locked` returns a text to show it locked, or `ITEM_HIDDEN` to hide it (e.g. *Sign out* while nobody is signed in). An app whose items are all hidden is left out of the list. |
| `row`, `row_title`, `row_open`, `row_refresh` | The row under the apps on the home screen, e.g. "Continue watching". |
| `shelf_label`, `draw_shelf` | The large area at the top of the home screen when the app has focus. |
| `inbox` | A message from the phone page (`/tmp/notatv-inbox/_<name>`). Return true when the app takes it. YouTube uses it for accounts sent from the phone. |
| `iface` | Functions the app offers the system or other apps. They are reached with `app_iface("<id>")`, which returns NULL when the app is not installed. |

## Between apps

Apps never call each other directly. A package cannot see the functions of another package. Instead an app offers
an interface struct (declared in `tv.h`), and the other side asks the register for it:

```c
const SeerrIface *seerr = app_iface("seerr");   /* NULL: Seerr is not installed - skip the feature */
if (seerr) seerr->ratings(false, tmdb, got_ratings, ctx);
```

Plex uses Seerr's ratings this way. The system's Bluetooth code sends "Now playing" to Music (`MusicIface`) the
same way.

Keep your own files in `app_data(&nota_app)`, i.e. `/data/notatv/apps/<id>/`. When the app is uninstalled, that
folder is wiped. Settings go through `cfg_get`/`cfg_set`. Prefix the keys with something unique to the app.

## Building

```
make -C src/apps                     # every package -> src/apps/out/<id>/
TVSHELL_APPS=$PWD/src/apps/out scripts/uitest.sh /tmp/t "w:60;s:home;q"
python3 scripts/i18n-check.py        # also checks each package's lang/*.po
```

The Buildroot package `tvshell` builds the packages together with `tvshell` (into its build folder, not into the
image). The release workflow then runs `scripts/apps-catalog.py`, which packs each app as
`<id>-<version>-api<N>.tar.gz` and writes the catalog `apps-api<N>.json` (with translated names and descriptions
from `lang/*.po`), and uploads them to the release `apps`. Raise `version` in `manifest.json` when an app changes,
so boxes see *Update* in the App Store. Keep `name`, `desc`, `icon`, colors and `order` the same as in `nota_app`.

When you raise `version`, write what is new in `changes` - one very short summary and a few points. The App Store
shows it on the app ("New: ...") and on the card before the update, and *Update all* lists it per app:

```json
"changes": {
  "summary": "Casting from the Plex app needs your Plex account",
  "points": ["Nobody else on the network can control the box through Plex", "Fewer freezes when a server is slow"]
}
```

Translate the texts in `lang/<code>.po` like the app's other texts (`scripts/i18n-check.py` finds them).
Without `changes`, *Update* installs right away.

To try the App Store locally: `python3 scripts/apps-catalog.py src/apps/out /tmp/dist`, then run tvshell with
`TVSHELL_APPS=/nonexistent TVSHELL_APPCAT_URL=file:///tmp/dist`. `tvshell` is linked with `-rdynamic`, so a package can call every function in `tv.h`, and
cJSON as well.

## Look and feel

Use the system's helpers, so every app looks and moves the same (all are `NOTA_NEW`; the `ui_*` wrappers are
inline and fall back to the old look on an older NotaTV, so they can be called without checks):

```c
/* a card (poster, tile, button) that can have focus: it lifts with a spring, a shadow and a soft glow */
float f = ui_focus(item, focused);               /* 0..1, animated; the key is anything stable per card */
float sc = 1 + 0.07f * f, w2 = w * sc, h2 = h * sc, x2 = x - (w2 - w) / 2, y2 = y - (h2 - h) / 2;
ui_focus_under(x2, y2, w2, h2, 14, f);
/* ... draw the card at x2, y2, w2, h2 ... */
ui_focus_over(x2, y2, w2, h2, 14, f);
```

- `slide_to(key, target)`: a position that glides (for a white pill that moves between rows).
- `draw_panel(x, y, w, h, r, alpha)`: a dialog or panel in dark glass.
- `img_get_logo(url)`: a channel logo cleaned to stand on the dark background without a box.
- `app_set_origin(x, y, w, h, r)` right before `app_push()`: the new screen grows out of that card and shrinks back
  into it on Back.
- The side menu (`side_*`) and dialogs (`confirm_open`, `choice_open` ...) already have the look.
- The app's icon on the home screen and in the App Store is `src/shell/assets/icons/<id>.png`, made by
  `scripts/gen-icons.py` (add a drawing there for a new app; without one the tile uses `icon`, `c1`, `c2`, `fg`).
- Keep 96 px from the left screen edge, like the rest of the system.
- `app_set_overlay(id, draw)`: a small layer on top of every screen. In the top bar, get the x from
  `app_overlay_place(right, w)` (NotaTV 1.0.6+), so overlays from different apps stand side by side.
- `gfx_drift_begin(x, y, w, h)` ... `gfx_drift_end(ox, oy)` (NotaTV 1.0.6+): burn-in protection that glides - what is
  drawn in between is shown moved by fractions of a pixel. Check that they are there (`if (gfx_drift_begin)`).

## Rules

- Bump `NOTA_API` in `tv.h` whenever something an app uses changes: a struct, a function signature or an enum.
  Packages for the old version are then skipped instead of crashing.
- A new function can be added to `tv.h` without bumping `NOTA_API`: mark it `NOTA_NEW` (a weak symbol). An app
  that calls it must check it first (`if (app_close) app_close(s); else ...`), because a box with an older NotaTV
  (same `NOTA_API`) gives NULL. Otherwise the package cannot be loaded there at all.
- A package is never unloaded while `tvshell` runs. An uninstalled app stays in memory until the next start, so
  jobs and threads still running cannot call code that is gone.
- A package is checked before it is used: its SHA256 must match the catalog, and it may only hold plain files and
  folders (`/usr/libexec/notatv/apppkg`). An old version is replaced by renames and deleted, never overwritten,
  because tvshell keeps the loaded `app.so` mapped until it restarts.
