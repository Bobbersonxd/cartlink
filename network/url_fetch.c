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
#define UF_MAX_HOPS     10
#define UF_MAX_BODY     ((size_t)2048 * 1024 * 1024)   /* 2 GiB */
/* Files first; Caddy answers a folder with its JSON listing. */
#define UF_ACCEPT       "Accept: */*, application/json;q=0.9\r\n"

enum uf_auth
{
   UF_AUTH_NONE = 0,
   UF_AUTH_BASIC,       /* https: Basic sent with the first request */
   UF_AUTH_ON_CHALLENGE,/* http: login held back until a 401 asks */
   UF_AUTH_LOW
};

struct uf_response
{
   uint8_t *data;
   size_t len;
   size_t total;               /* Content-Length seen by net_http, 0 if unknown */
   struct string_list *headers;
   int status;
   int tls_error;             /* enum net_http_tls_error */
   bool err;
   bool too_big;              /* over UF_MAX_BODY or out of memory */
};

static char uf_test_archive_host[256] = {0};

void url_fetch_set_test_archive_host(const char *host)
{
   if (host)
      strlcpy(uf_test_archive_host, host, sizeof(uf_test_archive_host));
   else
      uf_test_archive_host[0] = '\0';
}

static bool uf_host_is_archive(const char *host)
{
   if (uf_test_archive_host[0])
      return string_is_equal(host, uf_test_archive_host);
   return url_content_host_is_archive(host);
}

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

/* Blocking GET. Fills r; returns false only if no request could be
 * made at all (bad URL / out of memory). Requests that carry a login
 * or keys (non-empty auth) over https need a verified certificate:
 * without one nothing is sent and r->tls_error says why. The Accept
 * header is separate from auth so it never turns that check on. */
