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
   URL_FETCH_CATALOG        /* a Caddy folder listing (game list) */
};

typedef void (*url_fetch_progress_t)(size_t done, size_t total, void *ud);

/* Blocking. Fixes the link, returns CACHED if a cache file for it
 * exists, else downloads it. Auth: user/pass if user is non-empty,
 * else saved logins for the host, else archive.org keys (https only)
 * for hosts ending in archive.org. Over https the login goes as Basic
 * with the first request (retried once with Digest if the server asks
 * for it); over plain http nothing is sent until the server answers
 * 401, then its challenge (Digest preferred, else Basic) is answered
 * once. Any request carrying a login or keys over https needs a
 * certificate that verifies against the system CAs; otherwise it is
 * not sent and FAILED explains why. On NEED_LOGIN, challenge holds the
 * WWW-Authenticate value (may be empty). cb, out buffers and err may
 * be NULL / zero length. cache_dir is created if missing.
 * Every request sends an Accept header that also takes JSON. An
 * application/json answer that is a Caddy folder listing is a game
 * list: CATALOG, nothing cached, *catalog_or_null filled (entries
 * resolved against the URL after redirects; free it with
 * url_catalog_free), or with NULL err says to open it from the menu.
 * catalog_or_null is zeroed first, so freeing it is always safe. Other
 * JSON is a file as before. A list is never answered from the cache. */
enum url_fetch_result url_fetch_to_cache(const char *url, const char *cache_dir,
      const char *user_or_null, const char *pass_or_null,
      char *out_path, size_t out_len, char *challenge, size_t ch_len,
      char *err, size_t err_len, url_fetch_progress_t cb, void *ud,
      struct url_catalog *catalog_or_null);

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
