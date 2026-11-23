#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Checks griffin/griffin_mbedtls_*.c (the bundled mbedTLS for unity
# builds such as Xcode) against the separate-file build of
# deps/mbedtls/sources.mk, on Linux with gcc and clang:
# - every source in sources.mk is in exactly one part
# - each part compiles on its own (from griffin/, -Wall: no warnings)
# - together the parts define exactly the global symbols the separate
#   files define, none twice
# - every source preprocesses to the same tokens in both builds
#   (tools/griffin_mbedtls_pp.py), in gnu99 and strict c99, so a macro
#   from one source never changes another's code
# Run it after changing sources.mk, the parts or mbedTLS.
# Usage: tools/griffin_mbedtls_check.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/griffin_mbedtls.XXXXXX")
trap 'rm -r -- "$W"' EXIT
mkdir -p "$W/sep" "$W/chk"
FL=(-std=gnu99 -O0 -DHAVE_SSL -DHAVE_NETWORKING -DHAVE_BUILTINMBEDTLS
    -I"$ROOT/deps/mbedtls" -I"$ROOT/libretro-common/include")
SRCS=$(printf 'include %s/deps/mbedtls/sources.mk\nprint:\n\t@echo $(MBEDTLS_SRCS)\n' "$ROOT" \
   | make -s -f - print)
PARTS=("$ROOT"/griffin/griffin_mbedtls_*.c)

n=0
for s in $SRCS; do
   c=$(grep -l "^#include \"../deps/mbedtls/$s\"$" "${PARTS[@]}" | wc -l)
   [ "$c" -eq 1 ] || { echo "FAIL: $s is in $c parts"; exit 1; }
   n=$((n+1))
done
inc=$(cat "${PARTS[@]}" | grep -c '^#include "../deps/mbedtls/')
[ "$inc" -eq "$n" ] || { echo "FAIL: $inc includes for $n sources"; exit 1; }
echo "ok: $n sources, each in exactly one of ${#PARTS[@]} parts"

for cc in gcc clang; do
   command -v $cc >/dev/null || { echo "skip: no $cc"; continue; }
   for s in $SRCS; do
      $cc "${FL[@]}" -c -o "$W/sep/$cc-${s%.c}.o" "$ROOT/deps/mbedtls/$s"
   done
   CHK=()
   for f in "${PARTS[@]}"; do
      b=$(basename "$f" .c)
      (cd "$ROOT/griffin" && $cc -std=gnu99 -Wall -c -DHAVE_SSL -DHAVE_NETWORKING \
         -DHAVE_BUILTINMBEDTLS -I../deps/mbedtls -o "$W/chk/$cc-$b.o" "$b.c" \
         2> "$W/chk/$cc-$b.log") \
         || { echo "FAIL: $cc $b"; cat "$W/chk/$cc-$b.log"; exit 1; }
      if [ -s "$W/chk/$cc-$b.log" ]; then
         echo "FAIL: $cc $b has warnings"; cat "$W/chk/$cc-$b.log"; exit 1
      fi
      CHK+=("$W/chk/$cc-$b.o")
   done
   echo "ok: $cc compiles every part on its own, no warnings"
   nm --defined-only -g "$W"/sep/$cc-*.o | awk 'NF==3{print $3}' | sort > "$W/$cc-sep.glob"
   nm --defined-only -g "${CHK[@]}" | awk 'NF==3{print $3}' | sort > "$W/$cc-chk.glob"
   if [ -n "$(uniq -d "$W/$cc-chk.glob")" ]; then
      echo "FAIL: $cc symbols defined twice:"; uniq -d "$W/$cc-chk.glob"; exit 1
   fi
   diff -u "$W/$cc-sep.glob" "$W/$cc-chk.glob" \
      || { echo "FAIL: $cc global symbols differ"; exit 1; }
   echo "ok: $cc $(wc -l < "$W/$cc-sep.glob") global symbols, the same set, none twice"
   python3 -I "$ROOT/tools/griffin_mbedtls_pp.py" "$ROOT" $cc -std=gnu99
   python3 -I "$ROOT/tools/griffin_mbedtls_pp.py" "$ROOT" $cc -std=c99
done
