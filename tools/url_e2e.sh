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
#   Run L: a plain http URL that asks for a login exits 1 with "use
#          its https:// address" (no prompt, nothing sent)
#   Run S: the same http URL with a saved login for its https site
#          (or an old host-only one) still sends nothing, exits 1
#   Run H: in its own user + network namespace (loopback only, the
#          throwaway test CA bound over the system CA bundle there, so
#          nothing outside changes): a login saved per site
#          (url_login_https://localhost:8443_*) downloads; an old
#          host-only entry (url_login_localhost_*) works for
#          https://localhost (port 443) and is moved to the new key; it
#          is not used for another port (exit 1, login needed)
#   Run C: a game-list (folder) URL on the command line exits 1 with
#          "open it from the menu", caches nothing, saves no server
#   Run B: a program behind a game name (.gb with ELF bytes) exits 1
#          with "Blocked: ...", nothing left in the cache
#   Run F: a URL with #sha256=<fingerprint> downloads under the same
#          cache name and runs; a wrong fingerprint exits 1 with
#          "Blocked: ... (fingerprint mismatch)", nothing cached
#   Run T: https with a certificate that does not verify exits 1 with
#          the certificate message (the command line never offers
#          "Download anyway"), nothing cached; the same link with its
#          fingerprint downloads
#   Run 3: netplay, host and client both loading the same URL
#   Run 3F: netplay again, both peers with the fingerprinted URL
#   Run 4: a media URL (live.mp4) still takes the old media path
#   Run M: an extensionless stream URL with a media core (-L ...ffmpeg...)
#          is not fetched
#   Run R: a later content load (menu > History) of an http path does
#          not fetch and does not exit RetroArch
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
   PIDS=""
}
trap cleanup EXIT
trap 'cleanup; echo "url_e2e: interrupted, work dir kept: $WORK"; exit 130' INT TERM

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

# ------------------------------------------------- run L: login over http

LOGIN_HTTP_MSG="This server asks for a password without encryption (http). Use its https:// address."
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
   if [ $rc -eq 1 ] && grep -qF "$LOGIN_HTTP_MSG" "$WORK/runL.log.out" \
         && ! grep -q "Login needed" "$WORK/runL.log.out"; then
      pass "run L: login over http refused (exit $rc), no prompt"
   else
      fail "run L: login over http (exit $rc, see $WORK/runL.log.out)"
   fi
else
   fail "run L: serve.py did not start"
fi

# ------------------------------------------------- run S: saved login, http

PS=$(make_profile saved)
# A login saved for the https site on that host and port (or by an
# older build for the bare host) is never sent over plain http.
cat > "$PS/url_logins.cfg" <<EOF
url_login_https://127.0.0.1:${AUTH_PORT}_user = "testuser"
url_login_https://127.0.0.1:${AUTH_PORT}_pass = "pw"
url_login_127_0_0_1_user = "testuser"
url_login_127_0_0_1_pass = "pw"
EOF
chmod 600 "$PS/url_logins.cfg"
ra "$PS" "$WORK/runS.log" -L "$CORE" \
   "http://127.0.0.1:$AUTH_PORT/basic/game.sfc" --max-frames=30
rc=$?
sfiles=$(find "$PS/assets/url_cache" -type f 2>/dev/null | wc -l)
if [ $rc -eq 1 ] && grep -qF "$LOGIN_HTTP_MSG" "$WORK/runS.log.out" \
      && [ "$sfiles" -eq 0 ]; then
   pass "run S: saved login not sent over http (exit $rc)"
else
   fail "run S: saved login over http (exit $rc, cache files $sfiles, see $WORK/runS.log.out)"
fi

# ------------------------------------------------- run H: saved login, https

# Inside "unshare -rmn" (no root needed): a private loopback where
# port 443 may be used, and the test CA bound over the system CA bundle
# for this namespace only. Two serve.py: https on 443 and on 8443.
CERTS=$ROOT/tests-other/url_content/certs
make -s -C "$ROOT/tests-other/url_content" certs/ca.pem certs/srv.pem \
   certs/srv.key >/dev/null 2>&1
SYS_CA=""
for f in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt \
      /etc/ssl/ca-bundle.pem /etc/ssl/cert.pem; do
   if [ -e "$f" ]; then SYS_CA=$f; break; fi
