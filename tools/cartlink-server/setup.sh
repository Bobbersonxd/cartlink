#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
#
# Set up (or update) the Cartlink game server on this PC. Safe to run again.
# Arch Linux and Debian/Ubuntu only. See SETUP.md next to this file.
#   sudo tools/cartlink-server/setup.sh NAME.duckdns.org TOKEN_FILE
#   tools/cartlink-server/setup.sh --print-site DOMAIN  (only prints the Caddy site file)
#   tools/cartlink-server/setup.sh --check-layout       (only checks the folder for planted links)
#   tools/cartlink-server/setup.sh --check-system       (only says which system and whether
#                                                        /etc/caddy/Caddyfile is the package's sample)
#   tools/cartlink-server/setup.sh --print-main         (only prints the Caddyfile that would
#                                                        replace the package's sample)
#
# What it changes, and nothing else:
#   /srv/cartlink/                 root's folder (0755) holding games/ (yours,
#                                  group caddy), users (yours, 0640), duckdns.token (root, 0600)
#   /etc/caddy/conf.d/cartlink.caddy   the site file (from Caddyfile.template)
#   /etc/caddy/Caddyfile           only if needed to add
#                                  "import /etc/caddy/conf.d/*" (see below)
#   /var/log/caddy/                only made (owner caddy) if it is missing
#   /etc/hosts                     one line "127.0.0.1 NAME.duckdns.org # cartlink"
#   /usr/local/lib/cartlink/duckdns-update.sh
#   /etc/systemd/system/cartlink-duckdns.{service,timer}
#   systemd: enables cartlink-duckdns.timer and caddy, reloads caddy
# Other Caddy sites and other programs are left alone.
set -euo pipefail
shopt -u patsub_replacement 2>/dev/null || true # keep "&" literal in ${x//a/b}

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=${CARTLINK_ROOT:-/srv/cartlink}
TLS=${CARTLINK_TLS:-}
LOG=${CARTLINK_LOG:-/var/log/caddy/cartlink.log}
CADDY_DIR=/etc/caddy
SITE_FILE=$CADDY_DIR/conf.d/cartlink.caddy
MAIN_FILE=${CARTLINK_CADDYFILE:-$CADDY_DIR/Caddyfile}
IMPORT_LINE='import /etc/caddy/conf.d/*'
HOSTS=/etc/hosts
MARK='# cartlink'

say() { echo "==> $*"; }
warn() { echo "WARNING: $*" >&2; }
die() { echo "setup.sh: $*" >&2; exit 1; }
usage() {
	echo "Usage: sudo $0 NAME.duckdns.org TOKEN_FILE" >&2
	echo "       $0 --print-site DOMAIN" >&2
	echo "       $0 --check-layout          (read-only safety check of the server folder)" >&2
	echo "       $0 --check-system          (read-only: system and Caddyfile state)" >&2
	echo "       $0 --print-main            (prints the Caddyfile that replaces the sample)" >&2
	exit 2
}

