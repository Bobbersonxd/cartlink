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
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <retro_dirent.h>
#include <retro_miscellaneous.h>
#include <retro_timers.h>
#include <features/features_cpu.h>
#include <lists/string_list.h>
#include <net/net_http.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "url_content.h"
#include "url_logins.h"
#include "url_fetch.h"

#define UF_URL_LEN      4096
#define UF_HDR_LEN      2048
#define UF_PREFIX_LEN   17         /* 16 hex + '_' */
#define UF_STALL_USEC   30000000   /* give up after 30 s without data */

enum uf_auth
{
   UF_AUTH_NONE = 0,
   UF_AUTH_BASIC,
   UF_AUTH_LOW
};

struct uf_response
{
   uint8_t *data;
   size_t len;
   size_t total;               /* Content-Length seen by net_http, 0 if unknown */
   struct string_list *headers;
   int status;
   bool err;
};

static void uf_set_err(char *err, size_t err_len, const char *msg)
{
   if (err && err_len)
      strlcpy(err, msg, err_len);
}

static void uf_response_free(struct uf_response *r)
{
   if (r->data)
      free(r->data);
   if (r->headers)
      string_list_free(r->headers);
   r->data    = NULL;
   r->headers = NULL;
}

/* Value of the first header named name (case-insensitive), or NULL. */
static const char *uf_header(const struct string_list *headers,
      const char *name)
{
   size_t i;
   size_t nlen = strlen(name);
   if (!headers)
      return NULL;
   for (i = 0; i < headers->size; i++)
   {
      const char *line = headers->elems[i].data;
      if (     line
            && string_starts_with_case_insensitive(line, name)
            && line[nlen] == ':')
      {
         line += nlen + 1;
         while (*line == ' ' || *line == '\t')
            line++;
         return line;
      }
   }
   return NULL;
}

/* First WWW-Authenticate value starting with scheme_or_null
 * (any scheme if NULL). */
static const char *uf_challenge(const struct string_list *headers,
      const char *scheme_or_null)
{
   size_t i;
   const size_t nlen = STRLEN_CONST("WWW-Authenticate");
   if (!headers)
      return NULL;
   for (i = 0; i < headers->size; i++)
   {
      const char *line = headers->elems[i].data;
      if (     !line
            || !string_starts_with_case_insensitive(line, "WWW-Authenticate")
            || line[nlen] != ':')
         continue;
      line += nlen + 1;
      while (*line == ' ' || *line == '\t')
         line++;
      if (     !scheme_or_null
            || string_starts_with_case_insensitive(line, scheme_or_null))
         return line;
   }
   return NULL;
}

/* Request URI (path + query, no fragment) of an absolute URL. */
static void uf_request_uri(char *out, size_t len, const char *url)
{
   const char *p = strstr(url, "://");
   size_t n;
   p = p ? p + 3 : url;
   while (*p && *p != '/' && *p != '?' && *p != '#')
      p++;
   if (*p != '/')
   {
      strlcpy(out, "/", len);
      if (*p == '?')
         strlcat(out, p, len);
   }
   else
      strlcpy(out, p, len);
   n = strcspn(out, "#");
   out[n] = '\0';
}

/* True if the decimal Content-Length value equals len (C89: no
 * strtoull, and a value too big for size_t cannot match). */
static bool uf_length_matches(const char *value, size_t len)
{
   size_t n = 0;
   if (*value < '0' || *value > '9')
      return false;
   for (; *value >= '0' && *value <= '9'; value++)
   {
      size_t d = (size_t)(*value - '0');
      if (n > ((size_t)-1 - d) / 10)
         return false;
      n = n * 10 + d;
   }
   return n == len;
}

static bool uf_host_is_archive(const char *host)
{
   size_t hl = strlen(host);
   size_t al = STRLEN_CONST("archive.org");
   if (hl < al || !string_is_equal_case_insensitive(host + hl - al,
            "archive.org"))
      return false;
   return hl == al || host[hl - al - 1] == '.';
}

/* Blocking GET. Fills r; returns false only if no request could be
 * made at all (bad URL / out of memory). */
static bool uf_get(const char *url, const char *headers_or_null,
      struct uf_response *r, url_fetch_progress_t cb, void *ud)
{
   struct http_connection_t *conn;
   struct http_t *http;
   retro_time_t last_change;
   size_t last_done = (size_t)-1;

   memset(r, 0, sizeof(*r));
   r->status = -1;

