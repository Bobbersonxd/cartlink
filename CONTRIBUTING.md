# Contributing to Cartlink

Cartlink is an unofficial fork of RetroArch that adds Load from URL. It
is a small volunteer project: thank you for helping. Please follow the
[code of conduct](CODE_OF_CONDUCT.md).

## Reporting bugs and ideas

- Cartlink problems (Load from URL, game lists, Saved Servers, logins,
  the server kit in `tools/cartlink-server/`, the release builds): open
  an issue at https://github.com/OWNER/cartlink/issues and fill in the
  template. Include `cartlink --version`.
- Security problems: not as a public issue, see [SECURITY.md](SECURITY.md).
- Problems that are also in upstream RetroArch or in a core (netplay,
  video/audio drivers, a core's emulation) belong to RetroArch or to that
  core. Check them with the latest RetroArch first.
- Never post links to, or ask for, copyrighted games (ROMs) or BIOS
  files. Such issues are closed. Don't include real passwords or server
  names either.

## Pull requests

- Keep a change small and about one thing; say what it changes and how
  you tested it.
- Follow the style of the file you change (RetroArch's: C89, three-space
  indents, no `//` comments in C files).
- Build as in the README ("Build"), then run the tests. They use
  throwaway folders and local test servers only:

  ```sh
  make -C tests-other/url_content test fetch-test
  make -C tests-other/url_content win-test   # needs MinGW-w64 and Wine
  tools/url_e2e.sh                           # end to end, headless
  ```
- New files carry "Copyright (C) 2026 Cartlink contributors" and the GPL
  v3 or later notice. An upstream file you change for the first time
  gets the line "Modified by Cartlink contributors, 2026" near the top;
  then regenerate `NOTICE-CARTLINK.md` with `tools/cartlink_notice.py`.
- By contributing you agree that your work is published under the GNU
  GPL v3 or later, like the rest of Cartlink.

## Credits

Cartlink is built on [RetroArch](https://github.com/libretro/RetroArch)
by the libretro team and its many contributors. Cartlink is not made,
endorsed or supported by them, so please don't send Cartlink reports to
RetroArch or libretro.
