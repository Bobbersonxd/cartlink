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

/* "Load from URL" blocking fetcher: downloads one URL into a cache
 * directory as <16 hex of SHA1(fixed url)>_<name>, via a .part file. */

#ifndef __RARCH_URL_FETCH_H
#define __RARCH_URL_FETCH_H

#ifdef HAVE_NETWORKING

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "url_catalog.h"

RETRO_BEGIN_DECLS

enum url_fetch_result
{
   URL_FETCH_OK = 0,        /* downloaded, out_path set */
   URL_FETCH_CACHED,        /* already in cache, out_path set */
   URL_FETCH_NEED_LOGIN,    /* 401, challenge copied out */
   URL_FETCH_NOT_A_FILE,    /* got an HTML page */
   URL_FETCH_TOO_BIG,       /* allocation failed */
   URL_FETCH_FAILED,        /* network / HTTP error, msg in err */
   URL_FETCH_CATALOG,       /* a Caddy folder listing (game list) */
   URL_FETCH_BLOCKED_PROGRAM, /* a program, not a game: nothing kept */
   URL_FETCH_BLOCKED_ARCHIVE, /* archive with a program/unsafe path/bomb, or unreadable */
   URL_FETCH_BLOCKED_TOO_BIG, /* archive unpacks to more than 2 GiB */
   URL_FETCH_BLOCKED_FINGERPRINT, /* #sha256= did not match: nothing kept */
   URL_FETCH_CERT_INVALID,    /* https certificate did not verify (no login
                                 sent): may be retried with download_anyway */
   URL_FETCH_LOGIN_HTTP       /* a plain http server answered 401: no login
                                 is ever sent over http, no prompt */
};

typedef void (*url_fetch_progress_t)(size_t done, size_t total, void *ud);

/* Blocking. Fixes the link, returns CACHED if a cache file for it
 * exists, else downloads it. Auth: user/pass if user is non-empty,
 * else the saved login for the link's site (url_logins: https only,
 * scheme://host[:port]), else archive.org keys (https only) for hosts
 * ending in archive.org. A login goes only to its own https site (same
 * host and port; an http link the server moves to https on the same
 * host counts as that https site), only with a verified certificate,
 * as Basic with the first request (retried once with Digest if the
 * server asks for it). Over plain http no login is ever sent, Basic or
 * Digest: a 401 there is LOGIN_HTTP (err says to use https), never
 * NEED_LOGIN. Certificates: see url_fetch_to_cache_ex. On NEED_LOGIN,
 * challenge holds the WWW-Authenticate value (may be empty). cb, out buffers and err may
 * be NULL / zero length. cache_dir is created if missing.
 * Every request sends an Accept header that also takes JSON. An
 * application/json answer that is a Caddy folder listing is a game
 * list: CATALOG, nothing cached, *catalog_or_null filled (entries
 * resolved against the URL after redirects; free it with
 * url_catalog_free), or with NULL err says to open it from the menu.
 * catalog_or_null is zeroed first, so freeing it is always safe. Other
 * JSON is a file as before. A list is never answered from the cache.
 * The body is written to the .part file as it arrives (no whole-body
 * buffer; 2 GiB cap). Every finished download and every cache hit is
 * checked with url_safety (not-a-game + .zip/.7z members): a refused
 * file is deleted and BLOCKED_* returned with the reason in err. A
 * new download is marked as from the internet (Windows). */
enum url_fetch_result url_fetch_to_cache(const char *url, const char *cache_dir,
      const char *user_or_null, const char *pass_or_null,
      char *out_path, size_t out_len, char *challenge, size_t ch_len,
      char *err, size_t err_len, url_fetch_progress_t cb, void *ud,
      struct url_catalog *catalog_or_null);

/* url_fetch_to_cache with the certificate rules spelled out (the plain
 * call is download_anyway = false):
 * - Every https request needs a certificate that verifies against the
 *   system CAs. A request carrying a login or archive.org keys that
 *   fails it is never sent: FAILED, the login was not sent.
 * - Without a login, a failed check is CERT_INVALID (nothing sent or
 *   kept, err says so): the caller may ask the user and call again with
 *   download_anyway = true, which skips the check for this download's
 *   requests that carry no login (never for a login or keys).
 * - A link ending in #sha256=<64 hex> is downloaded without the check
 *   (no login) but the file (or game list) must hash to it: otherwise
 *   it is deleted and BLOCKED_FINGERPRINT. Cache hits for such a link
 *   are hashed again every time. A broken #sha256= fails the link
 *   (FAILED). The fragment is never sent and the cache name is that of
 *   the URL without it, so files cached before stay valid.
 * login_site_out (may be NULL) gets the https site a login (typed or
 * saved) was sent to on an OK or CATALOG answer, empty otherwise: the
 * site to save a typed login under. */
enum url_fetch_result url_fetch_to_cache_ex(const char *url,
      const char *cache_dir,
      const char *user_or_null, const char *pass_or_null,
      char *out_path, size_t out_len, char *challenge, size_t ch_len,
      char *err, size_t err_len, url_fetch_progress_t cb, void *ud,
      struct url_catalog *catalog_or_null, bool download_anyway,
      char *login_site_out, size_t ls_len);

/* Deletes the downloaded files in cache_dir: plain files named
 * <16 hex>_..., nothing else (no sub-folders, no other files). Refuses
 * (false) unless the folder is named url_cache. A missing folder is
 * already clear (true). removed (may be NULL) gets the count. */
bool url_fetch_clear_cache(const char *cache_dir, unsigned *removed);

/* Test hook: treat only this host as archive.org (for the keys).
 * NULL restores the real archive.org check. */
void url_fetch_set_test_archive_host(const char *host);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