   if (!(conn = net_http_connection_new(url, "GET", NULL)))
      return false;
   if (     !net_http_connection_iterate(conn)
         || !net_http_connection_done(conn))
   {
      net_http_connection_free(conn);
      return false;
   }
   if (headers_or_null && *headers_or_null)
      net_http_connection_set_headers(conn, headers_or_null);
   net_http_connection_set_user_agent(conn, "RetroArch");

   http = net_http_new(conn);
   net_http_connection_free(conn);
   if (!http)
      return false;

   last_change = cpu_features_get_time_usec();
   for (;;)
   {
      size_t done  = 0;
      size_t total = 0;
      bool finished = net_http_update(http, &done, &total);

      if (total)
         r->total = total;
      if (cb)
         cb(done, total, ud);
      if (finished)
         break;

      if (done != last_done)
      {
         last_done   = done;
         last_change = cpu_features_get_time_usec();
      }
      else
      {
         if (cpu_features_get_time_usec() - last_change > UF_STALL_USEC)
         {
            r->err = true;
            break;
         }
         retro_sleep(1);
      }
   }

   r->status  = net_http_status(http);
   r->err     = r->err || r->status < 0;
   r->data    = net_http_data(http, &r->len, true);
   r->headers = net_http_response_headers(http);
   /* net_http_delete frees neither the body nor the header list. */
   net_http_delete(http);
   return true;
}

/* Scans cache_dir for <prefix>*: deletes leftover .part files and
 * copies the first finished file name into found (empty if none). */
static void uf_scan_cache(const char *cache_dir, const char *prefix,
      bool delete_parts, char *found, size_t found_len)
{
   struct RDIR *dir;
   found[0] = '\0';
   if (!(dir = retro_opendir_include_hidden(cache_dir, true)))
      return;
   while (retro_readdir(dir))
   {
      const char *name = retro_dirent_get_name(dir);
      if (     !name
            || strncmp(name, prefix, UF_PREFIX_LEN) != 0
            || retro_dirent_is_dir(dir, NULL))
         continue;
      if (string_ends_with(name, ".part"))
      {
         if (delete_parts)
         {
            char path[PATH_MAX_LENGTH];
            fill_pathname_join_special(path, cache_dir, name, sizeof(path));
            filestream_delete(path);
         }
      }
      else if (!found[0])
         strlcpy(found, name, found_len);
   }
   retro_closedir(dir);
}

