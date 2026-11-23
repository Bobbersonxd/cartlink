#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Builds the 64-bit Linux cartlink from the committed sources
# (`git archive HEAD`) inside an Ubuntu 22.04 container (rootless
# podman), so it runs on any distro with glibc 2.35 or newer, and writes:
#   cartlink-<version>-linux-x86_64.tar.gz   cartlink, COPYING,
#                                            THIRD-PARTY-NOTICES.txt,
#                                            SOURCE.txt, README
#   cartlink-<version>-source.tar.gz         the exact source it was built from
# A tree with uncommitted or untracked files is refused; --dirty builds
# it anyway for a local test and names both files -DIRTY. While the
# source link is the OWNER placeholder, --allow-placeholder is needed
# (names get -PLACEHOLDER). See tools/release_common.sh.
#
# The container sees only the unpacked source tarball (no network
# during the build). Its packages are in tools/linux-build/Containerfile;
# the image is built once (that step downloads packages) and reused.
# Left out on purpose (not installed by default on many distros): Qt
# companion UI, OpenSSL, sixel, JACK, Video4Linux2, SSA subtitles.
# The PipeWire audio driver is left out too: it needs libpipewire
# 0.3.49+, Ubuntu 22.04 has 0.3.48 (PipeWire systems play sound through
# the PulseAudio driver via pipewire-pulse).
# HTTPS uses the bundled mbedTLS. The program is linked dynamically
# against the distro's libraries (listed in SOURCE.txt and README).
#
# Usage: tools/build_linux.sh [--dirty] [--allow-placeholder]
#   BUILD_DIR=<dir>   default: <repo>/build/linux (git-ignored)
#   OUT_DIR=<dir>     default: <repo>/dist (git-ignored)
#   JOBS=<n>          default: nproc
#   CARTLINK_REPO_URL=<url>   repo link written into SOURCE.txt
#   CARTLINK_LINUX_BASE=<image>  default docker.io/library/ubuntu:22.04
#   GLIBC_FLOOR=<x.y> refuse a binary needing a newer glibc (default 2.35)
# Needs: podman (rootless), git, gzip, tar, python3, binutils.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=tools/release_common.sh
. "$ROOT/tools/release_common.sh"

BUILD_DIR=${BUILD_DIR:-$ROOT/build/linux}
OUT_DIR=${OUT_DIR:-$ROOT/dist}
JOBS=${JOBS:-$(nproc)}
BASE_IMAGE=${CARTLINK_LINUX_BASE:-docker.io/library/ubuntu:22.04}
GLIBC_FLOOR=${GLIBC_FLOOR:-2.35}

for tool in podman strip objdump git gzip tar python3 sha256sum; do
   command -v "$tool" >/dev/null 2>&1 || { echo "missing: $tool" >&2; exit 1; }
done

release_init "$ROOT" "$@"
mkdir -p "$BUILD_DIR" "$OUT_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
OUT_DIR=$(cd "$OUT_DIR" && pwd)
SRC=$BUILD_DIR/src
ARCH=x86_64
PKG=$NAME-linux-$ARCH
OUT=$OUT_DIR/$PKG.tar.gz
TARBALL=$OUT_DIR/$NAME-source.tar.gz

CONFIGURE_OPTS="--disable-systemmbedtls --enable-builtinmbedtls --disable-ffmpeg \
 --disable-qt --disable-sixel --disable-jack --disable-v4l2 --disable-ssa \
 --disable-pipewire"

echo "== Source $TARBALL (commit $SHORT)"
release_source_tarball "$ROOT" "$TARBALL"
release_unpack "$TARBALL" "$SRC"
rm -f "$BUILD_DIR/runtime-packages.txt"

CF=$SRC/tools/linux-build/Containerfile
IMAGE=localhost/cartlink-build:$(printf '%s\n%s\n' "$BASE_IMAGE" "$(cat "$CF")" | sha256sum | cut -c1-12)
if ! podman image exists "$IMAGE"; then
   echo "== Building the build image $IMAGE from $BASE_IMAGE (downloads packages)"
   $LOW podman build -q --build-arg BASE="$BASE_IMAGE" -t "$IMAGE" -f "$CF" \
      "$SRC/tools/linux-build" > "$BUILD_DIR/image.log" 2>&1 \
      || { tail -20 "$BUILD_DIR/image.log"; exit 1; }
fi
BASE_DIGEST=$(podman image inspect --format '{{.Digest}}' "$BASE_IMAGE" 2>/dev/null || echo unknown)

