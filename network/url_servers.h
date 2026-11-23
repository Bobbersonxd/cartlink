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

/* "Load from URL" Saved Servers: url_servers.cfg next to retroarch.cfg,
 * one URL per line, newest first. URLs only: any "user:pass@" is
 * stripped before a URL is kept (logins live in url_logins.cfg). */

#ifndef __RARCH_URL_SERVERS_H
#define __RARCH_URL_SERVERS_H

#ifdef HAVE_NETWORKING

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define URL_SERVERS_MAX 64

/* Sets the file path (copied) and loads the list from it. NULL or ""
 * empties the list and disables add/remove. */
void   url_servers_set_path(const char *path);

size_t url_servers_count(void);

/* Entry i (0 = newest). False if i is out of range or out is too small. */
bool   url_servers_get(size_t i, char *out, size_t len);

/* Puts root_url (login stripped) at the top and saves the file through
 * <path>.tmp and a rename. Already present: moved to the top. Over
 * URL_SERVERS_MAX entries: the oldest is dropped. False for a value
 * that is not a scheme://host URL, holds CR/LF, or cannot be saved;
 * the list is unchanged then. */
bool   url_servers_add(const char *root_url);

/* Removes entry i and saves. False if out of range or not saved. */
bool   url_servers_remove(size_t i);

/* "scheme://host[:port]/" of catalog_url, login dropped, scheme and
 * host lower-cased. "" if it is not a URL or does not fit. */
void   url_servers_root(char *out, size_t len, const char *catalog_url);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
