/*  RetroArch - A frontend for libretro.
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* "Load from URL" helpers: pure string/hash logic, no I/O. */

#ifndef __RARCH_URL_CONTENT_H
#define __RARCH_URL_CONTENT_H

#ifdef HAVE_NETWORKING

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* True for http:// or https:// URLs whose path extension is not a
 * streamable media type (mp4, mkv, avi, webm, flv, ogm, 3gp, 3g2,
 * mp3, ogg, flac, wav, m3u8). */
bool url_content_is_downloadable(const char *path);

/* Command-line gate: true if the content argument should be downloaded
 * before loading. Always false when first_pass is false: only the
 * first argument parse is the real command line; later parses are
 * RetroArch's own content loads (menu, history, netplay) and must
 * behave as upstream. Also false if it is not downloadable, if the core's file
 * name (core_path) contains "ffmpeg" or "mpv" in any case (media cores
 * keep every URL, even extensionless streams), and, when no core is
 * given, if builtin_player is true (the built-in media player takes
 * URLs, as before). */
bool url_content_cli_should_fetch(bool first_pass, const char *content,
      const char *core_path, bool builtin_player);

/* Splits an optional fingerprint off a link: url_out gets the URL
 * without its fragment (everything from the first '#'). A fragment
 * "sha256=<64 hex>" (key and hex in any case) sets *has_fp and the 32
 * bytes in sha256_out; any other fragment is dropped and ignored
 * (*has_fp false). False (and an empty url_out, *has_fp false) for a
 * broken fingerprint (a "sha256=" fragment that is not exactly 64 hex
 * digits) or if the URL does not fit: such a link is refused rather
 * than loaded without its check. Fragments are never sent to a server
 * and never part of the cache name. */
bool url_content_split_fingerprint(char *url_out, size_t len,
      uint8_t sha256_out[32], bool *has_fp, const char *url);

/* Rewrites share links (Google Drive, Dropbox, OneDrive, archive.org)
 * into direct-download links; http:// archive.org links become
 * https://. Other URLs are copied unchanged. */
void url_content_fix_link(char *out, size_t len, const char *url);

/* Extracts the file name from a Content-Disposition header line
 * (with or without the "Content-Disposition:" prefix). Prefers
 * filename*=charset''pct-encoded over filename=. Returns false if
 * there is no non-empty file name. The result is NOT sanitized. */
bool url_content_name_from_disposition(char *out, size_t len,
      const char *header_line);

/* Cache file name: 16 hex chars of SHA1(fixed_url), '_', then a
 * sanitized file name (name_or_null if non-empty, else the URL's last
 * path segment, percent-decoded, else "download.bin"). The name is
 * cut to at most 100 bytes on a UTF-8 character boundary, keeping an
 * extension of up to 16 bytes. */
void url_content_cache_name(char *out, size_t len,
      const char *fixed_url, const char *name_or_null);

/* Absolute URL for ref (a link or Location value) seen at base:
 * absolute http(s), scheme-relative, absolute path, or relative path
 * (joined to base's folder). Dot segments are NOT removed. False for
 * other schemes, an empty ref, or if it does not fit. */
bool url_content_resolve(char *out, size_t len, const char *base,
      const char *ref);

/* Lower-case host of a URL, without userinfo or port. */
void url_content_host(char *out, size_t len, const char *url);

/* Writes "Authorization: Basic <base64(user:pass)>\r\n". */
void url_content_basic_auth_header(char *out, size_t len,
      const char *user, const char *pass);

/* True for archive.org and any *.archive.org host (case-insensitive). */
bool url_content_host_is_archive(const char *host);

/* Writes "Authorization: LOW <access>:<secret>\r\n" (archive.org S3 keys). */
void url_content_archive_auth_header(char *out, size_t len,
      const char *access, const char *secret);

/* Builds an RFC 2617 MD5 digest "Authorization: Digest ...\r\n" header
 * from a WWW-Authenticate challenge (with or without the header name).
 * Supports qop=auth and no qop, algorithm MD5 / MD5-sess.
 * Returns false if the challenge cannot be answered. */
bool url_content_digest_auth_header(char *out, size_t len,
      const char *www_authenticate, const char *method, const char *uri,
      const char *user, const char *pass);

/* True if the first 512 bytes (after whitespace and a UTF-8 BOM) start
 * with "<!doctype html" or "<html" (case-insensitive). */
bool url_content_looks_like_html(const uint8_t *data, size_t len);

/* application/x-www-form-urlencoded value: unreserved characters
 * (A-Z a-z 0-9 - . _ ~) as is, space as '+', everything else %XX.
 * Returns false (and an empty out) if the result does not fit. */
bool url_content_form_encode(char *out, size_t len, const char *s);

/* Reads an archive.org xauthn login reply. True only for
 * "success": true with non-empty values.s3.access and values.s3.secret
 * that fit; then err is "". Otherwise access and secret are emptied and
 * err gets a short reason (from values.reason or error). */
bool url_content_parse_archive_login(const char *json, size_t json_len,
      char *access, size_t alen, char *secret, size_t slen,
      char *err, size_t elen);

/* Zeroes len bytes at p through a volatile pointer, so the compiler
 * cannot drop it. For passwords, keys and auth headers. NULL is fine. */
void url_content_wipe(void *p, size_t len);

/* Test hook: forces the digest client nonce. NULL restores random. */
void url_content_set_test_cnonce(const char *cnonce);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