static bool uf_get(const char *url, const char *auth_or_null,
      bool accept_json, struct uf_response *r,
      url_fetch_progress_t cb, void *ud)
{
   bool verify = auth_or_null && *auth_or_null
      && string_starts_with_case_insensitive(url, "https://");
   char headers[UF_HDR_LEN + sizeof(UF_ACCEPT)];
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
   headers[0] = '\0';
   if (accept_json)
      strlcpy(headers, UF_ACCEPT, sizeof(headers));
   if (auth_or_null && *auth_or_null)
      strlcat(headers, auth_or_null, sizeof(headers));
   if (headers[0])
      net_http_connection_set_headers(conn, headers);
   url_content_wipe(headers, sizeof(headers));
   net_http_connection_set_user_agent(conn, "RetroArch");
   net_http_connection_set_follow_redirects(conn, false);
   net_http_connection_set_max_body(conn, UF_MAX_BODY);
   if (verify)
      net_http_connection_set_verify_tls(conn, true);

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

   r->too_big   = net_http_too_big(http);
   r->tls_error = net_http_tls_error(http);
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

/* Picks the Authorization header for one hop. Logins (explicit or
 * saved) go only to the original host: over https as Basic right
 * away, over plain http only once the server's 401 asks (and then as
 * Digest if it offers it). archive keys go only to archive.org hosts
 * over https. Nothing once an https hop led to plain http. */
static enum uf_auth uf_pick_auth(char *auth, size_t auth_len,
      const char *hop_url, const char *hop_host, const char *orig_host,
      bool downgraded, bool have_login, const char *user, const char *pass)
{
   bool https = string_starts_with_case_insensitive(hop_url, "https://");
   auth[0] = '\0';
   if (downgraded)
      return UF_AUTH_NONE;
   if (have_login && string_is_equal(hop_host, orig_host))
   {
      if (!https)
         return UF_AUTH_ON_CHALLENGE;
      url_content_basic_auth_header(auth, auth_len, user, pass);
      return UF_AUTH_BASIC;
   }
   if (https && uf_host_is_archive(hop_host))
   {
      char access[256];
      char secret[256];
      enum uf_auth kind = UF_AUTH_NONE;
      if (url_logins_get_archive_keys(access, sizeof(access),
               secret, sizeof(secret)))
      {
         url_content_archive_auth_header(auth, auth_len, access, secret);
         kind = UF_AUTH_LOW;
      }
      url_content_wipe(access, sizeof(access));
      url_content_wipe(secret, sizeof(secret));
      return kind;
   }
   return UF_AUTH_NONE;
}

enum url_fetch_result url_fetch_to_cache(const char *url, const char *cache_dir,
      const char *user_or_null, const char *pass_or_null,
      char *out_path, size_t out_len, char *challenge, size_t ch_len,
      char *err, size_t err_len, url_fetch_progress_t cb, void *ud,
      struct url_catalog *catalog_or_null)
{
   char fixed[UF_URL_LEN];
   char cur[UF_URL_LEN];
   char next[UF_URL_LEN];
   char uri[UF_URL_LEN];
   char host[256];
   char hop_host[256];
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
   const char *ctype;
   struct uf_response r;
   enum url_fetch_result ret = URL_FETCH_FAILED;
   bool have_login           = false;
   bool downgraded           = false;
   unsigned hops             = 0;

   memset(&r, 0, sizeof(r));
   user[0] = '\0';
   pass[0] = '\0';
   auth[0] = '\0';

   if (out_path && out_len)
      out_path[0] = '\0';
   if (challenge && ch_len)
      challenge[0] = '\0';
   if (err && err_len)
      err[0] = '\0';
   /* Always safe to url_catalog_free afterwards. */
   if (catalog_or_null)
      memset(catalog_or_null, 0, sizeof(*catalog_or_null));

   if (string_is_empty(url) || string_is_empty(cache_dir))
   {
      uf_set_err(err, err_len, "No URL or cache folder.");
      goto end;
   }

   url_content_fix_link(fixed, sizeof(fixed), url);
   url_content_host(host, sizeof(host), fixed);

   /* The hash part of the cache name is the same for any file name. */
   url_content_cache_name(prefix, sizeof(prefix), fixed, "x");
   if (strlen(prefix) < UF_PREFIX_LEN || prefix[UF_PREFIX_LEN - 1] != '_')
   {
      uf_set_err(err, err_len, "Cannot name the cache file.");
      goto end;
   }
   prefix[UF_PREFIX_LEN] = '\0';

   if (!path_is_directory(cache_dir) && !path_mkdir(cache_dir))
   {
      uf_set_err(err, err_len, "Cannot create the cache folder.");
      goto end;
   }

   uf_scan_cache(cache_dir, prefix, true, found, sizeof(found));
   if (found[0])
   {
      if (out_path && out_len)
         fill_pathname_join_special(out_path, cache_dir, found, out_len);
      ret = URL_FETCH_CACHED;
      goto end;
   }

   if (!string_is_empty(user_or_null))
   {
      strlcpy(user, user_or_null, sizeof(user));
      if (pass_or_null)
         strlcpy(pass, pass_or_null, sizeof(pass));
      have_login = true;
   }
   else
      have_login = url_logins_get(host, user, sizeof(user),
            pass, sizeof(pass));

   /* Redirects are followed here, not by net_http, so that the
    * credentials can be chosen again for every hop. */
   strlcpy(cur, fixed, sizeof(cur));
   for (;;)
   {
      enum uf_auth kind;
      const char *loc;

      url_content_host(hop_host, sizeof(hop_host), cur);
      kind = uf_pick_auth(auth, sizeof(auth), cur, hop_host, host,
            downgraded, have_login, user, pass);

      uf_response_free(&r);
      if (!uf_get(cur, auth, true, &r, cb, ud))
      {
         uf_set_err(err, err_len, "Bad URL.");
         goto end;
      }
      if (r.tls_error)
         break;

      /* Answer the server's challenge with the login: Digest when
       * offered (the password itself is never sent), else Basic, which
       * over plain http is only sent once the server asked for it. */
      if (     r.status == 401
            && (kind == UF_AUTH_BASIC || kind == UF_AUTH_ON_CHALLENGE))
      {
         const char *digest = uf_challenge(r.headers, "Digest");
         bool retry         = false;
         uf_request_uri(uri, sizeof(uri), cur);
         if (digest && url_content_digest_auth_header(auth, sizeof(auth),
                  digest, "GET", uri, user, pass))
            retry = true;
         else if (kind == UF_AUTH_ON_CHALLENGE
               && uf_challenge(r.headers, "Basic"))
         {
            url_content_basic_auth_header(auth, sizeof(auth), user, pass);
            retry = true;
         }
         if (retry)
         {
            uf_response_free(&r);
            if (!uf_get(cur, auth, true, &r, cb, ud))
            {
               uf_set_err(err, err_len, "Bad URL.");
               goto end;
            }
            if (r.tls_error)
               break;
         }
      }

      if (     r.err
            || r.status < 300 || r.status > 399
            || !(loc = uf_header(r.headers, "Location")))
         break;

      if (++hops > UF_MAX_HOPS)
      {
         uf_set_err(err, err_len, "Too many redirects.");
         goto end;
      }
      if (!url_content_resolve(next, sizeof(next), cur, loc))
      {
         uf_set_err(err, err_len, "Bad redirect.");
         goto end;
      }
      if (     string_starts_with_case_insensitive(cur, "https://")
            && !string_starts_with_case_insensitive(next, "https://"))
         downgraded = true;
      strlcpy(cur, next, sizeof(cur));
   }

   if (r.tls_error)
   {
      uf_set_err(err, err_len, r.tls_error == NET_HTTP_TLS_NO_CA
            ? "Can't check this site's security certificate, so the login was not sent."
            : "This site's security certificate is not valid, so the login was not sent.");
      goto end;
   }

   if (r.status == 401)
   {
      const char *ch = uf_challenge(r.headers, "Digest");
      if (!ch)
         ch = uf_challenge(r.headers, NULL);
      if (ch && challenge && ch_len)
         strlcpy(challenge, ch, ch_len);
      uf_set_err(err, err_len, "Login needed.");
      ret = URL_FETCH_NEED_LOGIN;
      goto end;
   }

   if (r.too_big)
   {
      uf_set_err(err, err_len, "File is too big.");
      ret = URL_FETCH_TOO_BIG;
      goto end;
   }

   if (r.err)
   {
      uf_set_err(err, err_len, "Download failed.");
      goto end;
   }

   if (r.status < 200 || r.status > 299)
   {
      char msg[64];
      snprintf(msg, sizeof(msg), "HTTP error %d.", r.status);
      uf_set_err(err, err_len, msg);
      goto end;
   }

   if (!r.len)
   {
      uf_set_err(err, err_len, "The server sent an empty file.");
      goto end;
   }

   if (!r.data)
   {
      uf_set_err(err, err_len, "Out of memory.");
      ret = URL_FETCH_TOO_BIG;
      goto end;
   }

   clen = uf_header(r.headers, "Content-Length");
   if (clen && !uf_length_matches(clen, r.len))
   {
      uf_set_err(err, err_len, "Download was cut short.");
      goto end;
   }

   /* A game list (Caddy folder listing): nothing goes to the cache.
    * cur is the URL after redirects, so the entries resolve against
    * the folder that actually answered. JSON that is not a listing
    * falls through and is saved as a file, as before. */
   ctype = uf_header(r.headers, "Content-Type");
   if (ctype && string_starts_with_case_insensitive(ctype, "application/json"))
   {
      struct url_catalog tmp;
      memset(&tmp, 0, sizeof(tmp));
      if (url_catalog_parse((const char*)r.data, r.len, cur, &tmp))
      {
         if (catalog_or_null)
            *catalog_or_null = tmp;
         else
         {
            url_catalog_free(&tmp);
            uf_set_err(err, err_len, "This link is a game list. Open it from the menu: Load Content > Load from URL.");
         }
         ret = URL_FETCH_CATALOG;
         goto end;
      }
      url_catalog_free(&tmp);
   }

   if (url_content_looks_like_html(r.data, r.len))
   {
      uf_set_err(err, err_len, "Got a web page, not a file.");
      ret = URL_FETCH_NOT_A_FILE;
      goto end;
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
      goto end;
   }

   if (out_path && out_len)
      strlcpy(out_path, final_path, out_len);
   ret = URL_FETCH_OK;

end:
   uf_response_free(&r);
   url_content_wipe(user, sizeof(user));
   url_content_wipe(pass, sizeof(pass));
   url_content_wipe(auth, sizeof(auth));
   return ret;
}

/* True for "<16 lower-case hex>_..." (the names url_fetch writes). */
static bool uf_is_cache_name(const char *name)
{
   int i;
   for (i = 0; i < UF_PREFIX_LEN - 1; i++)
   {
      char c = name[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }
   return name[UF_PREFIX_LEN - 1] == '_' && name[UF_PREFIX_LEN] != '\0';
}

bool url_fetch_clear_cache(const char *cache_dir, unsigned *removed)
{
   char dir[PATH_MAX_LENGTH];
   size_t n;
   struct RDIR *rd;
   bool ok = true;

   if (removed)
      *removed = 0;
   if (string_is_empty(cache_dir))
      return false;

   /* Guard: only ever a folder named url_cache. */
   strlcpy(dir, cache_dir, sizeof(dir));
   n = strlen(dir);
   while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
      dir[--n] = '\0';
   if (!string_is_equal(path_basename(dir), "url_cache"))
      return false;
   if (!path_is_directory(dir))
      return true; /* nothing downloaded yet */

   if (!(rd = retro_opendir_include_hidden(dir, true)))
      return false;
   while (retro_readdir(rd))
   {
      char path[PATH_MAX_LENGTH];
      const char *name = retro_dirent_get_name(rd);
      /* Files only, no sub-folders, and only names url_fetch made. */
      if (     !name
            || retro_dirent_is_dir(rd, NULL)
            || !uf_is_cache_name(name))
         continue;
      fill_pathname_join_special(path, dir, name, sizeof(path));
      if (filestream_delete(path) == 0)
      {
         if (removed)
            (*removed)++;
      }
      else
         ok = false;
   }
   retro_closedir(rd);
   return ok;
}

#endif /* HAVE_NETWORKING */