enum url_fetch_result url_fetch_to_cache(const char *url, const char *cache_dir,
      const char *user_or_null, const char *pass_or_null,
      char *out_path, size_t out_len, char *challenge, size_t ch_len,
      char *err, size_t err_len, url_fetch_progress_t cb, void *ud)
{
   char fixed[UF_URL_LEN];
   char host[256];
   char prefix[PATH_MAX_LENGTH];
   char found[PATH_MAX_LENGTH];
   char user[256];
   char pass[256];
   char auth[UF_HDR_LEN];
   char name[PATH_MAX_LENGTH];
   char final_path[PATH_MAX_LENGTH];
   char part_path[PATH_MAX_LENGTH];
   const char *disp;
   const char *clen;
   struct uf_response r;
   enum uf_auth auth_kind = UF_AUTH_NONE;

   if (out_path && out_len)
      out_path[0] = '\0';
   if (challenge && ch_len)
      challenge[0] = '\0';
   if (err && err_len)
      err[0] = '\0';

   if (string_is_empty(url) || string_is_empty(cache_dir))
   {
      uf_set_err(err, err_len, "No URL or cache folder.");
      return URL_FETCH_FAILED;
   }

   url_content_fix_link(fixed, sizeof(fixed), url);
   url_content_host(host, sizeof(host), fixed);

   /* The hash part of the cache name is the same for any file name. */
   url_content_cache_name(prefix, sizeof(prefix), fixed, "x");
   prefix[UF_PREFIX_LEN] = '\0';

   if (!path_is_directory(cache_dir) && !path_mkdir(cache_dir))
   {
      uf_set_err(err, err_len, "Cannot create the cache folder.");
      return URL_FETCH_FAILED;
   }

   uf_scan_cache(cache_dir, prefix, true, found, sizeof(found));
   if (found[0])
   {
      if (out_path && out_len)
         fill_pathname_join_special(out_path, cache_dir, found, out_len);
      return URL_FETCH_CACHED;
   }

   /* Pick the credentials for the first try. */
   auth[0] = '\0';
   user[0] = '\0';
   pass[0] = '\0';
   if (!string_is_empty(user_or_null))
   {
      strlcpy(user, user_or_null, sizeof(user));
      if (pass_or_null)
         strlcpy(pass, pass_or_null, sizeof(pass));
      auth_kind = UF_AUTH_BASIC;
   }
   else if (url_logins_get(host, user, sizeof(user), pass, sizeof(pass)))
      auth_kind = UF_AUTH_BASIC;
   else if (uf_host_is_archive(host))
   {
      char access[256];
      char secret[256];
      if (url_logins_get_archive_keys(access, sizeof(access),
               secret, sizeof(secret)))
      {
         snprintf(auth, sizeof(auth), "Authorization: LOW %s:%s\r\n",
               access, secret);
         auth_kind = UF_AUTH_LOW;
      }
   }
   if (auth_kind == UF_AUTH_BASIC)
      url_content_basic_auth_header(auth, sizeof(auth), user, pass);

   if (!uf_get(fixed, auth, &r, cb, ud))
   {
      uf_set_err(err, err_len, "Bad URL.");
      return URL_FETCH_FAILED;
   }

   if (r.status == 401 && auth_kind == UF_AUTH_BASIC)
   {
      const char *digest = uf_challenge(r.headers, "Digest");
      char uri[UF_URL_LEN];
      uf_request_uri(uri, sizeof(uri), fixed);
      if (digest && url_content_digest_auth_header(auth, sizeof(auth),
               digest, "GET", uri, user, pass))
      {
         uf_response_free(&r);
         if (!uf_get(fixed, auth, &r, cb, ud))
         {
            uf_set_err(err, err_len, "Bad URL.");
            return URL_FETCH_FAILED;
         }
      }
   }

   if (r.status == 401)
   {
      const char *ch = uf_challenge(r.headers, "Digest");
      if (!ch)
         ch = uf_challenge(r.headers, NULL);
      if (ch && challenge && ch_len)
         strlcpy(challenge, ch, ch_len);
      uf_set_err(err, err_len, "Login needed.");
      uf_response_free(&r);
      return URL_FETCH_NEED_LOGIN;
   }

   if (r.err)
   {
      /* net_http loses the buffer when growing it fails. */
      bool oom = !r.data && r.total > 0;
      uf_set_err(err, err_len, oom ? "Out of memory." : "Download failed.");
      uf_response_free(&r);
      return oom ? URL_FETCH_TOO_BIG : URL_FETCH_FAILED;
   }

   if (r.status < 200 || r.status > 299)
   {
      char msg[64];
      snprintf(msg, sizeof(msg), "HTTP error %d.", r.status);
      uf_set_err(err, err_len, msg);
      uf_response_free(&r);
      return URL_FETCH_FAILED;
   }

   if (!r.data && r.len > 0)
   {
      uf_set_err(err, err_len, "Out of memory.");
      uf_response_free(&r);
      return URL_FETCH_TOO_BIG;
   }

   clen = uf_header(r.headers, "Content-Length");
   if (clen && !uf_length_matches(clen, r.len))
   {
      uf_set_err(err, err_len, "Download was cut short.");
      uf_response_free(&r);
      return URL_FETCH_FAILED;
   }

   if (url_content_looks_like_html(r.data, r.len))
   {
      uf_set_err(err, err_len, "Got a web page, not a file.");
      uf_response_free(&r);
      return URL_FETCH_NOT_A_FILE;
   }

   name[0] = '\0';
   disp    = uf_header(r.headers, "Content-Disposition");
   if (disp)
      url_content_name_from_disposition(name, sizeof(name), disp);
   url_content_cache_name(found, sizeof(found), fixed,
         name[0] ? name : NULL);

   fill_pathname_join_special(final_path, cache_dir, found,
         sizeof(final_path));
   strlcpy(part_path, final_path, sizeof(part_path));
   strlcat(part_path, ".part", sizeof(part_path));

   if (     !filestream_write_file(part_path, r.data, (int64_t)r.len)
         || filestream_rename(part_path, final_path) != 0)
   {
      filestream_delete(part_path);
      uf_set_err(err, err_len, "Cannot write the cache file.");
      uf_response_free(&r);
      return URL_FETCH_FAILED;
   }

   uf_response_free(&r);
   if (out_path && out_len)
      strlcpy(out_path, final_path, out_len);
   return URL_FETCH_OK;
}

#endif /* HAVE_NETWORKING */