GITV=$SHORT
[ "$DIRTY" = 1 ] && GITV=$SHORT-dirty
echo "== Configuring + building in $IMAGE (logs: $BUILD_DIR/configure.log, make.log)"
$LOW podman run --rm --network=none \
   -v "$BUILD_DIR:/work" \
   -e CONFIGURE_OPTS="$CONFIGURE_OPTS" -e JOBS="$JOBS" -e GITV="$GITV" \
   "$IMAGE" bash /work/src/tools/linux-build/build-in-container.sh

BIN=$SRC/cartlink
MAXGLIBC=$(objdump -T "$BIN" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1)
if [ "$(printf '%s\n%s\n' "$MAXGLIBC" "$GLIBC_FLOOR" | sort -V | tail -1)" != "$GLIBC_FLOOR" ]; then
   echo "cartlink needs glibc $MAXGLIBC, newer than the floor $GLIBC_FLOOR" >&2; exit 1
fi
NEEDED=$(objdump -p "$BIN" | awk '/NEEDED/ {print $2}')
for bad in libssl libcrypto libQt libsixel libjack libv4l; do
   if printf '%s\n' "$NEEDED" | grep -q "^$bad"; then
      echo "cartlink links $bad, which the release leaves out" >&2; exit 1
   fi
done
PACKAGES=$(tr '\n' ' ' < "$BUILD_DIR/runtime-packages.txt" | sed 's/ $//')
echo "== Needs glibc $MAXGLIBC; packages: $PACKAGES"

echo "== Packing $OUT"
STAGE="$BUILD_DIR/stage/$PKG"
rm -rf "$BUILD_DIR/stage"
mkdir -p "$STAGE"
strip -o "$STAGE/cartlink" "$BIN"
cp "$SRC/COPYING" "$STAGE/COPYING"
# Mozilla's CA list: used when the system has none.
cp "$SRC/media/cacert.pem" "$STAGE/cacert.pem"
python3 "$SRC/tools/third_party_notices.py" "$SRC" "$STAGE/THIRD-PARTY-NOTICES.txt" linux
release_source_txt "$STAGE/SOURCE.txt" "Linux $ARCH" \
"Built in a container from $BASE_IMAGE
  $BASE_DIGEST
with the packages in
tools/linux-build/Containerfile and these configure options:
  $(echo $CONFIGURE_OPTS)

cartlink needs glibc $MAXGLIBC or newer and is dynamically linked
against these system libraries, which are not included (they come with
your Linux distribution):
$(printf '%s\n' "$NEEDED" | sed 's/^/  /')
On Ubuntu 22.04 they are in these packages:
$(printf '%s\n' "$PACKAGES" | fold -s -w 66 | sed 's/^/  /')"
cat > "$STAGE/README" <<EOF
Cartlink $VERSION, Linux $ARCH
An unofficial fork of RetroArch 1.22.2 (not affiliated with libretro)
that adds Load from URL. Licence: GNU GPL v3 or later (COPYING).
Source code: see SOURCE.txt.

1. Unpack anywhere and run ./cartlink. It needs glibc $MAXGLIBC or newer
   (Ubuntu 22.04, Debian 12, Fedora 36 or later) and the usual desktop
   libraries (SDL2, OpenGL/EGL, X11 or Wayland, ALSA/PulseAudio/
   PipeWire, FreeType). On Debian/Ubuntu, if one is missing:
     sudo apt install $PACKAGES
   Settings go in ~/.config/cartlink.
2. First run: Main Menu > Online Updater
     - Update Assets            (menu icons and fonts)
     - Update Core Info Files
     - Core Downloader          (pick the cores you need)
3. Load Content > Load from URL, paste a link with Ctrl+V, press Enter.

Netplay: everyone loads the same link, then host / join as usual.
Only load games you own or have the right to use. Cartlink does not
provide, link to or help find games.

cacert.pem is Mozilla's list of trusted certificate authorities
(MPL-2.0, see THIRD-PARTY-NOTICES.txt). Cartlink checks https sites
against your system's list and uses this one only if it finds none.
EOF
rm -f "$OUT"
tar -C "$BUILD_DIR/stage" --owner=0 --group=0 --numeric-owner -cf - "$PKG" | gzip -n -9 > "$OUT"
echo "== Done:"
echo "   $OUT"
echo "   $TARBALL"
tar -tvzf "$OUT"
# (Listed first: grep -q would cut the pipe and fail it under pipefail.)
LISTING=$(tar -tzf "$OUT")
printf '%s\n' "$LISTING" | grep -qx "$PKG/cacert.pem" \
   || { echo "cacert.pem is missing from $OUT" >&2; exit 1; }
