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

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "url_servers.h"

#define URL_SERVERS_URL_LEN 2048

static char   url_servers_path[4096] = {0};
static char   url_servers_list[URL_SERVERS_MAX][URL_SERVERS_URL_LEN];
static size_t url_servers_n = 0;
/* Candidate list built by add/remove; copied over the real one only
 * once the file is saved. Static: too big for some stacks. */
static char   url_servers_next[URL_SERVERS_MAX][URL_SERVERS_URL_LEN];

/* Length of "scheme://" at the start of url, or 0 if there is none. */
static size_t url_servers_scheme_len(const char *url)
{
   size_t i = 0;
   char c   = url[0];
   if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
      return 0;
   for (i = 1; url[i]; i++)
   {
      c = url[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
         break;
   }
   if (url[i] != ':' || url[i + 1] != '/' || url[i + 2] != '/')
      return 0;
   return i + 3;
}

/* Finds the host[:port] part of url, after any "user:pass@".
 * False if url is not scheme://host. */
static bool url_servers_split(const char *url, size_t *scheme_len,
      const char **host, const char **host_end)
{
   const char *auth;
   const char *end;
   const char *p;
   size_t      n;

   if (!url || !(n = url_servers_scheme_len(url)))
      return false;
   auth = url + n;
   end  = auth + strcspn(auth, "/?#");
   for (p = auth; p < end; p++)
      if (*p == '@')
         auth = p + 1;
   if (auth >= end)
      return false;
   *scheme_len = n;
   *host       = auth;
   *host_end   = end;
   return true;
}

/* url with any login removed. False if it is not a URL, holds a
 * character the line format cannot store, or does not fit. */
static bool url_servers_clean(char *out, size_t len, const char *url)
{
   size_t      scheme_len;
   const char *host;
   const char *host_end;
   const char *p;

   if (!url_servers_split(url, &scheme_len, &host, &host_end))
      return false;
   for (p = url; *p; p++)
      if ((unsigned char)*p < 0x20 || *p == 0x7f)
         return false;
   if (scheme_len + strlen(host) >= len)
      return false;
   memcpy(out, url, scheme_len);
   strlcpy(out + scheme_len, host, len - scheme_len);
   return true;
}

void url_servers_root(char *out, size_t len, const char *catalog_url)
{
   size_t      scheme_len;
   size_t      host_len;
   size_t      i;
   const char *host;
   const char *host_end;

   if (!len)
      return;
   out[0] = '\0';
   if (!url_servers_split(catalog_url, &scheme_len, &host, &host_end))
      return;
   host_len = (size_t)(host_end - host);
   /* scheme:// + host[:port] + '/' + NUL */
   if (scheme_len + host_len + 2 > len)
      return;
   memcpy(out, catalog_url, scheme_len);
   memcpy(out + scheme_len, host, host_len);
   out[scheme_len + host_len]     = '/';
   out[scheme_len + host_len + 1] = '\0';
   /* Lower-case scheme and host, up to the port (or past an IPv6 ']'). */
   for (i = 0; out[i]; i++)
   {
      char c = out[i];
      if (i >= scheme_len && (c == ':' || c == ']' || c == '/'))
         break;
      if (c >= 'A' && c <= 'Z')
         out[i] = (char)(c - 'A' + 'a');
   }
}

static void url_servers_load(void)
{
   void    *buf = NULL;
   int64_t  len = 0;
   char    *line;
   char    *next;

   url_servers_n = 0;
   if (string_is_empty(url_servers_path) || !path_is_valid(url_servers_path))
      return;
   if (!filestream_read_file(url_servers_path, &buf, &len) || !buf)
      return;

   for (line = (char*)buf; line && url_servers_n < URL_SERVERS_MAX;
         line = next)
   {
      char   clean[URL_SERVERS_URL_LEN];
      size_t i;
      size_t n;
      bool   dup = false;

      if ((next = strchr(line, '\n')))
         *next++ = '\0';
      n = strlen(line);
      if (n && line[n - 1] == '\r')
         line[n - 1] = '\0';
      if (!url_servers_clean(clean, sizeof(clean), line))
         continue;
      for (i = 0; i < url_servers_n; i++)
         if (string_is_equal(url_servers_list[i], clean))
            dup = true;
      if (!dup)
         strlcpy(url_servers_list[url_servers_n++], clean,
               URL_SERVERS_URL_LEN);
   }
   free(buf);
}

void url_servers_set_path(const char *path)
{
   if (path)
      strlcpy(url_servers_path, path, sizeof(url_servers_path));
   else
      url_servers_path[0] = '\0';
   url_servers_load();
}

size_t url_servers_count(void)
{
   return url_servers_n;
}

bool url_servers_get(size_t i, char *out, size_t len)
{
   if (i >= url_servers_n || !out || !len)
      return false;
   return strlcpy(out, url_servers_list[i], len) < len;
}

/* Writes the n entries of url_servers_next to <path>.tmp, renames it
 * over the file, then makes them the live list. On failure the file
 * and the live list stay as they were. */
static bool url_servers_commit(size_t n)
{
   char    tmp[sizeof(url_servers_path) + 8];
   char   *data;
   size_t  size = 0;
   size_t  i;
   bool    ok;

   if (string_is_empty(url_servers_path))
      return false;
   if (     strlcpy(tmp, url_servers_path, sizeof(tmp)) >= sizeof(tmp)
         || strlcat(tmp, ".tmp", sizeof(tmp)) >= sizeof(tmp))
      return false;

   if (!(data = (char*)malloc(n * URL_SERVERS_URL_LEN + 1)))
      return false;
   for (i = 0; i < n; i++)
   {
      size_t l = strlen(url_servers_next[i]);
      memcpy(data + size, url_servers_next[i], l);
      size += l;
      data[size++] = '\n';
   }

   if (path_is_valid(tmp))
      filestream_delete(tmp);
   ok = filestream_write_file(tmp, data, (int64_t)size)
      && path_is_valid(tmp);
   free(data);
   if (!ok)
   {
      filestream_delete(tmp);
      return false;
   }
   if (filestream_rename(tmp, url_servers_path) != 0)
   {
      /* Windows cannot rename over an existing file. */
      filestream_delete(url_servers_path);
      if (filestream_rename(tmp, url_servers_path) != 0)
      {
         filestream_delete(tmp);
         return false;
      }
   }

   for (i = 0; i < n; i++)
      strlcpy(url_servers_list[i], url_servers_next[i], URL_SERVERS_URL_LEN);
   url_servers_n = n;
   return true;
}

bool url_servers_add(const char *root_url)
{
   char   clean[URL_SERVERS_URL_LEN];
   size_t i;
   size_t n = 1;

   if (string_is_empty(url_servers_path)
         || !url_servers_clean(clean, sizeof(clean), root_url))
      return false;

   strlcpy(url_servers_next[0], clean, URL_SERVERS_URL_LEN);
   for (i = 0; i < url_servers_n && n < URL_SERVERS_MAX; i++)
      if (!string_is_equal(url_servers_list[i], clean))
         strlcpy(url_servers_next[n++], url_servers_list[i],
               URL_SERVERS_URL_LEN);
   return url_servers_commit(n);
}

bool url_servers_remove(size_t i)
{
   size_t j;
   size_t n = 0;

   if (i >= url_servers_n)
      return false;
   for (j = 0; j < url_servers_n; j++)
      if (j != i)
         strlcpy(url_servers_next[n++], url_servers_list[j],
               URL_SERVERS_URL_LEN);
   return url_servers_commit(n);
}

#endif /* HAVE_NETWORKING */
