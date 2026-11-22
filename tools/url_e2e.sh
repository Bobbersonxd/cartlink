#!/bin/sh
# End-to-end check for "Load from URL" (command line side).
#
# Everything is headless and silent (null video/audio/input drivers),
# runs at low priority, and uses a throwaway HOME, config and folders,
# so the user's own RetroArch setup is never read or written.
#
#   Run 0: a local file through the normal path (regression check)
#   Run 1: a URL on the command line is downloaded, cached and runs
#   Run 2: the same URL again is a cache hit (no new request)
#   Run L: a URL that needs a login exits non-zero with a hint
#   Run 3: netplay, host and client both loading the same URL
#   Run 4: a media URL (live.mp4) still takes the old media path
#   Run 5: ./configure --disable-networking still builds
#
# Usage: tools/url_e2e.sh
#   RA=<retroarch binary>          default: ./retroarch in the repo
#   URL_E2E_BASE=<dir>             default: ~/retro-link (test-cores/,
#                                  test-content/ live there)
#   URL_E2E_SKIP_NONET=1           skip run 5 (it builds a full copy)
# Needs: python3, git, make, a C compiler.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RA=${RA:-$ROOT/retroarch}
BASE=${URL_E2E_BASE:-$HOME/retro-link}
CORES=$BASE/test-cores
CONTENT=$BASE/test-content
CORE=$CORES/fceumm_libretro.so
ROM=$CONTENT/game.nes
NICE="nice -n 19 ionice -c3"
LIMIT="timeout 120"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/url_e2e.XXXXXX") || exit 1
PIDS=""
FAILS=0

cleanup()
{
   for p in $PIDS; do
      kill "$p" 2>/dev/null
      wait "$p" 2>/dev/null
   done
}
trap cleanup EXIT INT TERM

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

