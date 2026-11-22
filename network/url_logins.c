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

#ifdef HAVE_NETWORKING

#include <stdio.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) \
   || defined(__OpenBSD__) || defined(__NetBSD__)
#define URL_LOGINS_POSIX_MODE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <compat/strl.h>
#include <file/config_file.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "url_logins.h"

#define URL_LOGINS_KEY_LEN 320

static char url_logins_path[4096] = {0};

void url_logins_set_path(const char *path)
{
   if (path)
      strlcpy(url_logins_path, path, sizeof(url_logins_path));
   else
      url_logins_path[0] = '\0';
}

/* "url_login_<host>_<field>", host lower-cased and every character
 * other than [a-z0-9-] turned into '_'. */
static bool url_logins_host_key(char *out, size_t len,
      const char *host, const char *field)
{
   size_t i;
   size_t n;

   if (string_is_empty(host))
      return false;

   n = strlcpy(out, "url_login_", len);
   for (i = 0; host[i] && n + 1 < len; i++)
   {
      char c = host[i];
      if (c >= 'A' && c <= 'Z')
         c = (char)(c - 'A' + 'a');
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
         c = '_';
      out[n++] = c;
   }
   if (host[i])
      return false;
   out[n] = '\0';
   n += snprintf(out + n, len - n, "_%s", field);
   return n < len;
}

/* config_file stores values as "..." with no escaping. */
static bool url_logins_value_ok(const char *s)
{
   return s && !strpbrk(s, "\"\r\n");
}

/* Existing file, or an empty config if there is none yet. NULL if the
 * file exists but cannot be read (never clobber it). */
static config_file_t *url_logins_open(bool create)
{
   if (string_is_empty(url_logins_path))
      return NULL;
   if (path_is_valid(url_logins_path))
      return config_file_new_from_path_to_string(url_logins_path);
   return create ? config_file_new_alloc() : NULL;
}

static bool url_logins_write(config_file_t *conf)
{
#ifdef URL_LOGINS_POSIX_MODE
   /* Create (or tighten) the file as 0600 before any secret is
    * written; fopen("wb") then truncates it and keeps the mode. */
   int fd = open(url_logins_path, O_WRONLY | O_CREAT, 0600);
   if (fd < 0)
      return false;
   close(fd);
   if (chmod(url_logins_path, 0600) != 0)
      return false;
#endif
   return config_file_write(conf, url_logins_path, true);
}

static bool url_logins_get_pair(const char *key_a, char *a, size_t alen,
      const char *key_b, char *b, size_t blen)
{
   bool ok;
   config_file_t *conf = url_logins_open(false);
   if (!conf)
      return false;
   ok = config_get_array(conf, key_a, a, alen)
     && config_get_array(conf, key_b, b, blen);
   config_file_free(conf);
   return ok;
}

static bool url_logins_set_pair(const char *key_a, const char *a,
      const char *key_b, const char *b)
{
   bool ok;
   config_file_t *conf;
   if (!url_logins_value_ok(a) || !url_logins_value_ok(b))
      return false;
   if (!(conf = url_logins_open(true)))
      return false;
   config_set_string(conf, key_a, a);
   config_set_string(conf, key_b, b);
   ok = url_logins_write(conf);
   config_file_free(conf);
   return ok;
}

bool url_logins_get(const char *host, char *user, size_t ulen,
      char *pass, size_t plen)
{
   char key_user[URL_LOGINS_KEY_LEN];
   char key_pass[URL_LOGINS_KEY_LEN];
   if (  !url_logins_host_key(key_user, sizeof(key_user), host, "user")
      || !url_logins_host_key(key_pass, sizeof(key_pass), host, "pass"))
      return false;
   return url_logins_get_pair(key_user, user, ulen, key_pass, pass, plen);
}

bool url_logins_set(const char *host, const char *user, const char *pass)
{
   char key_user[URL_LOGINS_KEY_LEN];
   char key_pass[URL_LOGINS_KEY_LEN];
   if (  !url_logins_host_key(key_user, sizeof(key_user), host, "user")
      || !url_logins_host_key(key_pass, sizeof(key_pass), host, "pass"))
      return false;
   return url_logins_set_pair(key_user, user, key_pass, pass);
}

bool url_logins_get_archive_keys(char *access, size_t alen,
      char *secret, size_t slen)
{
   return url_logins_get_pair("url_login_archive_access", access, alen,
         "url_login_archive_secret", secret, slen);
}

bool url_logins_set_archive_keys(const char *access, const char *secret)
{
   return url_logins_set_pair("url_login_archive_access", access,
         "url_login_archive_secret", secret);
}

bool url_logins_forget_all(void)
{
   if (string_is_empty(url_logins_path))
      return false;
   if (path_is_valid(url_logins_path))
      filestream_delete(url_logins_path);
   return !path_is_valid(url_logins_path);
}

#endif /* HAVE_NETWORKING */
