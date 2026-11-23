#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Cross-builds the 64-bit Windows cartlink.exe with MinGW-w64 on Linux
# and packs it into a zip.
#
# The build runs in a separate folder (a copy of the sources), so the
# Linux build in this repo is never touched. Builds are incremental:
# only changed files are copied, and configure only re-runs when the
# options below change.
#
# What is in the build: networking + HTTPS (bundled mbedTLS), menus
# (Ozone, XMB, MaterialUI, RGUI), video (OpenGL, Vulkan, D3D9/10/11/12,
# GDI), audio (XAudio2, WASAPI, DirectSound), input (DirectInput,
# XInput, raw input), zip/7z, slang shaders (bundled glslang and
# SPIRV-Cross). Everything is linked statically: no MinGW DLLs.
# Left out (no MinGW copies of these on the build machine): SDL/SDL2,
# FreeType (the built-in STB font is used), FFmpeg, Cg, Qt, Discord.
#
# Usage: tools/build_windows.sh
#   BUILD_DIR=<dir>   default: <repo>/../build-win
#   OUT_DIR=<dir>     default: <repo>/../builds
#   JOBS=<n>          default: nproc
# Needs: x86_64-w64-mingw32-gcc (pacman: mingw-w64-gcc), git, rsync, zip.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$(dirname "$ROOT")/build-win}
OUT_DIR=${OUT_DIR:-$(dirname "$ROOT")/builds}
JOBS=${JOBS:-$(nproc)}
HOST=x86_64-w64-mingw32
SRC=$BUILD_DIR/src
ZIP=$OUT_DIR/cartlink-win64.zip

for tool in $HOST-gcc $HOST-g++ $HOST-strip git rsync zip; do
   command -v "$tool" >/dev/null 2>&1 || { echo "missing: $tool" >&2; exit 1; }
done

LOW="nice -n 19"
command -v ionice >/dev/null 2>&1 && LOW="$LOW ionice -c3"

CONFIGURE_OPTS="--host=$HOST \
 --disable-qt --disable-sdl --disable-sdl2 --disable-ffmpeg --disable-cg \
 --disable-freetype --disable-discord \
 --enable-builtinzlib --enable-networking --enable-ssl \
 --disable-systemmbedtls --enable-builtinmbedtls"
STATIC_LDFLAGS="-static-libgcc -static-libstdc++ -static"

echo "== Copying sources to $SRC"
mkdir -p "$SRC" "$OUT_DIR"
# Tracked files plus new, not-ignored ones, as they are on disk now
# (uncommitted edits included). Deleted files are skipped.
(cd "$ROOT" && git ls-files -z --cached --others --exclude-standard) \
   | (cd "$ROOT" && while IFS= read -r -d '' f; do
        [ -e "$f" ] && printf '%s\0' "$f"; done) \
   | rsync -a --from0 --files-from=- "$ROOT/" "$SRC/"

STAMP="$SRC/.cartlink-configure"
WANT="$CONFIGURE_OPTS LDFLAGS=$STATIC_LDFLAGS"
if [ ! -f "$SRC/config.mk" ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ]; then
   echo "== Configuring"
   # shellcheck disable=SC2086
   (cd "$SRC" && LDFLAGS="$STATIC_LDFLAGS" CROSS_COMPILE=$HOST- \
      $LOW ./configure $CONFIGURE_OPTS > "$BUILD_DIR/configure.log" 2>&1) \
      || { tail -20 "$BUILD_DIR/configure.log"; exit 1; }
   printf '%s' "$WANT" > "$STAMP"
fi

echo "== Building (log: $BUILD_DIR/make.log)"
(cd "$SRC" && $LOW make -j"$JOBS" > "$BUILD_DIR/make.log" 2>&1) \
   || { grep -E 'error' "$BUILD_DIR/make.log" | head -20; exit 1; }

if $HOST-objdump -p "$SRC/cartlink.exe" | grep -qiE 'DLL Name: lib(gcc|stdc|winpthread)'; then
   echo "cartlink.exe still needs MinGW DLLs; static linking failed" >&2
   exit 1
fi

echo "== Packing $ZIP"
STAGE="$BUILD_DIR/stage/Cartlink"
rm -rf "$BUILD_DIR/stage"
mkdir -p "$STAGE"
$HOST-strip -o "$STAGE/cartlink.exe" "$SRC/cartlink.exe"
cp "$ROOT/COPYING" "$STAGE/COPYING.txt"
# Windows line endings for Notepad.
sed 's/$/\r/' > "$STAGE/README-Cartlink.txt" <<'EOF'
Cartlink, 64-bit Windows
An unofficial fork of RetroArch 1.22.2 (not affiliated with libretro)
that adds Load from URL. Licence: GNU GPL v3 or later (COPYING.txt).

1. Unzip this folder anywhere (not inside Program Files) and run cartlink.exe.
   Settings, cores and downloads are kept in this folder.
   This folder holds your saved logins (url_logins.cfg): don't share it.
   Logins are protected for your Windows user; on another PC or user
   they are lost and must be typed again.
2. First run: Main Menu > Online Updater
     - Update Assets            (menu icons and fonts)
     - Update Core Info Files
     - Core Downloader          (pick the cores you need)
3. Load Content > Load from URL, paste a link with Ctrl+V, press Enter.

Netplay: everyone loads the same link, then host / join as usual.
Only load games you own or have the right to use. Cartlink does not
provide, link to or help find games.

If Windows SmartScreen warns about an unknown app: More info > Run anyway.
Needs Windows 10 or newer.
EOF
rm -f "$ZIP"
(cd "$BUILD_DIR/stage" && zip -q -9 -r "$ZIP" Cartlink)
echo "== Done: $ZIP"
unzip -l "$ZIP"