done
if [ -n "$SYS_CA" ] && [ -f "$CERTS/ca.pem" ] \
      && unshare -rmn true 2>/dev/null; then
   PH=$(make_profile https-saved)
   PO=$(make_profile https-old)
   PP=$(make_profile https-port)
   printf '%s\n' 'url_login_https://localhost:8443_user = "testuser"' \
      'url_login_https://localhost:8443_pass = "pw"' > "$PH/url_logins.cfg"
   printf '%s\n' 'url_login_localhost_user = "testuser"' \
      'url_login_localhost_pass = "pw"' \
      'url_login_archive_access = "AK"' \
      'url_login_archive_secret = "SK"' > "$PO/url_logins.cfg"
   cp "$PO/url_logins.cfg" "$PP/url_logins.cfg"
   chmod 600 "$PH/url_logins.cfg" "$PO/url_logins.cfg" "$PP/url_logins.cfg"
   # The inner shell gets everything through the environment.
   E2E_RA=$RA E2E_CORE=$CORE E2E_CERTS=$CERTS E2E_SYS_CA=$SYS_CA \
   E2E_SERVE=$ROOT/tests-other/url_content/serve.py E2E_WORK=$WORK \
   E2E_PH=$PH E2E_PO=$PO E2E_PP=$PP E2E_LIMIT=$LIMIT E2E_NICE=$NICE \
      unshare -rmn sh -s > "$WORK/runH.inner.log" 2>&1 <<'INNER'
ip link set lo up || exit 2
mount --bind "$E2E_CERTS/ca.pem" "$E2E_SYS_CA" || exit 3
python3 "$E2E_SERVE" 8080 443 "$E2E_CERTS/srv.pem" "$E2E_CERTS/srv.key" \
   >/dev/null 2>&1 &
s1=$!
python3 "$E2E_SERVE" 8081 8443 "$E2E_CERTS/srv.pem" "$E2E_CERTS/srv.key" \
   >/dev/null 2>&1 &
s2=$!
i=0
until python3 -c 'import socket; [socket.create_connection(("127.0.0.1", p), 1).close() for p in (443, 8443)]' 2>/dev/null; do
   i=$((i + 1)); [ $i -gt 200 ] && break
   python3 -c 'import time; time.sleep(0.05)'
done
run()
{
   prof=$1; log=$2; shift 2
   HOME=$prof/home XDG_CONFIG_HOME=$prof/home/.config \
      $E2E_LIMIT $E2E_NICE "$E2E_RA" --config="$prof/retroarch.cfg" \
      --appendconfig="$prof/append.cfg" --verbose --log-file="$log" "$@" \
      </dev/null >"$log.out" 2>&1
   echo "$log: exit $?"
}
run "$E2E_PH" "$E2E_WORK/runH.log" -L "$E2E_CORE" \
   "https://localhost:8443/basic/game.sfc" --max-frames=30
run "$E2E_PO" "$E2E_WORK/runHO.log" -L "$E2E_CORE" \
   "https://localhost/basic/game.sfc" --max-frames=30
run "$E2E_PP" "$E2E_WORK/runHP.log" -L "$E2E_CORE" \
   "https://localhost:8443/basic/game.sfc" --max-frames=30
kill $s1 $s2 2>/dev/null
wait 2>/dev/null
INNER
   # The .sfc is not an NES game, so fceumm refusing it afterwards is
   # fine: only the download counts.
   cached=$(ls "$PH/assets/url_cache/"*_game.sfc 2>/dev/null | head -n 1)
   if [ -n "$cached" ] && [ "$(wc -c < "$cached")" -eq 8192 ] \
         && grep -q "\[URL\] Downloaded" "$WORK/runH.log"; then
      pass "run H: saved login for https://localhost:8443 used"
   else
      fail "run H: saved login over https (see $WORK/runH.log, $WORK/runH.inner.log)"
   fi
   cached=$(ls "$PO/assets/url_cache/"*_game.sfc 2>/dev/null | head -n 1)
   if [ -n "$cached" ] && [ "$(wc -c < "$cached")" -eq 8192 ] \
         && grep -q '^url_login_https://localhost_user = "testuser"' "$PO/url_logins.cfg" \
         && ! grep -q '^url_login_localhost_' "$PO/url_logins.cfg" \
         && grep -q '^url_login_archive_access = "AK"' "$PO/url_logins.cfg" \
         && [ "$(stat -c %a "$PO/url_logins.cfg")" = 600 ]; then
      pass "run H: old host-only login used for https://localhost and moved to the new key"
   else
      fail "run H: old-format login (see $WORK/runHO.log, $PO/url_logins.cfg)"
   fi
   pfiles=$(find "$PP/assets/url_cache" -type f 2>/dev/null | wc -l)
   if grep -q "runHP.log: exit 1" "$WORK/runH.inner.log" \
         && grep -q "Login needed for localhost" "$WORK/runHP.log.out" \
         && [ "$pfiles" -eq 0 ]; then
      pass "run H: old host-only login not used for another port"
   else
      fail "run H: old login on another port (cache files $pfiles, see $WORK/runHP.log.out)"
   fi