check_domain() {
	if [[ ${CARTLINK_TEST_DOMAIN:-} == 1 && $1 =~ ^https://127\.0\.0\.1:[0-9]{1,5}$ ]]; then return; fi
	[[ $1 =~ ^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.duckdns\.org$ ]] ||
		die "the name must look like yourname.duckdns.org (lower case), got '$1'"
}

# Print the site file made from Caddyfile.template.
render_site() { # render_site DOMAIN
	local t
	check_domain "$1"
	[[ $ROOT =~ ^/[A-Za-z0-9._/-]+$ ]] || die "CARTLINK_ROOT must be a plain absolute path"
	[[ $LOG =~ ^/[A-Za-z0-9._/-]+$ ]] || die "CARTLINK_LOG must be a plain absolute path"
	[[ -z $TLS || $TLS == 'tls internal' ]] || die "CARTLINK_TLS may only be empty or 'tls internal'"
	t=$(<"$HERE/Caddyfile.template")
	t=${t//'{{DOMAIN}}'/$1}
	t=${t//'{{ROOT}}'/$ROOT}
	t=${t//'{{TLS}}'/$TLS}
	t=${t//'{{LOG}}'/$LOG}
	[[ $t != *'{{'* ]] || die "Caddyfile.template has an unknown {{...}} placeholder"
	printf '%s\n' "$t"
}

# Write CONTENT to FILE only if it differs. Returns 0 if it changed.
write_if_changed() { # write_if_changed FILE MODE CONTENT
	if [[ -f $1 ]] && [[ "$(<"$1")" == "$3" ]]; then return 1; fi
	local tmp
	tmp=$(mktemp)
	printf '%s\n' "$3" > "$tmp"
	install -D -m "$2" -o root -g root "$tmp" "$1"
	rm -f "$tmp"
	return 0
}

# Which package manager installed Caddy: SYSTEM=arch, debian or "" (other).
detect_system() {
	SYSTEM=
	if command -v pacman >/dev/null && pacman -Qq caddy >/dev/null 2>&1; then
		SYSTEM=arch
	elif command -v dpkg-query >/dev/null &&
		[[ $(dpkg-query -W -f='${Status}' caddy 2>/dev/null) == 'install ok installed' ]]; then
		SYSTEM=debian
	fi
}

install_hint() {
	if command -v pacman >/dev/null; then
		echo "    sudo pacman -S caddy"
	elif command -v apt-get >/dev/null; then
		echo "    sudo apt install caddy"
		echo "(Debian 12+ / Ubuntu 24.04+; older ones: https://caddyserver.com/docs/install)"
	else
		echo "    (Cartlink's setup supports Arch and Debian/Ubuntu only)"
	fi
}

stock_caddyfile() { # true if the Caddyfile is still the package's sample
	local want have verify
	case $SYSTEM in
	arch)
		LC_ALL=C pacman -Qii caddy 2>/dev/null |
			grep -Eq "(Backup Files *: *| +)$MAIN_FILE \[unmodified\]"
		;;
	debian)
		# Stock only if it is the package's conffile, its checksum is still
		# the packaged one, and dpkg --verify doesn't report it as changed.
		want=$(LC_ALL=C dpkg-query -W -f='${Conffiles}\n' caddy 2>/dev/null |
			awk -v f="$MAIN_FILE" '$1 == f && $3 != "obsolete" { print $2 }')
		[[ $want =~ ^[0-9a-f]{32}$ ]] || return 1
		have=$(md5sum < "$MAIN_FILE" 2>/dev/null) || return 1
		[[ ${have%% *} == "$want" ]] || return 1
		# (dpkg --verify exits non-zero when it finds a change, so only its
		# output counts.)
		verify=$(LC_ALL=C dpkg --verify caddy 2>/dev/null || true)
		! awk -v f="$MAIN_FILE" '$NF == f { found = 1 } END { exit !found }' <<<"$verify"
		;;
	*) return 1 ;;
	esac
}

# The Caddyfile that replaces the package's sample: the conf.d import, plus on
# Arch the admin socket its caddy.service reloads through.
main_text() {
	local admin=()
	if [[ $SYSTEM == arch ]]; then
		admin=('{' $'\tadmin "unix//run/caddy/admin.socket"' '}' '')
	fi
	printf '%s\n' \
		"# Written by Cartlink's setup.sh. The package's sample" \
		'# (a welcome page on port 80) is in Caddyfile.package-sample.' \
		"${admin[@]}" \
		"$IMPORT_LINE"
}

check_system() {
	detect_system
	[[ -n $SYSTEM ]] || die "Caddy from pacman or apt was not found. Cartlink's setup supports Arch and Debian/Ubuntu only."
	echo "system: $SYSTEM"
	if [[ ! -f $MAIN_FILE ]]; then echo "Caddyfile: missing"
	elif stock_caddyfile; then echo "Caddyfile: stock"
	else echo "Caddyfile: changed"; fi
}

main_caddyfile() {
	if [[ ! -f $MAIN_FILE ]] || stock_caddyfile; then
		# The package's sample Caddyfile serves a welcome page to anyone on
		# port 80. Once port 80 is forwarded that would be open to the
		# internet without a login, so the sample is put aside and replaced
		# by its admin setting + the conf.d import.
		if [[ -f $MAIN_FILE && ! -e $MAIN_FILE.package-sample ]]; then
			cp -p "$MAIN_FILE" "$MAIN_FILE.package-sample"
			say "Saved the sample Caddyfile as $MAIN_FILE.package-sample"
		fi
		local main
		main=$(main_text)
		write_if_changed "$MAIN_FILE" 0644 "$main" && say "Wrote $MAIN_FILE"
		return 0
	fi
	# A Caddyfile someone changed: keep it, only make sure it has the import.
	if ! grep -Eq '^[[:space:]]*import[[:space:]]+/etc/caddy/conf\.d/\*[[:space:]]*$' "$MAIN_FILE"; then
		printf '\n# Added by Cartlink setup.sh\n%s\n' "$IMPORT_LINE" >> "$MAIN_FILE"
		say "Added '$IMPORT_LINE' to $MAIN_FILE"
	fi
	if grep -Eq '^[[:space:]]*(http://|:80)([[:space:],{]|$)' "$MAIN_FILE"; then
		warn "$MAIN_FILE has a site on plain port 80. With port 80 forwarded it is open"
		warn "to the internet without a login. Remove it unless you really want it."
	fi
}

hosts_line() { # make sure /etc/hosts has exactly one "# cartlink" line for DOMAIN
	local want="127.0.0.1 $1 $MARK" now
	now=$(grep -F -- "$MARK" "$HOSTS" || true)
	[[ $now == "$want" ]] && return
	local tmp
	tmp=$(mktemp)
	grep -vF -- "$MARK" "$HOSTS" > "$tmp" || true
	printf '%s\n' "$want" >> "$tmp"
	cat "$tmp" > "$HOSTS" # in place: keeps the file's owner and mode
	rm -f "$tmp"
	say "Pointed $1 at this PC in $HOSTS (so the same link works at home)"
}

# Safety checks on the server folder (read-only). setup.sh runs as root, so it
# must never follow a link that the normal account could have planted there.
check_layout() {
	[[ ! -L $ROOT ]] || die "$ROOT is a symlink, refusing (remove it and run again)"
	[[ ! -e $ROOT || -d $ROOT ]] || die "$ROOT is not a folder, refusing"
	local f
	for f in games users duckdns.token; do
		[[ ! -L $ROOT/$f ]] || die "$ROOT/$f is a symlink, refusing (remove it and run again)"
	done
	[[ ! -e $ROOT/games || -d $ROOT/games ]] || die "$ROOT/games is not a folder, refusing"
	if [[ -e $ROOT/users ]]; then
		[[ -f $ROOT/users ]] || die "$ROOT/users is not a plain file, refusing"
		[[ $(stat -c %h "$ROOT/users") == 1 ]] || die "$ROOT/users has extra hard links, refusing"
	fi
}

layout() { # layout OWNER
	# The top folder is root's, so the normal account can't swap what is in it
	# (users, games, duckdns.token) for links. Check first, then take it over;
	# from then on nothing in it can change under us.
	check_layout
	install -d -o root -g root -m 0755 "$ROOT"
	check_layout
	install -d -o "$1" -g caddy -m 2750 "$ROOT/games"
	# Games: owner can change them, Caddy can read them, nobody else.
	# chown -R -h never follows links; chmod runs as the owner, not as root.
	chown -R -h "$1:caddy" "$ROOT/games"
	runuser -u "$1" -- find "$ROOT/games" -type d -exec chmod 2750 {} +
	runuser -u "$1" -- find "$ROOT/games" -type f -exec chmod 0640 {} +
	if [[ ! -e $ROOT/users ]]; then
		install -m 0640 -o "$1" -g caddy /dev/null "$ROOT/users"
		say "Made an empty friends list $ROOT/users"
	else
		chown -h "$1:caddy" "$ROOT/users"
		chmod 0640 "$ROOT/users"
	fi
}

token() { # token TOKEN_FILE   (stored in the root-owned $ROOT, mode 0600)
	local t tmp
	[[ -r $1 ]] || die "can't read the token file $1"
	t=$(tr -d '[:space:]' < "$1")
	[[ $t =~ ^[0-9a-f-]{36}$ ]] || die "$1 doesn't look like a DuckDNS token (copy it from duckdns.org)"
	tmp=$(mktemp "$ROOT/.duckdns.token.XXXXXX") # new file, 0600, root's
	printf '%s\n' "$t" > "$tmp"
	mv -f "$tmp" "$ROOT/duckdns.token" # rename replaces a link, never follows it
}

duckdns() { # duckdns NAME
	local unit
	install -D -m 0755 -o root -g root "$HERE/duckdns-update.sh" /usr/local/lib/cartlink/duckdns-update.sh
	unit=$(<"$HERE/cartlink-duckdns.service")
	write_if_changed /etc/systemd/system/cartlink-duckdns.service 0644 "${unit//'{{NAME}}'/$1}" || true
	write_if_changed /etc/systemd/system/cartlink-duckdns.timer 0644 "$(<"$HERE/cartlink-duckdns.timer")" || true
	systemctl daemon-reload
	systemctl enable --now cartlink-duckdns.timer
	if systemctl start cartlink-duckdns.service; then
		say "DuckDNS now points $1.duckdns.org at this home"
	else
		warn "the DuckDNS update failed; see: journalctl -u cartlink-duckdns"
	fi
}

site() { # site DOMAIN  -> writes the site file, validates, rolls back on error
	local new backup=
	new=$(render_site "$1")
	install -d -m 0755 "$CADDY_DIR/conf.d"
	if [[ -f $SITE_FILE ]]; then
		backup=$(mktemp)
		cp -p "$SITE_FILE" "$backup"
	fi
	write_if_changed "$SITE_FILE" 0644 "$new" && say "Wrote $SITE_FILE"
	say "Checking the Caddy setup"
	# Validate as the caddy user, so it also proves Caddy can read the users file.
	if ! runuser -u caddy -- env HOME=/var/lib/caddy XDG_DATA_HOME=/var/lib/caddy XDG_CONFIG_HOME=/var/lib/caddy \
		caddy validate --config "$MAIN_FILE" --adapter caddyfile >/dev/null; then
		if [[ -n $backup ]]; then cp -p "$backup" "$SITE_FILE"; else rm -f "$SITE_FILE"; fi
		rm -f "$backup"
		die "Caddy didn't accept the setup (error above); the old site file is back"
	fi
	rm -f "$backup"
}

log_dir() { # Caddy (user caddy) writes the access log; make its folder if missing
	local d=${LOG%/*}
	[[ ! -L $d ]] || die "$d is a symlink, refusing"
	if [[ ! -e $d ]]; then
		install -d -o caddy -g caddy -m 0750 "$d"
		say "Made $d for Caddy's logs"
	fi
}

start_caddy() {
	systemctl enable caddy
	if systemctl is-active --quiet caddy; then
		systemctl reload caddy
		say "Reloaded Caddy"
	else
		systemctl start caddy
		say "Started Caddy"
	fi
}

main() {
	local domain=$1 token_file=$2 owner name
	# The test overrides are only for the read-only modes; the system units expect /srv/cartlink.
	[[ -z ${CARTLINK_ROOT:-}${CARTLINK_TLS:-}${CARTLINK_LOG:-}${CARTLINK_TEST_DOMAIN:-}${CARTLINK_CADDYFILE:-} ]] ||
		die "unset CARTLINK_ROOT/CARTLINK_TLS/CARTLINK_LOG/CARTLINK_TEST_DOMAIN/CARTLINK_CADDYFILE for a real setup"
	((EUID == 0)) || die "run it with sudo: sudo $0 $domain $token_file"
	if ! command -v caddy >/dev/null; then
		echo "Caddy is not installed. Install it first with:"
		install_hint
		exit 1
	fi
	detect_system
	[[ -n $SYSTEM ]] || die "Caddy was not installed with pacman or apt. Cartlink's setup supports Arch and Debian/Ubuntu only."
	getent group caddy >/dev/null || die "the caddy group is missing (reinstall caddy)"
	owner=${SUDO_USER:-}
	[[ -n $owner && $owner != root ]] || die "run it with sudo from your normal account"
	check_domain "$domain"
	name=${domain%.duckdns.org}

	layout "$owner"
	token "$token_file"
	log_dir
	main_caddyfile
	site "$domain"
	hosts_line "$domain"
	duckdns "$name"
	start_caddy

	echo
	say "Done. Your server link: https://$domain/"
	echo "    Add a friend:  $HERE/cartlink-users add NAME"
	echo "    Games go in:   $ROOT/games/<System>/"
}

case ${1:-} in
	--print-site) [[ $# == 2 ]] || usage; render_site "$2" ;;
	--check-layout) [[ $# == 1 ]] || usage; check_layout; echo "layout ok: $ROOT" ;;
	--check-system) [[ $# == 1 ]] || usage; check_system ;;
	--print-main) [[ $# == 1 ]] || usage; detect_system; main_text ;;
	-h | --help | '') usage ;;
	*) [[ $# == 2 ]] || usage; main "$1" "$2" ;;
esac
