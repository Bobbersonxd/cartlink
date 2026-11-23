# Cartlink

Cartlink is an **unofficial fork of [RetroArch](https://github.com/libretro/RetroArch)**
v1.22.2 that adds **Load from URL**: paste a link to a game file, Cartlink
downloads it and loads it the normal way (core choice, zip/7z handling,
history). Everything else, netplay included, works like RetroArch.

The point: friends who load the same link get the exact same file, so
normal netplay just works and nobody has to pass files around first.

Cartlink is not made, endorsed or supported by the libretro team. All
the emulation, menus and netplay are RetroArch's work; please report
Cartlink problems [here](https://github.com/OWNER/cartlink/issues), not
to libretro. Licence: GNU GPL v3 or later, the same as RetroArch
(see [COPYING](COPYING)). The files Cartlink changed are listed in
[NOTICE-CARTLINK.md](NOTICE-CARTLINK.md).

> **Only load games you own or have the right to use. Cartlink does not
> provide, link to or help find games.** It contains no games, BIOS files
> or links to them.

## What Cartlink adds

- **Load Content → Load from URL**: type a link or paste it with
  **Ctrl+V** / **Shift+Insert**. Share links from Google Drive, Dropbox,
  OneDrive and archive.org are turned into direct downloads; plain
  direct links work as they are.
- **Your own server or NAS**: a login popup for sites that ask for one,
  saved per site (`https://host:port`). Logins are **only ever sent over
  https with a valid certificate**, never over plain http.
- **archive.org sign-in** for items that need an account (uses
  archive.org's public API; respect their terms).
- **Game lists**: a link to a folder served as a listing (Caddy's
  `file_server browse`) opens as a menu of games and subfolders. Folders
  you open are kept under **Saved Servers**.
- **Fingerprints**: a link may end with `#sha256=<64 hex digits>`. The
  file must match it or it is refused.
- **Safety checks** on every download: programs and scripts are
  refused ("Blocked: this link gives a program, not a game."), archives
  are checked before they are unpacked (no unsafe paths, no programs,
  at most 2 GB unpacked), and every https download checks the site's
  certificate.
- **URL cache**: a link downloaded once loads from the cache next time.
  **Settings → Network → Clear URL Cache** empties it.
- **Command line**: `cartlink -L <core> https://example.com/game.sfc`.
- **Netplay**: everyone loads the same link, then hosts / joins as usual.
- Its own settings folder, so it never changes a RetroArch install's
  settings (see [Where things are saved](#where-things-are-saved)).
- A home server kit: [tools/cartlink-server/SETUP.md](tools/cartlink-server/SETUP.md).

## How to use

### Load a game from a link
1. **Load Content → Load from URL**.
2. Type the link, or paste it with **Ctrl+V** or **Shift+Insert**.
   (On Linux, paste needs `wl-paste`, `xclip` or `xsel`; most desktops
   have one.)
3. Press Enter. "Downloading ..." shows while it downloads, then a core
   is picked (or you are asked) just like for a file on disk.

Google Drive links get Google's "can't scan this file for viruses" page
skipped on purpose (that is how big files download); Cartlink's own
program and archive checks still run.

### Fingerprints (`#sha256=`)
Whoever shares a link can add the file's SHA-256 so everyone is sure to
get exactly that file:

```
https://example.com/games/mygame.gb#sha256=3f2a...  (64 hex digits)
```

The part after `#` is never sent to the server. If the downloaded (or
cached) file has a different SHA-256, it is deleted and refused:
"Blocked: this file is not the one that was shared (fingerprint
mismatch)." Make the fingerprint with `sha256sum mygame.gb` (Linux),
`shasum -a 256` (macOS), `certutil -hashfile mygame.gb SHA256` (Windows),
or the server kit's `cartlink-link` helper, which prints the whole link.

### Certificates
Every https download checks the site's security certificate. If it is
not valid you see "This site's security certificate is not valid.
Someone may be changing the file on the way." with **Cancel** and
**Download anyway (unsafe)**. *Download anyway* is offered only for
downloads without a login, for that one download. A link with a
fingerprint is downloaded and then must match its fingerprint. The
command line never offers *Download anyway*.

### Your own server or NAS
If a site asks for a login, a popup asks for the username and then the
password. If it works, the login is saved for that site and used next
time. If the password is wrong, it says so and asks again.

Logins (and archive.org keys) are only sent over https to a site whose
certificate is valid. A server that asks for a password over plain http
gets: "This server asks for a password without encryption (http). Use
its https:// address." A NAS with a self-signed certificate: give it a
real certificate (for example Caddy's automatic HTTPS), or use links
without a login.

### Game lists and Saved Servers
A link that ends in `/` and points at a folder listing opens a game
list: pick a folder to go deeper, a file to download it. Cartlink reads
the listing format of Caddy's `file_server browse`. Servers you open are
kept under **Load Content → Saved Servers** (only the address, never the
login) and can be removed there.

To share your own games with friends over the internet (one link, a
login for each friend, a list of all games), see the server kit:
[tools/cartlink-server/SETUP.md](tools/cartlink-server/SETUP.md).
Only put files on a server that you have the right to share.

### archive.org account
**Settings → Network → archive.org Account**, then your email and
password. Cartlink keeps archive.org's access keys, not your password.
Anyone with these keys can use your archive.org account, so use
**Forget Saved Logins** on shared computers.

### Forget saved logins
**Settings → Network → Forget Saved Logins** removes every saved login
(sites and archive.org) from Cartlink's own folder.

If you upgraded from a build that used RetroArch's folder, your old
logins were *copied* (never moved) and the old file is still there in
plain text. Cartlink never deletes RetroArch's files, so delete it
yourself once Cartlink works:
- Linux: `~/.config/retroarch/url_logins.cfg` (or
  `$XDG_CONFIG_HOME/retroarch/url_logins.cfg`)
- macOS: `~/Library/Application Support/RetroArch/config/url_logins.cfg`
- Windows: `%APPDATA%\url_logins.cfg`, or `url_logins.cfg` next to the
  old `retroarch.exe` of a portable test build

### Clear URL Cache
**Settings → Network → Clear URL Cache** deletes the downloaded games,
so links download again. Use this if a friend's copy doesn't match yours.

### Command line
```sh
cartlink -L <core> https://example.com/path/game.sfc
```
It downloads (or uses the cache), then starts the game. The command line
can't ask for a login: open a login-protected link once from the menu
(so the login is saved), then the command line can use it. Folder links
(game lists) open only from the menu.

### Netplay
Everyone loads the **same link** (with the same fingerprint, if any),
then uses normal RetroArch netplay (host / join). Because everyone has
the same bytes, the game check matches.

## Where things are saved

Cartlink has its own settings folder (it keeps RetroArch's file names
inside, such as `retroarch.cfg`):

| System  | Folder |
|---------|--------|
| Linux   | `~/.config/cartlink` (or `$XDG_CONFIG_HOME/cartlink`) |
| Windows | the folder `cartlink.exe` is in (portable), else `%APPDATA%\Cartlink` |
| macOS   | `~/Library/Application Support/Cartlink` |
| Android | not changed yet (an Android build is planned later) |

- **Logins:** `url_logins.cfg` in that folder, kept apart from
  `retroarch.cfg`. On Linux and macOS it is plain text with owner-only
  permissions (0600). On Windows every value is protected with DPAPI
  for your Windows user: if you move a portable folder to another PC
  or another Windows user, the saved logins can't be read there and
  must be typed again.
- **The settings folder holds your passwords: don't share it** (or zip
  it up for a friend). Cloud Sync never uploads the logins or servers
  files.
- **Saved Servers:** `url_servers.cfg` (addresses only).
- **Downloads:** `url_cache` inside the Downloads folder (Settings →
  Directory). Delete files there to free space.

### Cores and assets

Cartlink's folder starts empty: no cores, menu icons or fonts. Either
- use **Main Menu → Online Updater**: Update Assets, Update Core Info
  Files, then Core Downloader for the cores you want; or
- reuse what an installed RetroArch already has: **Settings → Directory**
  and point **Cores** (and **Core Info**, **Assets**) at RetroArch's
  folders, for example `~/.config/retroarch/cores` on Linux, or
  `/usr/lib/libretro` for distro packages. On Linux, menu assets of a
  distro RetroArch in `/usr/share/retroarch` are found automatically.

Shared folders are shared both ways (a core Cartlink downloads there is
then RetroArch's too); RetroArch's settings file is never changed.

**Upgrading from a build that used RetroArch's folder** (for example an
earlier Cartlink / "Retro Link" test build): on first start, if
Cartlink's folder has no `url_logins.cfg` / `url_servers.cfg` but the
RetroArch folder (`~/.config/retroarch` on Linux) has them, they are
**copied** over once. The RetroArch files are never moved or deleted, and
`retroarch.cfg` is never imported.

## Build

### Linux
```sh
./configure --disable-systemmbedtls --enable-builtinmbedtls --disable-ffmpeg
make -j"$(nproc)"
```
The result is the `cartlink` program in the repo folder.

- `--enable-builtinmbedtls`: Cartlink bundles mbedTLS 2.28.10 (see
  `deps/mbedtls/CARTLINK-VERSION.txt`); current distros ship mbedTLS
  4.x, which this code base does not support.
- `--disable-ffmpeg`: RetroArch 1.22.2's FFmpeg player core does not
  build with FFmpeg 8.x.
- `--disable-networking` builds without Load from URL (and without
  netplay), as in RetroArch.

Typical build packages (Debian/Ubuntu names): `build-essential
pkg-config libx11-dev libxext-dev libxrandr-dev libgl-dev libegl-dev
libasound2-dev libpulse-dev libudev-dev libwayland-dev
wayland-protocols libxkbcommon-dev zlib1g-dev`.

`make install` installs only Cartlink's own files (binary, desktop
entry `io.github.cartlink.Cartlink`, icon, docs), so it can sit next to
an installed RetroArch.

### Windows (cross-built on Linux)
Needs MinGW-w64 (`x86_64-w64-mingw32-gcc`), `rsync` and `zip`.
```sh
tools/build_windows.sh            # BUILD_DIR=... OUT_DIR=... to choose folders
```
It builds a static 64-bit `cartlink.exe` (Windows 10 or newer) in a
separate copy of the sources and packs `cartlink-win64.zip` with the
licence and a short README. No assets, core info files or cores are
included: on first run use **Main Menu → Online Updater** (Update
Assets, Update Core Info Files, then Core Downloader for the cores you
want). Windows SmartScreen may warn about an unknown app: **More info →
Run anyway**.

### Tests
```sh
make -C tests-other/url_content test fetch-test   # unit + local server tests
make -C tests-other/url_content win-test          # Windows logins under Wine
tools/url_e2e.sh                                  # end to end (headless)
tools/url_e2e_catalog.py                          # game list menus (headless gamescope)
```
The tests use throwaway folders and local test servers only; they
never read or write your real settings.

## Limits
- Files bigger than 2 GiB are refused.
- Google Drive, Dropbox and OneDrive: public share links only.
- The password is visible while you type it.
- On the command line, download progress shows only in the log.
- On builds with the FFmpeg or mpv player, a link given with no core
  (`cartlink https://...` without `-L`) is passed to the player as a
  stream, as RetroArch does.

## Security
See [SECURITY.md](SECURITY.md) for how to report a security problem.

## Credits
Cartlink is built on RetroArch by the libretro team and its many
contributors (see [AUTHORS.h](AUTHORS.h) and RetroArch's history).
The RetroArch and libretro names belong to their owners; Cartlink uses
them only to say what it is based on.