else
   echo "SKIP: run H (needs unshare -rmn, a system CA bundle and the test certificates)"
fi

# ------------------------------------------------- run C: game list

PC=$(make_profile catalog)
ra "$PC" "$WORK/runC.log" -L "$CORE" "http://127.0.0.1:$AUTH_PORT/cat/" \
   --max-frames=30
rc=$?
cfiles=$(find "$PC/assets/url_cache" -type f 2>/dev/null | wc -l)
if [ $rc -eq 1 ] && grep -q \
      "This link is a game list. Open it from the menu: Load Content > Load from URL." \
      "$WORK/runC.log.out" \
      && [ "$cfiles" -eq 0 ] && [ ! -e "$PC/url_servers.cfg" ]; then
   pass "run C: game list URL refused on the command line (exit $rc)"
else
   fail "run C: game list (exit $rc, cache files $cfiles, see $WORK/runC.log.out)"
fi

# ------------------------------------------------- run B: not a game

PB=$(make_profile blocked)
ra "$PB" "$WORK/runB.log" -L "$CORE" "http://127.0.0.1:$AUTH_PORT/sneaky.gb" \
   --max-frames=30
rc=$?
bfiles=$(find "$PB/assets/url_cache" -type f 2>/dev/null | wc -l)
if [ $rc -eq 1 ] && grep -q \
      "Blocked: this link gives a program, not a game." "$WORK/runB.log.out" \
      && [ "$bfiles" -eq 0 ]; then
   pass "run B: program behind a game name refused, nothing cached (exit $rc)"
else
   fail "run B: not a game (exit $rc, cache files $bfiles, see $WORK/runB.log.out)"
fi

# ------------------------------------------------- run F: fingerprint

FP=$(sha256sum "$ROM" | cut -d' ' -f1)
PF=$(make_profile fingerprint)
ra "$PF" "$WORK/runF.log" -L "$CORE" "$URL#sha256=$FP" --max-frames=120
rc=$?
cached=$(ls "$PF/assets/url_cache/"*_game.nes 2>/dev/null | head -n 1)
plain=$(ls "$P1/assets/url_cache/"*_game.nes 2>/dev/null | head -n 1)
if [ $rc -eq 0 ] && [ -n "$cached" ] && cmp -s "$cached" "$ROM" \
      && [ "$(basename "$cached")" = "$(basename "$plain")" ] \
      && grep -q "Loading content file: \"$cached\"" "$WORK/runF.log" \
      && ! grep -q "sha256" "$WORK/server.log"; then
   pass "run F: fingerprinted URL downloaded (same cache name) and run"
else
   fail "run F: fingerprint (exit $rc, cached='$cached', see $WORK/runF.log)"
fi
PFB=$(make_profile fingerprint-bad)
ra "$PFB" "$WORK/runFB.log" -L "$CORE" \
   "$URL#sha256=0000000000000000000000000000000000000000000000000000000000000000" \
   --max-frames=30
rc=$?
ffiles=$(find "$PFB/assets/url_cache" -type f 2>/dev/null | wc -l)
if [ $rc -eq 1 ] && grep -q \
      "Blocked: this file is not the one that was shared (fingerprint mismatch)." \
      "$WORK/runFB.log.out" && [ "$ffiles" -eq 0 ]; then
   pass "run F: wrong fingerprint refused, nothing cached (exit $rc)"
else
   fail "run F: wrong fingerprint (exit $rc, cache files $ffiles, see $WORK/runFB.log.out)"
fi

# ------------------------------------------------- run T: certificate

