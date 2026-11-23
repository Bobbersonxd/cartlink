#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Runs inside the build container (started by tools/build_linux.sh):
# /work is the build folder, /work/src the unpacked source tarball.
# Env: CONFIGURE_OPTS, JOBS, GITV.
set -euo pipefail
LOW="nice -n 19"
ionice -c3 true 2>/dev/null && LOW="$LOW ionice -c3"
cd /work/src
# shellcheck disable=SC2086
if ! $LOW ./configure $CONFIGURE_OPTS > /work/configure.log 2>&1; then
   tail -20 /work/configure.log; exit 1
fi
if ! $LOW make -j"$JOBS" GIT_VERSION="$GITV" > /work/make.log 2>&1; then
   grep -E 'error' /work/make.log | head -20 || true
   tail -20 /work/make.log; exit 1
fi
# The distro packages that hold the libraries cartlink links directly
# (their own dependencies come with them).
needed=$(objdump -p cartlink | awk '/NEEDED/ {print $2}')
ldd cartlink | awk '/=> \// {print $1, $3}' | while read -r name lib; do
   printf '%s\n' "$needed" | grep -qxF "$name" || continue
   for p in "$lib" "$(readlink -f "$lib")" "/usr${lib#/usr}" "${lib#/usr}"; do
      if dpkg -S "$p" 2>/dev/null; then break; fi
   done
done | cut -d: -f1 | sort -u > /work/runtime-packages.txt
[ -s /work/runtime-packages.txt ] || { echo "no runtime packages found" >&2; exit 1; }
