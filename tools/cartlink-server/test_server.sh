#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
#
# Test the Cartlink home server kit as the normal user, no sudo.
# Runs a throwaway Caddy on 127.0.0.1:18443 (never 80/443) in a temp folder,
# with a self-signed certificate ("tls internal"), and checks logins,
# generated passwords, the JSON listing, downloads, hidden files,
# cartlink-link (through Cartlink's own fingerprint parser), the fail2ban
# filter and the system detection. Everything is removed on exit.
#   tools/cartlink-server/test_server.sh
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PORT=18443
URL="https://127.0.0.1:$PORT"
PASS_MINE='correct-horse-42-battery'
PASS_BAD='wrong-horse-42-battery'

command -v caddy >/dev/null || { echo "caddy is not installed"; exit 1; }
command -v jq >/dev/null || { echo "jq is not installed"; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/cartlink-test.XXXXXX")
CADDY_PID=
cleanup() {
	if [[ -n $CADDY_PID ]]; then
		kill "$CADDY_PID" 2>/dev/null || true
		wait "$CADDY_PID" 2>/dev/null || true
	fi
	rm -rf "$TMP"
}
trap cleanup EXIT

fails=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
check() { # check "label" command...
	local label=$1; shift
	if "$@"; then pass "$label"; else fail "$label"; fi
}

quiet() { "$@" >/dev/null 2>&1; }
jqok() { jq -e "$1" <<<"$2" >/dev/null 2>&1; } # jqok FILTER JSON

# --- every script parses ---
for s in setup.sh cartlink-users cartlink-link duckdns-update.sh test_server.sh; do
	check "bash -n $s" bash -n "$HERE/$s"
done

port_busy() { [[ -n $(ss -Hltn "sport = :$1") ]]; } # no early-exit pipe under pipefail

if port_busy "$PORT"; then echo "port $PORT is already in use, stopping"; exit 1; fi
ADMIN=
for p in $(seq 18444 18499); do
	if ! port_busy "$p"; then ADMIN=$p; break; fi
done
[[ -n $ADMIN ]] || { echo "no free admin port in 18444-18499"; exit 1; }

# Cartlink's own fingerprint parser, built from network/url_content.c.
make -s -C "$REPO/tests-other/url_content" check-link >/dev/null ||
	{ echo "could not build tests-other/url_content/check_link"; exit 1; }
CHECK_LINK=$REPO/tests-other/url_content/check_link

# Keep Caddy's own files (certificates, autosave) inside the temp folder.
export XDG_DATA_HOME="$TMP/xdg-data" XDG_CONFIG_HOME="$TMP/xdg-config"
export CARTLINK_ROOT="$TMP/cartlink" CARTLINK_NO_RELOAD=1

# Fixtures: dummy "games" (a few bytes, not real ROMs) and a hidden file.
GAME="$CARTLINK_ROOT/games/Game Boy/Test Game.gb"
ODD="$CARTLINK_ROOT/games/Game Boy/Odd #1 & 100%+more*é.gb"
mkdir -p "$CARTLINK_ROOT/games/Game Boy" "$CARTLINK_ROOT/games/.secret"
printf 'CARTLINK-TEST-GAME\x00\x01\x02\xff' > "$GAME"
printf 'CARTLINK-ODD-NAME\x00\x7f' > "$ODD"
printf 'secret' > "$CARTLINK_ROOT/games/.hidden"
printf 'secret' > "$CARTLINK_ROOT/games/.secret/inside.gb"
: > "$CARTLINK_ROOT/users"
chmod 0640 "$CARTLINK_ROOT/users"

# --- the site file comes from the real template, via setup.sh --print-site ---
PROD=$(CARTLINK_ROOT=/srv/cartlink "$HERE/setup.sh" --print-site example.duckdns.org) || PROD=
check "production site file has the domain" grep -q '^example.duckdns.org {' <<<"$PROD"
check "production site file has no tls internal" bash -c '! grep -q "tls internal" <<<"$1"' _ "$PROD"
check "production site file imports /srv/cartlink/users" grep -q 'import /srv/cartlink/users' <<<"$PROD"
check "production site file logs to /var/log/caddy/cartlink.log" grep -q 'output file /var/log/caddy/cartlink.log' <<<"$PROD"
check "production site file has no leftover placeholders" bash -c '[[ -n $1 ]] && ! grep -q "{{" <<<"$1"' _ "$PROD"
check "setup.sh rejects a non-duckdns name" bash -c '! "$1" --print-site "evil.example.com; rm" >/dev/null 2>&1' _ "$HERE/setup.sh"

SITE=$(CARTLINK_TLS='tls internal' CARTLINK_LOG="$TMP/access.log" CARTLINK_TEST_DOMAIN=1 \
	"$HERE/setup.sh" --print-site "$URL") || SITE=
{
	printf '{\n\tadmin localhost:%s\n\tdefault_bind 127.0.0.1\n\tauto_https disable_redirects\n\tskip_install_trust\n' "$ADMIN"
	printf '\tstorage file_system %s\n}\n\n' "$TMP/caddy-data"
	printf '%s\n' "$SITE"
} > "$TMP/Caddyfile"
check "test config validates" quiet caddy validate --config "$TMP/Caddyfile" --adapter caddyfile

caddy run --config "$TMP/Caddyfile" --adapter caddyfile >"$TMP/caddy.out" 2>&1 &
CADDY_PID=$!
for _ in $(seq 1 50); do
	curl -sk -o /dev/null "$URL/" && break
	sleep 0.2
done

code() { curl -sk -o /dev/null -w '%{http_code}' "$@"; }
reload() { caddy reload --config "$TMP/Caddyfile" --adapter caddyfile --address "localhost:$ADMIN" >/dev/null 2>&1; }
users() { "$HERE/cartlink-users" "$@"; }
choose() { CARTLINK_PASS_STDIN=1 "$HERE/cartlink-users" add --choose "$@"; } # password on stdin

check "no login -> 401" [ "$(code "$URL/")" = 401 ]

# --- cartlink-users input checks ---
check "bad name is refused" bash -c '! "$1" add "sam!" >/dev/null 2>&1' _ "$HERE/cartlink-users"
check "--choose: an 11-character password is refused" bash -c '! CARTLINK_PASS_STDIN=1 "$1" add --choose sam <<<"elevenchars" >/dev/null 2>&1' _ "$HERE/cartlink-users"

# --- add with a generated password ---
OUT=$(users add sam 2>&1) || OUT=
PASS_GEN=$(sed -n 's/^Password for sam: //p' <<<"$OUT")
check "add sam prints a generated password once" [ "$(grep -c '^Password for sam: ' <<<"$OUT")" = 1 ]
check "generated password is 5 or more words ($PASS_GEN)" \
	bash -c '[[ $1 =~ ^[a-z]+(-[a-z]+){4,}$ ]]' _ "$PASS_GEN"
check "every word of it is in the shipped word list" bash -c '
	for w in ${1//-/ }; do cut -f2 "$2" | tr "-" "\n" | grep -qx -- "$w" || exit 1; done' _ "$PASS_GEN" "$HERE/eff_short_wordlist_1.txt"
check "adding sam twice is refused" bash -c '! "$1" add sam >/dev/null 2>&1' _ "$HERE/cartlink-users"
check "list shows sam" bash -c '[[ "$("$1" list)" == sam ]]' _ "$HERE/cartlink-users"
check "users file has a bcrypt hash for sam" grep -Eq '^sam \$2[aby]\$' "$CARTLINK_ROOT/users"
check "users file has no plain password" bash -c '! grep -qF "$1" "$2"' _ "$PASS_GEN" "$CARTLINK_ROOT/users"
check "users file keeps mode 0640" [ "$(stat -c %a "$CARTLINK_ROOT/users")" = 640 ]

# Two more generated passwords are different from each other and from sam's.
OUT2=$(users add amy 2>&1) || OUT2=
PASS_AMY=$(sed -n 's/^Password for amy: //p' <<<"$OUT2")
check "a second generated password is different" bash -c '[[ -n $1 && $1 != "$2" ]]' _ "$PASS_AMY" "$PASS_GEN"
check "--choose with 12+ characters adds kim" quiet choose kim <<<"$PASS_MINE"
check "--choose does not print a password" bash -c '! CARTLINK_PASS_STDIN=1 "$1" add --choose lee <<<"$2" | grep -q "^Password for"' _ "$HERE/cartlink-users" "$PASS_MINE"

# Without the word list: 24 random base32 characters.
OUT3=$(CARTLINK_WORDLIST="$TMP/no-such-list" users add ted 2>&1) || OUT3=
PASS_TED=$(sed -n 's/^Password for ted: //p' <<<"$OUT3")
check "without a word list the password is 24 base32 characters ($PASS_TED)" \
	bash -c '[[ $1 =~ ^[A-Z2-7]{24}$ ]]' _ "$PASS_TED"
# A changed word list (here: one word made a duplicate) is not used.
sed '2s/\t.*/\tacid/' "$HERE/eff_short_wordlist_1.txt" > "$TMP/changed-list.txt"
OUT4=$(CARTLINK_WORDLIST="$TMP/changed-list.txt" users add una 2>&1) || OUT4=
check "a changed word list (sha256 differs) falls back to base32" \
	bash -c '[[ $1 =~ ^[A-Z2-7]{24}$ ]]' _ "$(sed -n 's/^Password for una: //p' <<<"$OUT4")"
check "reload after add" reload

check "generated password logs in" [ "$(code -u "sam:$PASS_GEN" "$URL/")" = 200 ]
check "base32 password logs in" [ "$(code -u "ted:$PASS_TED" "$URL/")" = 200 ]
check "chosen password logs in" [ "$(code -u "kim:$PASS_MINE" "$URL/")" = 200 ]

LISTING=$(curl -sk -u "sam:$PASS_GEN" -H 'Accept: application/json' "$URL/" || true)
check "listing is a JSON array with \"Game Boy/\"" \
	jqok 'type == "array" and any(.[]; .name == "Game Boy/" and .is_dir == true)' "$LISTING"
check ".hidden is not in the listing" jqok 'type == "array" and all(.[]; .name != ".hidden")' "$LISTING"
SUB=$(curl -sk -u "sam:$PASS_GEN" -H 'Accept: application/json' "$URL/Game%20Boy/" || true)
check "folder listing has Test Game.gb" jqok 'any(.[]; .name == "Test Game.gb")' "$SUB"

curl -sk -u "sam:$PASS_GEN" -o "$TMP/download.gb" "$URL/Game%20Boy/Test%20Game.gb" || true
check "game downloads byte-identical" cmp -s "$TMP/download.gb" "$GAME"
check "/.hidden -> 404" [ "$(code -u "sam:$PASS_GEN" "$URL/.hidden")" = 404 ]
check "wrong password -> 401" [ "$(code -u "sam:$PASS_BAD" "$URL/")" = 401 ]
check "unknown user -> 401" [ "$(code -u "max:$PASS_GEN" "$URL/")" = 401 ]
check "access log names the user" grep -q '"user_id":"sam"' "$TMP/access.log"
check "access log has no password" bash -c '! grep -qF "$1" "$2" && ! grep -qF "$3" "$2"' _ "$PASS_GEN" "$TMP/access.log" "$PASS_MINE"

# --- cartlink-link: links with a fingerprint, read by Cartlink's parser ---
sha() { sha256sum "$1" | cut -d' ' -f1; }
# parsed LINK -> "url hex" from check_link, or nothing if refused
parsed() { "$CHECK_LINK" "$1" 2>/dev/null | paste -sd' '; }
LINK=$("$HERE/cartlink-link" "$URL/Game%20Boy/Test%20Game.gb" "$GAME" 2>/dev/null) || LINK=
check "cartlink-link: URL#sha256=<64 hex> ($LINK)" \
	bash -c '[[ $1 == "$2#sha256="* && ${1##*=} =~ ^[0-9a-f]{64}$ ]]' _ "$LINK" "$URL/Game%20Boy/Test%20Game.gb"
check "cartlink-link: Cartlink's parser accepts it, same URL and fingerprint" \
	[ "$(parsed "$LINK")" = "$URL/Game%20Boy/Test%20Game.gb $(sha "$GAME")" ]
check "cartlink-link refuses a URL that already has a #" \
	bash -c '! "$1" "$2#x" "$3" >/dev/null 2>&1' _ "$HERE/cartlink-link" "$URL/a.gb" "$GAME"
check "cartlink-link refuses a non-web URL" \
	bash -c '! "$1" "file:///etc/passwd" "$2" >/dev/null 2>&1' _ "$HERE/cartlink-link" "$GAME"
check "cartlink-link refuses a missing file" \
	bash -c '! "$1" "$2" "$3" >/dev/null 2>&1' _ "$HERE/cartlink-link" "$URL/a.gb" "$TMP/none.gb"

DIRLINKS=$("$HERE/cartlink-link" --dir "$CARTLINK_ROOT/games" "$URL" 2>/dev/null) || DIRLINKS=
check "cartlink-link --dir lists the 2 games (no hidden files)" [ "$(grep -c . <<<"$DIRLINKS")" = 2 ]
check "cartlink-link --dir skips hidden files and folders" bash -c '! grep -q "hidden\|secret" <<<"$1"' _ "$DIRLINKS"
dir_ok=1
while IFS= read -r l; do
	[[ -n $l ]] || continue
	p=$(parsed "$l")
	u=${p% *} h=${p##* }
	[[ -n $p && $u == "$URL/"* ]] || { dir_ok=0; echo "  refused or wrong: $l"; continue; }
	curl -sk -u "sam:$PASS_GEN" -o "$TMP/dl" "$u" || true
	[[ $(sha "$TMP/dl") == "$h" ]] || { dir_ok=0; echo "  download does not match its fingerprint: $l"; }
done <<<"$DIRLINKS"
check "cartlink-link --dir: every link parses and its download matches its fingerprint" [ "$dir_ok" = 1 ]
check "cartlink-link --dir: odd names are %-encoded (#, &, %, +, *, space, UTF-8)" \
	grep -qF "$URL/Game%20Boy/Odd%20%231%20%26%20100%25%2Bmore%2A%C3%A9.gb#sha256=$(sha "$ODD")" <<<"$DIRLINKS"

# cartlink-link edges: a folder named "-...", a symlinked folder, "?" in the base URL.
cp -r "$CARTLINK_ROOT/games" "$TMP/-dash"
DASH=$(cd "$TMP" && "$HERE/cartlink-link" --dir -dash "$URL/" 2>/dev/null) || DASH=
check "cartlink-link --dir works on a folder whose name starts with -" \
	bash -c '[[ $(grep -c "^$2/Game%20Boy/.*#sha256=" <<<"$1") == 2 ]]' _ "$DASH" "$URL"
ln -s "$CARTLINK_ROOT/games" "$TMP/games-link"
check "cartlink-link --dir refuses a symlinked folder" \
	bash -c '! "$1" --dir "$2" "$3" >/dev/null 2>&1' _ "$HERE/cartlink-link" "$TMP/games-link" "$URL/"
check "cartlink-link --dir refuses a base URL with ?" \
	bash -c '! "$1" --dir "$2" "$3" >/dev/null 2>&1' _ "$HERE/cartlink-link" "$CARTLINK_ROOT/games" "$URL/?a=1"
check "cartlink-link --dir refuses a base URL with #" \
	bash -c '! "$1" --dir "$2" "$3" >/dev/null 2>&1' _ "$HERE/cartlink-link" "$CARTLINK_ROOT/games" "$URL/#x"

# --- fail2ban filter catches the failed logins in the real access log ---
if command -v fail2ban-regex >/dev/null; then
	for _ in 1 2 3; do code -u "sam:$PASS_BAD" "$URL/" >/dev/null; done
	sleep 0.5
	N401=$(jq -s '[.[] | select(.status == 401)] | length' "$TMP/access.log")
	F2B=$(fail2ban-regex "$TMP/access.log" "$HERE/fail2ban/cartlink-filter.conf" 2>&1 || true)
	MATCHED=$(sed -n 's/^Lines: .* \([0-9]\+\) matched.*/\1/p' <<<"$F2B")
	check "fail2ban filter matches every 401 ($N401) and nothing else (${MATCHED:-none})" \
		bash -c '[[ $1 -ge 4 && $1 == "$2" ]]' _ "$N401" "$MATCHED"
	check "fail2ban filter finds the client address 127.0.0.1" grep -q '127\.0\.0\.1' <<<"$F2B"
	check "fail2ban jail file uses the cartlink filter and log" \
		bash -c 'grep -q "^filter *= *cartlink$" "$1" && grep -q "^logpath *= */var/log/caddy/cartlink.log$" "$1"' _ "$HERE/fail2ban/cartlink-jail.local"
else
	echo "SKIP: fail2ban filter (fail2ban-regex not installed)"
fi

LISTEN=$(ss -Hltnp | grep "pid=$CADDY_PID," | awk '{print $4}' | sort -u | tr '\n' ' ')
check "Caddy listens only on 127.0.0.1:$PORT + admin ($LISTEN)" \
	bash -c '[[ "$1" == "127.0.0.1:$2 127.0.0.1:$3 " || "$1" == "127.0.0.1:$3 127.0.0.1:$2 " ]]' _ "$LISTEN" "$PORT" "$ADMIN"

check "remove sam" quiet users remove sam
check "removing an unknown name fails" bash -c '! "$1" remove nobody >/dev/null 2>&1' _ "$HERE/cartlink-users"
check "reload after remove" reload
check "removed user -> 401" [ "$(code -u "sam:$PASS_GEN" "$URL/")" = 401 ]
check "other friends still log in after a remove" [ "$(code -u "kim:$PASS_MINE" "$URL/")" = 200 ]

# --- setup.sh refuses planted links in the server folder (it runs as root) ---
lay() { CARTLINK_ROOT=$1 "$HERE/setup.sh" --check-layout >/dev/null 2>&1; }
refused() { ! lay "$1"; }
check "layout check passes on the test folder" lay "$CARTLINK_ROOT"
for bad in users games duckdns.token; do
	mkdir -p "$TMP/bad-$bad/games"; : > "$TMP/bad-$bad/users"
	rm -rf "${TMP:?}/bad-$bad/$bad"; ln -s /etc/passwd "$TMP/bad-$bad/$bad"
	check "layout check refuses a symlinked $bad" refused "$TMP/bad-$bad"
done
mkdir -p "$TMP/bad-link"; ln -s "$CARTLINK_ROOT" "$TMP/bad-link/root"
check "layout check refuses a symlinked server folder" refused "$TMP/bad-link/root"
mkdir -p "$TMP/bad-hard/games"; : > "$TMP/bad-hard/users"; ln "$TMP/bad-hard/users" "$TMP/bad-hard/other"
check "layout check refuses a hard-linked users file" refused "$TMP/bad-hard"
ln -s /etc "$CARTLINK_ROOT/games/linked-etc"
check "--check-layout warns about links inside games/ (and still passes)" \
	bash -c 'CARTLINK_ROOT="$1" "$2" --check-layout 2>&1 >/dev/null | grep -q "linked-etc"' _ "$CARTLINK_ROOT" "$HERE/setup.sh"
check "cartlink-users list warns about links inside games/" \
	bash -c '"$1" list 2>&1 >/dev/null | grep -q "has links in it"' _ "$HERE/cartlink-users"
# Many links: the warning must not kill the scripts (SIGPIPE under pipefail).
mkdir -p "$CARTLINK_ROOT/games/many"
for i in $(seq 1 3000); do ln -s /etc "$CARTLINK_ROOT/games/many/l$i"; done
OUTM=$(users add moe 2>"$TMP/moe.err") && RCM=0 || RCM=$?
check "add with 3000 links in games/ succeeds (rc $RCM)" [ "$RCM" = 0 ]
check "add with 3000 links still shows the password" grep -q '^Password for moe: ' <<<"$OUTM"
check "the warning lists 5 links, each indented" \
	bash -c '[[ $(grep -c "^    /" "$1") == 5 && $(grep -vc "^WARNING\|^    /" "$1") == 0 ]]' _ "$TMP/moe.err"
check "list with 3000 links succeeds" quiet users list
check "--check-layout with 3000 links succeeds" quiet env CARTLINK_ROOT="$CARTLINK_ROOT" "$HERE/setup.sh" --check-layout
rm -rf "$CARTLINK_ROOT/games/many"
rm "$CARTLINK_ROOT/games/linked-etc"
check "no warning without links" bash -c '[[ -z $("$1" list 2>&1 >/dev/null) ]]' _ "$HERE/cartlink-users"
check "cartlink-users refuses a symlinked users file" \
	bash -c '! CARTLINK_ROOT="$1" "$2" list >/dev/null 2>&1' _ "$TMP/bad-users" "$HERE/cartlink-users"
printf 'zed $2a$14$xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' > "$CARTLINK_ROOT/users" # no final newline
quiet users add sam || true
check "add keeps lines apart when the file has no final newline" \
	bash -c '[[ "$("$1" list | tr "\n" " ")" == "zed sam " ]]' _ "$HERE/cartlink-users"

# --- setup.sh tells a stock Caddyfile from a changed one (Arch + Debian) ---
# Fake pacman / dpkg / dpkg-query first on PATH; FAKE_* say what they report.
FAKE=$TMP/fakebin
mkdir -p "$FAKE"
cat > "$FAKE/pacman" <<'EOF'
#!/usr/bin/env bash
[[ ${FAKE_PKG:-} == arch ]] || exit 1
case $1 in
	-Qq) echo caddy ;;
	-Qii) printf 'Name            : caddy\nBackup Files    : %s %s\n' "$FAKE_CADDYFILE" "$FAKE_STATE" ;;
esac
EOF
cat > "$FAKE/dpkg-query" <<'EOF'
#!/usr/bin/env bash
[[ ${FAKE_PKG:-} == debian ]] || exit 1
case "$*" in
	*Status*) echo 'install ok installed' ;;
	*Conffiles*) printf ' %s %s\n' "$FAKE_CADDYFILE" "$FAKE_MD5" ;;
