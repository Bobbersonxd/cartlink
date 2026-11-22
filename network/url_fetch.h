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
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum url_fetch_result
{
   URL_FETCH_OK = 0,        /* downloaded, out_path set */
   URL_FETCH_CACHED,        /* already in cache, out_path set */
   URL_FETCH_NEED_LOGIN,    /* 401, challenge copied out */
   URL_FETCH_NOT_A_FILE,    /* got an HTML page */
   URL_FETCH_TOO_BIG,       /* allocation failed */
   URL_FETCH_FAILED         /* network / HTTP error, msg in err */
};

typedef void (*url_fetch_progress_t)(size_t done, size_t total, void *ud);

/* Blocking. Fixes the link, returns CACHED if a cache file for it
 * exists, else downloads it. Auth: user/pass if user is non-empty,
 * else saved logins for the host, else archive.org keys for hosts
 * ending in archive.org. A Basic try answered by a Digest challenge is
 * retried once with Digest. On NEED_LOGIN, challenge holds the
 * WWW-Authenticate value (may be empty). cb, out buffers and err may
 * be NULL / zero length. cache_dir is created if missing. */
enum url_fetch_result url_fetch_to_cache(const char *url, const char *cache_dir,
      const char *user_or_null, const char *pass_or_null,
      char *out_path, size_t out_len, char *challenge, size_t ch_len,
      char *err, size_t err_len, url_fetch_progress_t cb, void *ud);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
