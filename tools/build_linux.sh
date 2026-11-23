#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Builds the 64-bit Linux cartlink from the committed sources
# (`git archive HEAD`) and writes:
#   cartlink-<version>-linux-x86_64.tar.gz   cartlink, COPYING,
#                                            THIRD-PARTY-NOTICES.txt,
#                                            SOURCE.txt, README
#   cartlink-<version>-source.tar.gz         the exact source it was built from
# A tree with uncommitted or untracked files is refused; --dirty builds
# it anyway for a local test and names both files -DIRTY (see
# tools/release_common.sh). Every run is a fresh build in a fresh copy,
# so the repo's own build is never touched.
#
# Configure options as in README.md (bundled mbedTLS, no FFmpeg); the
# rest is what configure finds on this machine. The program is linked
# dynamically against this machine's system libraries (listed in
# SOURCE.txt), so it runs on distros about as new as this one.
#
# Usage: tools/build_linux.sh [--dirty]
#   BUILD_DIR=<dir>   default: <repo>/build/linux (git-ignored)
#   OUT_DIR=<dir>     default: <repo>/dist (git-ignored)
#   JOBS=<n>          default: nproc
#   CARTLINK_REPO_URL=<url>   repo link written into SOURCE.txt
# Needs: the build packages from README.md, git, gzip, tar, python3.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=tools/release_common.sh
. "$ROOT/tools/release_common.sh"

BUILD_DIR=${BUILD_DIR:-$ROOT/build/linux}
OUT_DIR=${OUT_DIR:-$ROOT/dist}
JOBS=${JOBS:-$(nproc)}

for tool in gcc g++ make strip objdump git gzip tar python3; do
   command -v "$tool" >/dev/null 2>&1 || { echo "missing: $tool" >&2; exit 1; }
done

release_init "$ROOT" "$@"
mkdir -p "$BUILD_DIR" "$OUT_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
OUT_DIR=$(cd "$OUT_DIR" && pwd)
SRC=$BUILD_DIR/src
ARCH=$(uname -m)
PKG=$NAME-linux-$ARCH
OUT=$OUT_DIR/$PKG.tar.gz
TARBALL=$OUT_DIR/$NAME-source.tar.gz

CONFIGURE_OPTS="--disable-systemmbedtls --enable-builtinmbedtls --disable-ffmpeg"

echo "== Source $TARBALL (commit $SHORT)"
release_source_tarball "$ROOT" "$TARBALL"
release_unpack "$TARBALL" "$SRC"

echo "== Configuring (log: $BUILD_DIR/configure.log)"
# shellcheck disable=SC2086
if ! (cd "$SRC" && $LOW ./configure $CONFIGURE_OPTS > "$BUILD_DIR/configure.log" 2>&1); then
   tail -20 "$BUILD_DIR/configure.log"; exit 1
fi

GITV=$SHORT
[ "$DIRTY" = 1 ] && GITV=$SHORT-dirty
echo "== Building (log: $BUILD_DIR/make.log)"
if ! (cd "$SRC" && $LOW make -j"$JOBS" GIT_VERSION="$GITV" > "$BUILD_DIR/make.log" 2>&1); then
   grep -E 'error' "$BUILD_DIR/make.log" | head -20 || true
   tail -20 "$BUILD_DIR/make.log"; exit 1
fi

echo "== Packing $OUT"
STAGE="$BUILD_DIR/stage/$PKG"
rm -rf "$BUILD_DIR/stage"
mkdir -p "$STAGE"
strip -o "$STAGE/cartlink" "$SRC/cartlink"
cp "$SRC/COPYING" "$STAGE/COPYING"
python3 "$SRC/tools/third_party_notices.py" "$SRC" "$STAGE/THIRD-PARTY-NOTICES.txt" linux
NEEDED=$(objdump -p "$SRC/cartlink" | awk '/NEEDED/ {print "  " $2}')
release_source_txt "$STAGE/SOURCE.txt" "Linux $ARCH" \
"cartlink is dynamically linked against these system libraries,
which are not included: they come with your Linux distribution
(built on $( (. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}") || echo unknown)).
$NEEDED"
cat > "$STAGE/README" <<EOF
Cartlink $VERSION, Linux $ARCH
An unofficial fork of RetroArch 1.22.2 (not affiliated with libretro)
that adds Load from URL. Licence: GNU GPL v3 or later (COPYING).
Source code: see SOURCE.txt.

1. Unpack anywhere and run ./cartlink. It needs the system libraries
   listed in SOURCE.txt (a desktop distro about as new as the one it
   was built on has them). Settings go in ~/.config/cartlink.
2. First run: Main Menu > Online Updater
     - Update Assets            (menu icons and fonts)
     - Update Core Info Files
     - Core Downloader          (pick the cores you need)
3. Load Content > Load from URL, paste a link with Ctrl+V, press Enter.

Netplay: everyone loads the same link, then host / join as usual.
Only load games you own or have the right to use. Cartlink does not
provide, link to or help find games.
EOF
rm -f "$OUT"
tar -C "$BUILD_DIR/stage" --owner=0 --group=0 --numeric-owner -cf - "$PKG" | gzip -n -9 > "$OUT"
echo "== Done:"
echo "   $OUT"
echo "   $TARBALL"
tar -tvzf "$OUT"
