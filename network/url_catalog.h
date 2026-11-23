/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 Cartlink contributors
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

/* "Load from URL" catalogs: Caddy `file_server browse` JSON listings.
 * Pure parsing, no I/O. */

#ifndef __RARCH_URL_CATALOG_H
#define __RARCH_URL_CATALOG_H

#ifdef HAVE_NETWORKING

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define URL_CATALOG_MAX      4096
#define URL_CATALOG_NAME_LEN 256
#define URL_CATALOG_URL_LEN  2048

struct url_catalog_entry
{
   char name[URL_CATALOG_NAME_LEN];
   char url[URL_CATALOG_URL_LEN];
   bool is_dir;
   uint64_t size;
};

struct url_catalog
{
   struct url_catalog_entry *entries;
   size_t count;
   bool truncated;
   char base[URL_CATALOG_URL_LEN];
};

/* True if json is a Caddy browse listing: a JSON array whose every
 * element is an object with string "name", bool "is_dir" and string
 * "url" ("size" optional; other members ignored). Then out holds the
 * entries (heap; free with url_catalog_free), folders first, then
 * files, each sorted by name ignoring ASCII case, at most
 * URL_CATALOG_MAX (truncated set if more). Each url is resolved
 * against base_url and must keep its scheme, host and port and stay
 * strictly under its folder, otherwise the entry is skipped. Entries
 * with an empty name, "." / "..", '/' or control characters in the
 * name are skipped too; names are cut to fit on a UTF-8 boundary.
 * On false out is empty. */
bool url_catalog_parse(const char *json, size_t len, const char *base_url,
      struct url_catalog *out);

void url_catalog_free(struct url_catalog *c);

/* Parent folder URL of a catalog URL (query dropped): the folder of a
 * file, or one level up from a folder. False at the server root. */
bool url_catalog_parent(char *out, size_t len, const char *url);

/* Menu title for a catalog URL: host[:port] and path, without scheme,
 * login, query or fragment, percent-decoded. Decoded control
 * characters are dropped; a bad escape stays as written. Cut to fit
 * (on a UTF-8 boundary). "" if url is not scheme://host. */
void url_catalog_title(char *out, size_t len, const char *url);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
