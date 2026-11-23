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
 * retroarch.cfg. Keys: url_login_<site>_user, url_login_<site>_pass
 * with <site> = "https://host[:port]" (url_content_origin: default
 * port left out), url_login_archive_access, url_login_archive_secret.
 * Logins are only for https sites (they are never sent over http).
 * Older builds keyed by bare host, url_login_<host>_user with any
 * character other than [a-z0-9-] as '_': such an entry is read as
 * https://<host> on the default port only, and replaced by the new key
 * when that site is saved or migrated.
 * POSIX: the file is mode 0600, values in plain text. Windows: every
 * value is sealed with DPAPI (CryptProtectData, current user) and the
 * file starts with "# cartlink-logins v2 dpapi"; a plain file from an
 * older build is read as is and sealed on the next save. A sealed file
 * is unreadable (and left alone) on other systems. */

#ifndef __RARCH_URL_LOGINS_H
#define __RARCH_URL_LOGINS_H

#ifdef HAVE_NETWORKING

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Sets the logins file path (copied). NULL or "" disables every call. */
void url_logins_set_path(const char *path);

/* Login for the https site of url (any URL of it, or the site itself).
 * False if the file or either key is missing, or url is not https. */
bool url_logins_get(const char *url, char *user, size_t ulen,
      char *pass, size_t plen);

/* Adds or replaces the login for the https site of url (an older
 * entry for it is removed) and writes the file through <path>.tmp and
 * a rename, so the old file stays whole if writing fails. Refuses
 * plain http and other schemes, and values containing '"', CR or LF,
 * which the file format cannot store. */
bool url_logins_set(const char *url, const char *user, const char *pass);

/* Moves an older build's entry for the https site of url (default
 * port only) to the new key. True if there was nothing to move or it
 * was moved; false if url is not https or writing failed. */
bool url_logins_migrate(const char *url);

bool url_logins_get_archive_keys(char *access, size_t alen,
      char *secret, size_t slen);
bool url_logins_set_archive_keys(const char *access, const char *secret);

/* Deletes the file (and a leftover <path>.tmp). True if it is gone
 * afterwards. */
bool url_logins_forget_all(void);

/* True for the files that hold logins or saved servers (url_logins.cfg,
 * url_servers.cfg and their .tmp/.bak), by base name, any case: Cloud
 * Sync never uploads them. name may carry a folder ("config/..."). */
bool url_logins_is_private_file(const char *name);

/* True if s is well-formed base64 (length a multiple of 4, only
 * A-Z a-z 0-9 + /, '=' only as 1-2 trailing padding): the only input
 * a sealed (Windows) value is decoded from. */
bool url_logins_base64_ok(const char *s);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
