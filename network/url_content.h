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

/* Rewrites share links (Google Drive, Dropbox, OneDrive, archive.org)
 * into direct-download links. Other URLs are copied unchanged. */
void url_content_fix_link(char *out, size_t len, const char *url);

/* Extracts the file name from a Content-Disposition header line
 * (with or without the "Content-Disposition:" prefix). Prefers
 * filename*=charset''pct-encoded over filename=. Returns false if
 * there is no non-empty file name. The result is NOT sanitized. */
bool url_content_name_from_disposition(char *out, size_t len,
      const char *header_line);

/* Cache file name: 16 hex chars of SHA1(fixed_url), '_', then a
 * sanitized file name (name_or_null if non-empty, else the URL's last
 * path segment, percent-decoded, else "download.bin"). */
void url_content_cache_name(char *out, size_t len,
      const char *fixed_url, const char *name_or_null);

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

/* Test hook: forces the digest client nonce. NULL restores random. */
void url_content_set_test_cnonce(const char *cnonce);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
