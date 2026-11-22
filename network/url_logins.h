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

/* "Load from URL" saved logins: a small config_file kept apart from
 * retroarch.cfg. Keys: url_login_<host>_user, url_login_<host>_pass,
 * url_login_archive_access, url_login_archive_secret; in <host>, any
 * character other than [a-z0-9-] (dots included) becomes '_'. */

#ifndef __RARCH_URL_LOGINS_H
#define __RARCH_URL_LOGINS_H

#ifdef HAVE_NETWORKING

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Sets the logins file path (copied). NULL or "" disables every call. */
void url_logins_set_path(const char *path);

/* False if the file, the host or either key is missing. */
bool url_logins_get(const char *host, char *user, size_t ulen,
      char *pass, size_t plen);

/* Adds or replaces the login for host and writes the file (mode 0600
 * on POSIX) through <path>.tmp and a rename, so the old file stays
 * whole if writing fails. Refuses values containing '"', CR or LF, which the file
 * format cannot store. */
bool url_logins_set(const char *host, const char *user, const char *pass);

bool url_logins_get_archive_keys(char *access, size_t alen,
      char *secret, size_t slen);
bool url_logins_set_archive_keys(const char *access, const char *secret);

/* Deletes the file (and a leftover <path>.tmp). True if it is gone
 * afterwards. */
bool url_logins_forget_all(void);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
