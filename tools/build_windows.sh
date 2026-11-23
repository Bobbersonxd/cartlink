#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Cross-builds the 64-bit Windows cartlink.exe with MinGW-w64 on Linux
# from the committed sources (`git archive HEAD`) and writes:
#   cartlink-<version>-win64.zip      cartlink.exe, cacert.pem,
#                                     COPYING.txt, THIRD-PARTY-NOTICES.txt,
#                                     SOURCE.txt, README-Cartlink.txt
#   cartlink-<version>-source.tar.gz  the exact source it was built from
# A tree with uncommitted or untracked files is refused; --dirty builds
# it anyway for a local test and names both files -DIRTY (see
# tools/release_common.sh). Every run is a fresh build in a fresh copy,
# so the repo's own Linux build is never touched.
#
# What is in the build: networking + HTTPS (bundled mbedTLS), menus
# (Ozone, XMB, MaterialUI, RGUI), video (OpenGL, Vulkan, D3D9/10/11/12,
# GDI), audio (XAudio2, WASAPI, DirectSound), input (DirectInput,
# XInput, raw input), zip/7z, slang shaders (bundled glslang and
# SPIRV-Cross). Everything is linked statically: no MinGW DLLs.
# Left out (no MinGW copies of these on the build machine): SDL/SDL2,
# FreeType (the built-in STB font is used), FFmpeg, Cg, Qt, Discord.
#
# Usage: tools/build_windows.sh [--dirty] [--allow-placeholder]
#   BUILD_DIR=<dir>   default: <repo>/build/win64 (git-ignored)
#   OUT_DIR=<dir>     default: <repo>/dist (git-ignored)
#   JOBS=<n>          default: nproc
#   CARTLINK_REPO_URL=<url>   repo link written into SOURCE.txt
# Needs: x86_64-w64-mingw32-gcc (pacman: mingw-w64-gcc), git, zip,
# python3.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=tools/release_common.sh
. "$ROOT/tools/release_common.sh"

BUILD_DIR=${BUILD_DIR:-$ROOT/build/win64}
OUT_DIR=${OUT_DIR:-$ROOT/dist}
JOBS=${JOBS:-$(nproc)}
HOST=x86_64-w64-mingw32
SRC=$BUILD_DIR/src

for tool in $HOST-gcc $HOST-g++ $HOST-strip $HOST-objdump git zip gzip tar python3; do
   command -v "$tool" >/dev/null 2>&1 || { echo "missing: $tool" >&2; exit 1; }
done

release_init "$ROOT" "$@"
mkdir -p "$BUILD_DIR" "$OUT_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
OUT_DIR=$(cd "$OUT_DIR" && pwd)
SRC=$BUILD_DIR/src
ZIP=$OUT_DIR/$NAME-win64.zip
TARBALL=$OUT_DIR/$NAME-source.tar.gz

CONFIGURE_OPTS="--host=$HOST \
 --disable-qt --disable-sdl --disable-sdl2 --disable-ffmpeg --disable-cg \
 --disable-freetype --disable-discord \
 --enable-builtinzlib --enable-networking --enable-ssl \
 --disable-systemmbedtls --enable-builtinmbedtls"
STATIC_LDFLAGS="-static-libgcc -static-libstdc++ -static"

echo "== Source $TARBALL (commit $SHORT)"
release_source_tarball "$ROOT" "$TARBALL"
release_unpack "$TARBALL" "$SRC"

echo "== Configuring (log: $BUILD_DIR/configure.log)"
# shellcheck disable=SC2086
if ! (cd "$SRC" && LDFLAGS="$STATIC_LDFLAGS" CROSS_COMPILE=$HOST- \
      $LOW ./configure $CONFIGURE_OPTS > "$BUILD_DIR/configure.log" 2>&1); then
   tail -20 "$BUILD_DIR/configure.log"; exit 1
fi

GITV=$SHORT
[ "$DIRTY" = 1 ] && GITV=$SHORT-dirty
echo "== Building (log: $BUILD_DIR/make.log)"
if ! (cd "$SRC" && $LOW make -j"$JOBS" GIT_VERSION="$GITV" > "$BUILD_DIR/make.log" 2>&1); then
   grep -E 'error' "$BUILD_DIR/make.log" | head -20 || true
   tail -20 "$BUILD_DIR/make.log"; exit 1
fi

if $HOST-objdump -p "$SRC/cartlink.exe" | grep -qiE 'DLL Name: lib(gcc|stdc|winpthread)'; then
   echo "cartlink.exe still needs MinGW DLLs; static linking failed" >&2
   exit 1
fi

echo "== Packing $ZIP"
STAGE="$BUILD_DIR/stage/Cartlink"
rm -rf "$BUILD_DIR/stage"
mkdir -p "$STAGE"
$HOST-strip -o "$STAGE/cartlink.exe" "$SRC/cartlink.exe"
cp "$SRC/COPYING" "$STAGE/COPYING.txt"
# Mozilla's CA list: checked after the Windows root store, which can
# lack roots on a fresh PC.
cp "$SRC/media/cacert.pem" "$STAGE/cacert.pem"
python3 "$SRC/tools/third_party_notices.py" "$SRC" "$STAGE/THIRD-PARTY-NOTICES.txt" windows
release_source_txt "$BUILD_DIR/SOURCE.txt" "64-bit Windows" \
"cartlink.exe is statically linked: it contains the MinGW-w64 runtime,
winpthreads, libgcc and libstdc++ (built with MinGW-w64 GCC
$($HOST-gcc -dumpversion)); their notices are in THIRD-PARTY-NOTICES.txt." \
COPYING.txt
# Windows line endings for Notepad.
sed 's/$/\r/' "$BUILD_DIR/SOURCE.txt" > "$STAGE/SOURCE.txt"
sed 's/$/\r/' > "$STAGE/README-Cartlink.txt" <<EOF
Cartlink $VERSION, 64-bit Windows
An unofficial fork of RetroArch 1.22.2 (not affiliated with libretro)
that adds Load from URL. Licence: GNU GPL v3 or later (COPYING.txt).
Source code: see SOURCE.txt.

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

cacert.pem is Mozilla's list of trusted certificate authorities
(MPL-2.0, see THIRD-PARTY-NOTICES.txt). Cartlink uses it with the
Windows list to check https sites: keep it next to cartlink.exe.

If Windows SmartScreen warns about an unknown app: More info > Run anyway.
Needs Windows 10 or newer.
EOF
rm -f "$ZIP"
(cd "$BUILD_DIR/stage" && zip -q -9 -r "$ZIP" Cartlink)
echo "== Done:"
echo "   $ZIP"
echo "   $TARBALL"
unzip -l "$ZIP"
unzip -l "$ZIP" | grep -q " Cartlink/cacert.pem$" \
   || { echo "cacert.pem is missing from $ZIP" >&2; exit 1; }