esac
EOF
cat > "$FAKE/dpkg" <<'EOF'
#!/usr/bin/env bash
[[ ${FAKE_PKG:-} == debian && $1 == --verify ]] || exit 1
[[ -z ${FAKE_VERIFY:-} ]] || { echo "$FAKE_VERIFY"; exit 1; }
EOF
cat > "$FAKE/caddy" <<'EOF'
#!/usr/bin/env bash
[[ $1 == version ]] && echo "${FAKE_CADDY_VERSION-v2.11.7 h1:test=}"
EOF
chmod +x "$FAKE"/*
CF=$TMP/etc-caddy/Caddyfile
mkdir -p "${CF%/*}"
printf ':80 {\n\troot * /usr/share/caddy\n\tfile_server\n}\n' > "$CF"
MD5=$(md5sum "$CF" | cut -d' ' -f1)
sys() { # sys PKG ... -> output of setup.sh --check-system
	env PATH="$FAKE:$PATH" CARTLINK_CADDYFILE="$CF" FAKE_CADDYFILE="$CF" FAKE_PKG="$@" \
		"$HERE/setup.sh" --check-system 2>&1 || true
}
check "Debian: unchanged sample Caddyfile is seen as stock" \
	grep -qx 'Caddyfile: stock' <<<"$(sys debian FAKE_MD5="$MD5")"
check "Debian: dpkg --verify reporting the file -> changed" \
	grep -qx 'Caddyfile: changed' <<<"$(sys debian FAKE_MD5="$MD5" FAKE_VERIFY="??5??????  c $CF")"
check "Debian: different checksum -> changed" \
	grep -qx 'Caddyfile: changed' <<<"$(sys debian FAKE_MD5=0123456789abcdef0123456789abcdef)"
check "Debian: the replacement Caddyfile has no Arch admin socket" \
	bash -c '! grep -q admin.socket <<<"$1" && grep -q "^import /etc/caddy/conf.d/\*$" <<<"$1"' _ \
	"$(env PATH="$FAKE:$PATH" FAKE_PKG=debian "$HERE/setup.sh" --print-main 2>&1)"
check "Arch: unmodified sample Caddyfile is seen as stock" \
	grep -qx 'Caddyfile: stock' <<<"$(sys arch FAKE_STATE='[unmodified]')"
check "Arch: modified -> changed" \
	grep -qx 'Caddyfile: changed' <<<"$(sys arch FAKE_STATE='[modified]')"
check "Arch: the replacement Caddyfile keeps the admin socket" \
	grep -q 'admin "unix//run/caddy/admin.socket"' <<<"$(env PATH="$FAKE:$PATH" FAKE_PKG=arch "$HERE/setup.sh" --print-main 2>&1)"
for v in 'v2.6.2 h1:x=' 'v2.7.6 h1:x=' 'v1.0.4' '(devel)' ''; do
	OUTV=$(sys debian FAKE_MD5="$MD5" FAKE_CADDY_VERSION="$v")
	check "Caddy '$v' is refused (needs 2.8 or newer)" \
		bash -c 'grep -q "Cartlink needs Caddy 2.8 or newer" <<<"$1" && grep -q "Nothing was changed" <<<"$1" && ! grep -q "^Caddyfile:" <<<"$1"' _ "$OUTV"
done
for v in 'v2.8.0 h1:x=' 'v2.10.2 h1:x=' 'v2.11.0-beta.1 h1:x=' 'v3.0.0'; do
	check "Caddy '$v' is accepted" grep -q '^caddy: .*ok' <<<"$(sys debian FAKE_MD5="$MD5" FAKE_CADDY_VERSION="$v")"
done
check "other systems: 'Arch and Debian/Ubuntu only'" \
	grep -q 'Arch and Debian/Ubuntu only' <<<"$(sys none)"
check "a real setup refuses the CARTLINK_CADDYFILE test override" \
	bash -c 'CARTLINK_CADDYFILE=/x "$1" example.duckdns.org /nonexistent 2>&1 | grep -q "unset CARTLINK"' _ "$HERE/setup.sh"

# --- DuckDNS updater refuses a missing token file and never prints a token ---
check "duckdns-update refuses a missing token file" bash -c '! "$1" example "$2" >/dev/null 2>&1' _ "$HERE/duckdns-update.sh" "$TMP/none"

echo
if ((fails)); then
	echo "$fails check(s) FAILED"
	sed 's/^/caddy: /' "$TMP/caddy.out" | tail -5
	exit 1
fi
echo "All checks passed."
