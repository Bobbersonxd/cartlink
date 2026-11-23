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

/* <path><suffix> into out. False if it does not fit. */
static bool url_logins_side(char *out, size_t len, const char *suffix)
{
   return strlcpy(out, url_logins_path, len) < len
      && strlcat(out, suffix, len) < len;
}

/* Moves tmp over the file. Windows cannot rename over an existing
 * file, so there the old file is first moved aside to .bak and put
 * back if the second rename fails (e.g. antivirus holding the new
 * file). If even that fails, .tmp (and .bak) stay on disk and
 * url_logins_open picks them up: saved logins are never lost. */
static bool url_logins_replace(const char *tmp, const char *bak)
{
   bool moved = false;

   if (filestream_rename(tmp, url_logins_path) != 0)
   {
      if (path_is_valid(url_logins_path))
      {
         if (path_is_valid(bak))
            filestream_delete(bak);
         if (filestream_rename(url_logins_path, bak) != 0)
         {
            /* Old file untouched. */
            filestream_delete(tmp);
            return false;
         }
         moved = true;
      }
      if (filestream_rename(tmp, url_logins_path) != 0)
      {
         if (moved && filestream_rename(bak, url_logins_path) == 0)
            filestream_delete(tmp);
         return false;
      }
   }
   if (path_is_valid(bak))
      filestream_delete(bak);
   return true;
}

/* Existing file, or an empty config if there is none yet. NULL if the
 * file exists but cannot be read (never clobber it). */
static config_file_t *url_logins_open(bool create)
{
   char        tmp[sizeof(url_logins_path) + 8];
   char        bak[sizeof(url_logins_path) + 8];
   const char *src = url_logins_path;

   if (     string_is_empty(url_logins_path)
         || !url_logins_side(tmp, sizeof(tmp), ".tmp")
         || !url_logins_side(bak, sizeof(bak), ".bak"))
      return NULL;
   /* A failed Windows replace can leave only the new logins (.tmp) or
    * the old ones (.bak). Use them and put them back in place. */
   if (!path_is_valid(src))
   {
      if (path_is_valid(tmp))
         src = tmp;
      else if (path_is_valid(bak))
         src = bak;
      else
         return create ? config_file_new_alloc() : NULL;
      if (filestream_rename(src, url_logins_path) == 0)
         src = url_logins_path;
   }
   return config_file_new_from_path_to_string(src);
}

/* Owner-only file mode (POSIX). True elsewhere. */
static bool url_logins_tighten(const char *path, bool create)
{
#ifdef URL_LOGINS_POSIX_MODE
   int fd = open(path, O_WRONLY | (create ? (O_CREAT | O_TRUNC) : 0), 0600);
   if (fd < 0)
      return false;
   close(fd);
   if (chmod(path, 0600) != 0)
      return false;
#endif
   return true;
}

/* Writes <path>.tmp (0600 on POSIX, before any secret is in it), then
 * renames it over the old file, so a crash or a full disk never
 * leaves a half-written logins file. */
static bool url_logins_write(config_file_t *conf)
{
   char tmp[sizeof(url_logins_path) + 8];
   char bak[sizeof(url_logins_path) + 8];

   if (!(conf->flags & CONF_FILE_FLG_MODIFIED))
      return url_logins_tighten(url_logins_path, false);

   if (     !url_logins_side(tmp, sizeof(tmp), ".tmp")
         || !url_logins_side(bak, sizeof(bak), ".bak"))
      return false;
   if (path_is_valid(tmp))
      filestream_delete(tmp);
   if (     !url_logins_tighten(tmp, true)
         || !config_file_write(conf, tmp, true)
         || !path_is_valid(tmp))
   {
      filestream_delete(tmp);
      return false;
   }
   return url_logins_replace(tmp, bak);
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
   char tmp[sizeof(url_logins_path) + 8];
   char bak[sizeof(url_logins_path) + 8];
   bool side_ok;
   if (string_is_empty(url_logins_path))
      return false;
   /* A .tmp left by a crash mid-write, or a .bak left by a failed
    * Windows replace, holds secrets too. */
   side_ok = url_logins_side(tmp, sizeof(tmp), ".tmp")
          && url_logins_side(bak, sizeof(bak), ".bak");
   if (side_ok && path_is_valid(tmp))
      filestream_delete(tmp);
   if (side_ok && path_is_valid(bak))
      filestream_delete(bak);
   if (path_is_valid(url_logins_path))
      filestream_delete(url_logins_path);
   return !path_is_valid(url_logins_path)
      && (!side_ok || (!path_is_valid(tmp) && !path_is_valid(bak)));
}

#endif /* HAVE_NETWORKING */
