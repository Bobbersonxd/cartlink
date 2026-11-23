#!/usr/bin/env bash
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
#
# Point NAME.duckdns.org at this home's public IP address.
#   duckdns-update.sh NAME TOKEN_FILE
# Run every 5 minutes by cartlink-duckdns.timer. The token is read from the
# file and sent to curl on stdin, so it never shows up in the process list
# or in the logs.
set -euo pipefail

die() { echo "duckdns-update: $*" >&2; exit 1; }

[[ $# == 2 ]] || die "usage: duckdns-update.sh NAME TOKEN_FILE"
name=$1
token_file=$2
[[ $name =~ ^[a-z0-9-]{1,63}$ ]] || die "NAME must be the part before .duckdns.org"
[[ -r $token_file ]] || die "can't read the token file $token_file"
token=$(tr -d '[:space:]' < "$token_file")
[[ $token =~ ^[0-9a-f-]{36}$ ]] || die "the token file doesn't look like a DuckDNS token"

# An empty ip= lets DuckDNS use the address the request came from.
reply=$(printf 'url = "https://www.duckdns.org/update?domains=%s&token=%s&ip="\n' "$name" "$token" |
	curl --silent --show-error --max-time 30 --config -) || die "could not reach duckdns.org"
[[ $reply == OK ]] || die "DuckDNS said '$reply' (check the name and the token)"
echo "DuckDNS updated for $name.duckdns.org"