# serve.py's https side uses the throwaway test CA, which is not one of
# the system CAs, so localhost's certificate does not verify here.
CERTS=$ROOT/tests-other/url_content/certs
make -s -C "$ROOT/tests-other/url_content" certs/ca.pem certs/srv.pem \
   certs/srv.key >/dev/null 2>&1
TLS_HTTP_PORT=$(free_port)
TLS_PORT=$(free_port)
python3 "$ROOT/tests-other/url_content/serve.py" "$TLS_HTTP_PORT" "$TLS_PORT" \
   "$CERTS/srv.pem" "$CERTS/srv.key" >/dev/null 2>&1 &
PIDS="$PIDS $!"
if [ -f "$CERTS/srv.pem" ] && wait_port "$TLS_PORT"; then
   PT=$(make_profile tls)
   ra "$PT" "$WORK/runT.log" -L "$CORE" \
      "https://localhost:$TLS_PORT/game.sfc" --max-frames=30
   rc=$?
   tfiles=$(find "$PT/assets/url_cache" -type f 2>/dev/null | wc -l)
   if [ $rc -eq 1 ] && grep -q \
         "This site's security certificate is not valid. Someone may be changing the file on the way." \
         "$WORK/runT.log.out" && [ "$tfiles" -eq 0 ]; then
      pass "run T: bad certificate refused on the command line (exit $rc)"
   else
      fail "run T: bad certificate (exit $rc, cache files $tfiles, see $WORK/runT.log.out)"
   fi
   # serve.py's game.sfc is not an NES game: only the download counts.
   SFC_FP=$(python3 -c 'import hashlib; print(hashlib.sha256(bytes((i * 7 + 3) & 0xFF for i in range(8192))).hexdigest())')
   ra "$PT" "$WORK/runTF.log" -L "$CORE" \
      "https://localhost:$TLS_PORT/game.sfc#sha256=$SFC_FP" --max-frames=30
   rc=$?
   cached=$(ls "$PT/assets/url_cache/"*_game.sfc 2>/dev/null | head -n 1)
   if [ -n "$cached" ] && [ "$(wc -c < "$cached")" -eq 8192 ] \
         && grep -q "\[URL\] Downloaded" "$WORK/runTF.log"; then
      pass "run T: fingerprinted link downloads despite the certificate"
   else
      fail "run T: fingerprinted https (exit $rc, see $WORK/runTF.log)"
   fi
else
   fail "run T: https serve.py did not start (certificates in $CERTS?)"
fi

# ------------------------------------------------- run 3: netplay

# $1 = run name, $2 = the link both peers load.
netplay_run()
{
   NP_PORT=$(free_port)
   PA=$(make_profile "host$1")
   PB=$(make_profile "client$1")
   HOME=$PA/home XDG_CONFIG_HOME=$PA/home/.config \
      $LIMIT $NICE "$RA" --config="$PA/retroarch.cfg" --appendconfig="$PA/append.cfg" \
      --verbose --log-file="$WORK/run$1a.log" -L "$CORE" "$2" \
      --host --port="$NP_PORT" --nick=hosty --max-frames=600 \
      </dev/null >"$WORK/run$1a.log.out" 2>&1 &
   APID=$!
   PIDS="$PIDS $APID"
   if wait_port "$NP_PORT"; then
      ra "$PB" "$WORK/run$1b.log" -L "$CORE" "$2" \
         --connect=127.0.0.1 --port="$NP_PORT" --nick=clienty --max-frames=600
      rcb=$?
      wait "$APID"
      rca=$?
      crca=$(grep -o "\[Content\] CRC32: 0x[0-9a-f]*" "$WORK/run$1a.log" | head -n 1)
      crcb=$(grep -o "\[Content\] CRC32: 0x[0-9a-f]*" "$WORK/run$1b.log" | head -n 1)
      if [ $rca -eq 0 ] && [ $rcb -eq 0 ] \
            && [ -n "$crca" ] && [ "$crca" = "$crcb" ] \
            && grep -q "\[Netplay\] Connected to" "$WORK/run$1b.log" \
            && ! grep -qi "CRC32s differ\|different game\|mismatch" \
               "$WORK/run$1a.log" "$WORK/run$1b.log"; then
         pass "run $1: netplay client connected, same ${crca#\[Content\] }, no mismatch"
      else
         fail "run $1: netplay (host exit $rca, client exit $rcb, see $WORK/run$1b.log)"
      fi
   else
      fail "run $1: netplay host never listened (see $WORK/run$1a.log)"
   fi
}

