# Cartlink changes

Changes in Cartlink itself. RetroArch's own changelog is in
[CHANGES.md](CHANGES.md); Cartlink is based on RetroArch v1.22.2.

## 0.1.0 (not released yet)

First public version.

### Load from URL
- Load Content → Load from URL: download a game from a link and load it
  the normal way. Google Drive, Dropbox, OneDrive and archive.org share
  links become direct downloads. Ctrl+V / Shift+Insert paste.
- Logins for your own server or NAS (Basic and Digest), asked in a
  popup and saved per site; archive.org sign-in (access keys, not the
  password); Forget Saved Logins.
- URL cache with Clear URL Cache; links downloaded once load from the
  cache.
- Game lists: a folder link served as a listing (Caddy `file_server
  browse`) opens as a menu; Saved Servers.
- Command-line URLs: `cartlink -L <core> <link>`.
- Netplay with the same link on every peer (unchanged netplay).

### Safety
- Programs, scripts and shortcuts are refused, by content and by name.
  Plain DOS programs are allowed only inside .zip/.7z archives.
- Archives are checked before unpacking: no absolute or `..` paths, no
  programs, at most 2 GB unpacked, 10,000 members, 1000:1 ratio.
  Downloads are written to disk as they arrive.
- Optional `#sha256=<hex>` fingerprints on links, checked after every
  download and on every cache hit.
- Every https download checks the certificate; without a login the user
  may pick "Download anyway (unsafe)" for that one download.
- Logins are never sent over plain http; saved per site
  (`https://host:port`); protected with DPAPI on Windows; never uploaded
  by Cloud Sync.
- Windows: downloaded files get the "from the internet" mark.
- Bundled mbedTLS updated from 2.6.0 to 2.28.10.

### Identity
- Name Cartlink: program `cartlink` / `cartlink.exe`, window title, own
  icon, desktop entry `io.github.cartlink.Cartlink`.
- Own settings folder (`~/.config/cartlink`, `%APPDATA%\Cartlink` or the
  portable folder, `~/Library/Application Support/Cartlink`); saved
  logins and servers are copied once from the RetroArch folder of
  earlier test builds (never moved, `retroarch.cfg` never imported).
