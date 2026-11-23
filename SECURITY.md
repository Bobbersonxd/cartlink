# Security policy (Cartlink)

Cartlink is an unofficial fork of RetroArch. This policy covers
Cartlink's own parts: Load from URL (downloads, logins, game lists,
fingerprints, certificate checks, the URL cache), the home server kit in
`tools/cartlink-server/`, and the bundled mbedTLS copy.

## Reporting a problem

Please **don't open a public issue** for a security problem. Use GitHub's
private reporting instead: the repository's **Security** tab →
**Report a vulnerability**
(https://github.com/OWNER/cartlink/security/advisories/new).

Include what you found, how to reproduce it, and which version
(`cartlink --version`). Don't include real passwords, server names or
copyrighted files. You should get a first answer within 14 days. This is
a small volunteer project: there is no bug bounty.

Problems that are also in upstream RetroArch (cores, netplay, the menu,
other drivers) are best reported to RetroArch as well:
https://github.com/libretro/RetroArch/security

## Supported versions

Only the latest Cartlink release gets fixes.

## What to expect from Cartlink

- Cores are normal programs: they can read and write files and use the
  network. Only use cores you trust. Load from URL never downloads
  cores, only content files.
- Downloads: programs and scripts are refused, archives are checked
  before unpacking, https certificates are checked on every download,
  and a `#sha256=` fingerprint, when present, must match.
- Logins are sent only over https with a valid certificate. They are
  stored in `url_logins.cfg` in Cartlink's settings folder: plain text
  with mode 0600 on Linux/macOS, DPAPI-protected (current user) on
  Windows. Anyone who can read your settings folder as you can use them.
- Binaries are not code-signed.
