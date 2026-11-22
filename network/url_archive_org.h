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

/* "Load from URL": archive.org sign-in. Trades an email and password
 * for the account's S3 keys (xauthn) and saves only the keys. */

#ifndef __RARCH_URL_ARCHIVE_ORG_H
#define __RARCH_URL_ARCHIVE_ORG_H

#ifdef HAVE_NETWORKING

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Blocking. POSTs email and password to archive.org's xauthn login and,
 * on "success": true, copies values.s3.access / values.s3.secret out
 * and saves them with url_logins_set_archive_keys. The password is only
 * held in a stack buffer here (wiped before returning) and is never
 * stored or logged. Redirects are not followed. Over https the
 * password is only sent if archive.org's certificate verifies against
 * the system CAs. The JSON reply is read whatever the HTTP status, so
 * a refused login gives its reason. On failure, err gets a short
 * reason. */
bool url_archive_org_login(const char *email, const char *password,
      char *access, size_t alen, char *secret, size_t slen,
      char *err, size_t elen);

/* Test hook: sends the login to url instead. NULL restores archive.org. */
void url_archive_org_set_test_url(const char *url);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
