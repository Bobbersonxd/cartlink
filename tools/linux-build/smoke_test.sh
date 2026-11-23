#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Checks a Linux release on a fresh distro image (rootless podman,
# headless): installs only the runtime packages tools/build_linux.sh
# found (plus a compiler for the tests), runs `cartlink --version`, then
# builds and runs the URL unit tests and the local-server fetch tests
# from the source tarball. The container downloads packages, so it has
# network; the tests themselves only use 127.0.0.1.
#
# Usage: tools/linux-build/smoke_test.sh <image> <linux .tar.gz> <source .tar.gz>
#          [<runtime-packages.txt>, default build/linux/runtime-packages.txt]
#   e.g. tools/linux-build/smoke_test.sh docker.io/library/debian:12 \
#          dist/cartlink-0.1.0-linux-x86_64.tar.gz dist/cartlink-0.1.0-source.tar.gz

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=$1
BIN_TGZ=$(realpath "$2")
SRC_TGZ=$(realpath "$3")
PKGS=$(realpath "${4:-$ROOT/build/linux/runtime-packages.txt}")

LOW="nice -n 19"
command -v ionice >/dev/null 2>&1 && LOW="$LOW ionice -c3"

$LOW podman run --rm \
   -v "$BIN_TGZ:/in/bin.tar.gz:ro" -v "$SRC_TGZ:/in/src.tar.gz:ro" \
   -v "$PKGS:/in/packages.txt:ro" \
   "$IMAGE" bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
grep -h PRETTY_NAME /etc/os-release
apt-get update -qq
# shellcheck disable=SC2046
apt-get install -y -qq --no-install-recommends $(cat /in/packages.txt) >/dev/null
mkdir -p /t/bin /t/src
tar -xzf /in/bin.tar.gz -C /t/bin --strip-components=1
echo "== ldd: missing libraries"
if ldd /t/bin/cartlink | grep "not found"; then exit 1; fi
echo "   none"
echo "== cartlink --version"
/t/bin/cartlink --version
# ca-certificates: the system CA bundle every desktop has (login tests).
apt-get install -y -qq --no-install-recommends gcc libc6-dev make python3 ca-certificates \
   openssl zlib1g-dev >/dev/null
tar -xzf /in/src.tar.gz -C /t/src --strip-components=1
echo "== URL unit tests + fetch tests"
nice -n 19 make -s -C /t/src/tests-other/url_content test fetch-test 2>&1 | tail -15
'