free_port()
{
   python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

wait_port()
{
   i=0
   until python3 -c 'import socket,sys; socket.create_connection(("127.0.0.1", int(sys.argv[1])), 1).close()' "$1" 2>/dev/null; do
      i=$((i + 1))
      if [ $i -gt 200 ]; then
         return 1
      fi
      python3 -c 'import time; time.sleep(0.05)'
   done
   return 0
}

# Number of GET requests for a path in a http.server log.
gets()
{
   grep -c "\"GET $2 " "$1" 2>/dev/null
}

# A throwaway profile: HOME, config and every folder RetroArch writes.
# $1 = profile name. Prints the profile directory.
make_profile()
{
   d=$WORK/$1
   mkdir -p "$d/home" "$d/assets" "$d/saves" "$d/states" "$d/system" \
      "$d/cache" "$d/playlists" "$d/logs" "$d/screens"
   cat > "$d/retroarch.cfg" <<EOF
video_driver = "null"
audio_driver = "null"
input_driver = "null"
joypad_driver = "null"
microphone_driver = "null"
midi_driver = "null"
camera_driver = "null"
location_driver = "null"
record_driver = "null"
menu_driver = "rgui"
audio_enable = "false"
vrr_runloop_enable = "true"
config_save_on_exit = "false"
history_list_enable = "false"
network_cmd_enable = "false"
netplay_nat_traversal = "false"
netplay_public_announce = "false"
netplay_use_mitm_server = "false"
core_updater_auto_extract_archive = "false"
pause_nonactive = "false"
core_assets_directory = "$d/assets"
savefile_directory = "$d/saves"
savestate_directory = "$d/states"
system_directory = "$d/system"
cache_directory = "$d/cache"
playlist_directory = "$d/playlists"
content_history_directory = "$d/playlists"
log_dir = "$d/logs"
screenshot_directory = "$d/screens"
libretro_directory = "$CORES"
EOF
   : > "$d/append.cfg"
   echo "$d"
}

# Runs RetroArch headless with profile $1, log $2, then the arguments.
ra()
{
   prof=$1; log=$2; shift 2
   HOME=$prof/home XDG_CONFIG_HOME=$prof/home/.config \
      $LIMIT $NICE "$RA" --config="$prof/retroarch.cfg" \
      --appendconfig="$prof/append.cfg" --verbose --log-file="$log" "$@" \
      </dev/null >"$log.out" 2>&1
}

# ---------------------------------------------------------------- setup

if [ ! -x "$RA" ]; then
   echo "No RetroArch binary at $RA (build it first)."
   exit 1
fi

if [ ! -f "$CORE" ]; then
   echo "Building the fceumm test core into $CORES ..."
   mkdir -p "$CORES"
   if [ ! -d "$CORES/src-fceumm" ]; then
      git clone --depth 1 https://github.com/libretro/libretro-fceumm.git \
         "$CORES/src-fceumm" || exit 1
   fi
   $NICE make -C "$CORES/src-fceumm" -j16 >"$CORES/build-fceumm.log" 2>&1 \
      || { echo "core build failed, see $CORES/build-fceumm.log"; exit 1; }
   cp "$CORES/src-fceumm/fceumm_libretro.so" "$CORE" || exit 1
fi

# A tiny valid iNES ROM, made here (no download): NROM, 16 KiB PRG that
# counts in RAM forever, 8 KiB CHR of a simple pattern.
if [ ! -f "$ROM" ]; then
   mkdir -p "$CONTENT"
   python3 - "$ROM" <<'EOF'
import sys
prg = bytearray([0xEA] * 0x4000)                 # NOP fill
code = [0x78,                                    # C000 SEI
        0xD8,                                    # C001 CLD
        0xA2, 0xFF,                              # C002 LDX #$FF
        0x9A,                                    # C004 TXS
        0xE6, 0x00,                              # C005 INC $00
        0xA5, 0x00,                              # C007 LDA $00
        0x85, 0x01,                              # C009 STA $01
        0x4C, 0x05, 0xC0]                        # C00B JMP $C005
prg[0:len(code)] = bytes(code)
prg[0x0010] = 0x40                               # C010 RTI (NMI/IRQ)
prg[0x3FFA:0x4000] = bytes([0x10, 0xC0,          # NMI   -> C010
                            0x00, 0xC0,          # RESET -> C000
                            0x10, 0xC0])         # IRQ   -> C010
chr_ = bytes((i * 37 + 11) & 0xFF for i in range(0x2000))
hdr = b"NES\x1a" + bytes([1, 1, 0, 0]) + bytes(8)
with open(sys.argv[1], "wb") as f:
    f.write(hdr + bytes(prg) + chr_)
EOF
fi

SERVE=$WORK/serve
mkdir -p "$SERVE"
cp "$ROM" "$SERVE/game.nes"
printf 'not a real movie\n' > "$SERVE/live.mp4"
HTTP_PORT=$(free_port)
python3 -u -m http.server --bind 127.0.0.1 --directory "$SERVE" "$HTTP_PORT" \
   >"$WORK/server.log" 2>&1 &
PIDS="$PIDS $!"
wait_port "$HTTP_PORT" || { echo "http.server did not start"; exit 1; }
URL="http://127.0.0.1:$HTTP_PORT/game.nes"

echo "work dir: $WORK"

# ------------------------------------------------- run 0: local file (R1)

P0=$(make_profile local)
ra "$P0" "$WORK/run0.log" -L "$CORE" "$ROM" --max-frames=120
rc=$?
if [ $rc -eq 0 ] \
      && grep -q "Loading content file: \"$ROM\"" "$WORK/run0.log" \
      && ! grep -q "\[URL\]" "$WORK/run0.log" \
      && [ "$(gets "$WORK/server.log" /game.nes)" = 0 ]; then
   pass "run 0: local file loads through the normal path"
else
   fail "run 0: local file (exit $rc, see $WORK/run0.log)"
fi

# ------------------------------------------------- run 1: download + run

P1=$(make_profile a)
ra "$P1" "$WORK/run1.log" -L "$CORE" "$URL" --max-frames=120
rc=$?
cached=$(ls "$P1/assets/url_cache/"*_game.nes 2>/dev/null | head -n 1)
if [ $rc -eq 0 ] && [ -n "$cached" ] && cmp -s "$cached" "$ROM" \
      && grep -q "\[URL\] Downloaded" "$WORK/run1.log" \
      && grep -q "Loading content file: \"$cached\"" "$WORK/run1.log" \
      && [ "$(gets "$WORK/server.log" /game.nes)" = 1 ]; then
   pass "run 1: URL downloaded to $(basename "$cached") and run"
else
   fail "run 1: download (exit $rc, cached='$cached', see $WORK/run1.log)"
fi

# ------------------------------------------------- run 2: cache hit

ra "$P1" "$WORK/run2.log" -L "$CORE" "$URL" --max-frames=120
rc=$?
if [ $rc -eq 0 ] && grep -q "\[URL\] Cache hit" "$WORK/run2.log" \
      && [ "$(gets "$WORK/server.log" /game.nes)" = 1 ]; then
   pass "run 2: cache hit, no new request"
else
   fail "run 2: cache (exit $rc, GETs $(gets "$WORK/server.log" /game.nes), see $WORK/run2.log)"
fi

# ------------------------------------------------- run L: login needed

AUTH_PORT=$(free_port)
python3 "$ROOT/tests-other/url_content/serve.py" "$AUTH_PORT" \
   >/dev/null 2>&1 &
PIDS="$PIDS $!"
if wait_port "$AUTH_PORT"; then
   PL=$(make_profile login)
   ra "$PL" "$WORK/runL.log" -L "$CORE" \
      "http://127.0.0.1:$AUTH_PORT/basic/game.sfc" --max-frames=60
   rc=$?
   # Exit 1 exactly: a crash (e.g. 139) is a failure too.
   if [ $rc -eq 1 ] && grep -q \
         "Login needed for 127.0.0.1: use the menu once to save it" \
         "$WORK/runL.log.out"; then
      pass "run L: login needed exits $rc with the hint"
   else
      fail "run L: login needed (exit $rc, see $WORK/runL.log.out)"
   fi
else
   fail "run L: serve.py did not start"
fi

# ------------------------------------------------- run 3: netplay

NP_PORT=$(free_port)
PA=$(make_profile host)
PB=$(make_profile client)
HOME=$PA/home XDG_CONFIG_HOME=$PA/home/.config \
   $LIMIT $NICE "$RA" --config="$PA/retroarch.cfg" --appendconfig="$PA/append.cfg" \
   --verbose --log-file="$WORK/run3a.log" -L "$CORE" "$URL" \
   --host --port="$NP_PORT" --nick=hosty --max-frames=600 \
   </dev/null >"$WORK/run3a.log.out" 2>&1 &
APID=$!
PIDS="$PIDS $APID"
if wait_port "$NP_PORT"; then
   ra "$PB" "$WORK/run3b.log" -L "$CORE" "$URL" \
      --connect=127.0.0.1 --port="$NP_PORT" --nick=clienty --max-frames=600
   rcb=$?
   wait "$APID"
   rca=$?
   crca=$(grep -o "\[Content\] CRC32: 0x[0-9a-f]*" "$WORK/run3a.log" | head -n 1)
   crcb=$(grep -o "\[Content\] CRC32: 0x[0-9a-f]*" "$WORK/run3b.log" | head -n 1)
   if [ $rca -eq 0 ] && [ $rcb -eq 0 ] \
         && [ -n "$crca" ] && [ "$crca" = "$crcb" ] \
         && grep -q "\[Netplay\] Connected to" "$WORK/run3b.log" \
         && ! grep -qi "CRC32s differ\|different game\|mismatch" \
            "$WORK/run3a.log" "$WORK/run3b.log"; then
      pass "run 3: netplay client connected, same ${crca#\[Content\] }, no mismatch"
   else
      fail "run 3: netplay (host exit $rca, client exit $rcb, see $WORK/run3b.log)"
   fi
else
   fail "run 3: netplay host never listened (see $WORK/run3a.log)"
fi

# ------------------------------------------------- run 4: media URL

before=$(gets "$WORK/server.log" /live.mp4)
P4=$(make_profile media)
ra "$P4" "$WORK/run4.log" "http://127.0.0.1:$HTTP_PORT/live.mp4" --max-frames=30
rc=$?
if ! grep -q "\[URL\]" "$WORK/run4.log" \
      && [ ! -d "$P4/assets/url_cache" ] \
      && [ "$(gets "$WORK/server.log" /live.mp4)" = "$before" ]; then
   pass "run 4: media URL not fetched by url_fetch (exit $rc; this build has no ffmpeg)"
else
   fail "run 4: media URL went through url_fetch (see $WORK/run4.log)"
fi

# ------------------------------------------------- run 5: no networking

if [ "${URL_E2E_SKIP_NONET:-0}" = 1 ]; then
   echo "SKIP: run 5 (URL_E2E_SKIP_NONET=1)"
else
   NN=$WORK/nonet
   mkdir -p "$NN"
   (cd "$ROOT" && git ls-files -co --exclude-standard -z \
      | xargs -0 cp --parents -t "$NN" 2>/dev/null)
   if (cd "$NN" && $NICE ./configure --disable-networking --disable-ffmpeg \
            --disable-qt >"$WORK/run5-configure.log" 2>&1 \
         && $NICE make -j16 >"$WORK/run5-make.log" 2>&1) \
         && [ -x "$NN/retroarch" ]; then
      pass "run 5: --disable-networking build compiles"
   else
      fail "run 5: --disable-networking build (see $WORK/run5-make.log)"
   fi
   rm -rf "$NN"
fi

if [ $FAILS -eq 0 ]; then
   echo "url_e2e: all runs passed"
   exit 0
fi
echo "url_e2e: $FAILS run(s) failed"
exit 1