netplay_run 3 "$URL"
netplay_run 3F "$URL#sha256=$FP"

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

# ------------------------------------------------- run M: media core

# The core file only has to exist for -L to take it (it is not a real
# core; loading it fails afterwards, which is fine here).
mkdir -p "$WORK/mediacore"
: > "$WORK/mediacore/ffmpeg_libretro.so"
PM=$(make_profile mediacore)
ra "$PM" "$WORK/runM.log" -L "$WORK/mediacore/ffmpeg_libretro.so" \
   "http://127.0.0.1:$HTTP_PORT/stream" --max-frames=30
rc=$?
if grep -q "ffmpeg_libretro.so" "$WORK/runM.log" \
      && ! grep -q "\[URL\]" "$WORK/runM.log" \
      && [ ! -d "$PM/assets/url_cache" ] \
      && [ "$(gets "$WORK/server.log" /stream)" = 0 ]; then
   pass "run M: extensionless URL with a media core not fetched (exit $rc)"
else
   fail "run M: media core URL was fetched (see $WORK/runM.log)"
fi

# ------------------------------------------------- run R: later load

# RetroArch starts with no content (the command-line pass sees no URL),
# then the menu loads a History entry whose path is an http link. That
# load is a later pass: it must not fetch and must not exit RetroArch.
# Upstream refuses a playlist path that is not a file before it ever
# re-parses; the gate for passes that do get there (cli_active) is
# covered by test_url_content (url_content_cli_should_fetch).
PR=$(make_profile reload)
CMD_PORT=$(free_port)
mkdir -p "$PR/hist"
cat >> "$PR/append.cfg" <<EOF
history_list_enable = "true"
content_history_directory = "$PR/hist"
network_cmd_enable = "true"
network_cmd_port = "$CMD_PORT"
EOF
cat > "$PR/hist/content_history.lpl" <<EOF
{"version":"1.5","default_core_path":"","default_core_name":"","label_display_mode":0,"right_thumbnail_mode":0,"left_thumbnail_mode":0,"sort_mode":0,"items":[{"path":"http://127.0.0.1:$HTTP_PORT/history.nes","label":"history","core_path":"$CORE","core_name":"FCEUmm","crc32":"DETECT","db_name":""}]}
EOF
HOME=$PR/home XDG_CONFIG_HOME=$PR/home/.config \
   $LIMIT $NICE "$RA" --config="$PR/retroarch.cfg" \
   --appendconfig="$PR/append.cfg" --verbose --log-file="$WORK/runR.log" \
   </dev/null >"$WORK/runR.log.out" 2>&1 &
RPID=$!
PIDS="$PIDS $RPID"
python3 -c 'import time; time.sleep(3)'
# Main menu: Load Core, Load Content, History -> open it, open the
# entry, Run. Each command is sent 3 times 5 ms apart (one UDP packet
# alone is sometimes missed between polls).
for c in MENU_DOWN MENU_DOWN MENU_B MENU_B MENU_B; do
   python3 -c 'import socket,sys,time
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
for i in range(3):
   s.sendto(sys.argv[2].encode(),("127.0.0.1",int(sys.argv[1]))); time.sleep(0.005)' \
      "$CMD_PORT" "$c"
   python3 -c 'import time; time.sleep(0.7)'
done
python3 -c 'import time; time.sleep(3)'
if kill -0 "$RPID" 2>/dev/null; then alive=1; else alive=0; fi
kill "$RPID" 2>/dev/null
wait "$RPID" 2>/dev/null
if [ $alive = 1 ] \
      && grep -q "Failed to load from playlist" "$WORK/runR.log" \
      && ! grep -q "\[URL\]" "$WORK/runR.log" \
      && [ "$(gets "$WORK/server.log" /history.nes)" = 0 ]; then
   pass "run R: History load of an http path: no fetch, RetroArch still running"
else
   fail "run R: later load (alive=$alive, see $WORK/runR.log)"
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

cleanup
if [ $FAILS -eq 0 ]; then
   echo "url_e2e: all runs passed"
   case "$WORK" in
      */url_e2e.*) rm -rf "$WORK" ;;
   esac
   exit 0
fi
echo "url_e2e: $FAILS run(s) failed, work dir kept: $WORK"
exit 1
