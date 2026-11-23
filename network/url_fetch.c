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

#ifdef HAVE_NETWORKING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <compat/strl.h>
#include <encodings/utf.h>
#include <file/file_path.h>
#include <retro_dirent.h>
#include <retro_miscellaneous.h>
#include <retro_timers.h>
#include <features/features_cpu.h>
#include <lists/string_list.h>
#include <lrc_hash.h>
#include <net/net_http.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "url_content.h"
#include "url_logins.h"
#include "url_safety.h"
#include "url_fetch.h"

#define UF_URL_LEN      4096
#define UF_HDR_LEN      2048
#define UF_PREFIX_LEN   17         /* 16 hex + '_' */
#define UF_STALL_USEC   30000000   /* give up after 30 s without data */
#define UF_MAX_HOPS     10
#define UF_MAX_BODY     ((size_t)2048 * 1024 * 1024)   /* 2 GiB */
/* A JSON answer (maybe a game list) is kept in memory up to this,
 * then treated as a plain file. */
#define UF_JSON_MEM     ((size_t)16 * 1024 * 1024)
/* A .part file untouched this long is a leftover (a crash); younger
 * ones may belong to another download of the same link right now. */
#define UF_PART_STALE_SEC 3600
/* Files first; Caddy answers a folder with its JSON listing. */
#define UF_ACCEPT       "Accept: */*, application/json;q=0.9\r\n"

enum uf_auth
{
   UF_AUTH_NONE = 0,
   UF_AUTH_BASIC,       /* https: Basic sent with the first request */
   UF_AUTH_LOW
};

#define UF_SITE_LEN     300
#define UF_MSG_LOGIN_HTTP "This server asks for a password without encryption (http). Use its https:// address."

/* Where a 2xx body goes while it arrives: straight into the .part
 * file, except a JSON answer (a game list is parsed from memory),
 * which stays in mem until it outgrows UF_JSON_MEM. head keeps the
 * first bytes for the web-page and program checks. Bodies of other
 * answers (3xx, 401, errors) are dropped. */
struct uf_sink
{
   const char *part_path;
   RFILE *file;
   uint8_t *mem;
   size_t mem_len;
   size_t mem_cap;
   uint8_t head[URL_SAFETY_HEAD_LEN];
   size_t head_len;
   bool decided;
   bool json;
   bool write_err;
};

struct uf_response
{
   size_t len;                 /* 2xx body bytes received */
   size_t total;               /* Content-Length seen by net_http, 0 if unknown */
   struct string_list *headers;
   int status;
   int tls_error;             /* enum net_http_tls_error */
   bool err;
   bool too_big;              /* over UF_MAX_BODY or out of memory */
   struct uf_sink sink;
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

/* Closes and deletes an unfinished .part file and frees the memory
 * copy. */
static void uf_sink_reset(struct uf_sink *s)
{
   if (s->file)
   {
      filestream_close(s->file);
      s->file = NULL;
      if (s->part_path)
         filestream_delete(s->part_path);
   }
   free(s->mem);
   s->mem      = NULL;
   s->mem_len  = 0;
   s->mem_cap  = 0;
   s->head_len = 0;
   s->decided  = false;
   s->json     = false;
   s->write_err = false;
}

static void uf_response_free(struct uf_response *r)
{
   if (r->headers)
      string_list_free(r->headers);
   r->headers = NULL;
   uf_sink_reset(&r->sink);
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

static bool uf_sink_write_file(struct uf_sink *s, const uint8_t *data,
      size_t len)
{
   if (!s->file && !(s->file = filestream_open(s->part_path,
               RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return false;
   return !len || filestream_write(s->file, data, (int64_t)len) == (int64_t)len;
}

/* net_http body callback (fetch thread). */
static bool uf_sink_cb(void *ud, int status,
      const struct string_list *headers, const uint8_t *data, size_t len)
{
   struct uf_response *r = (struct uf_response*)ud;
   struct uf_sink *s     = &r->sink;

   if (status < 200 || status > 299)
      return true;            /* not the file: nothing to keep */
   if (!s->decided)
   {
      const char *ctype = uf_header(headers, "Content-Type");
      s->decided = true;
      s->json    = ctype
         && string_starts_with_case_insensitive(ctype, "application/json");
   }
   r->len += len;
   if (s->head_len < sizeof(s->head))
   {
      size_t n = sizeof(s->head) - s->head_len;
      if (n > len)
         n = len;
      memcpy(s->head + s->head_len, data, n);
      s->head_len += n;
   }

   if (s->json && !s->file)
   {
      if (s->mem_len + len <= UF_JSON_MEM)
      {
         if (s->mem_len + len > s->mem_cap)
         {
            size_t cap = s->mem_cap ? s->mem_cap : 64 * 1024;
            uint8_t *tmp;
            while (cap < s->mem_len + len)
               cap *= 2;
            if (cap > UF_JSON_MEM)
               cap = UF_JSON_MEM;
            if (!(tmp = (uint8_t*)realloc(s->mem, cap)))
            {
               s->write_err = true;
               return false;
            }
            s->mem     = tmp;
            s->mem_cap = cap;
         }
         memcpy(s->mem + s->mem_len, data, len);
         s->mem_len += len;
         return true;
      }
      /* Too big for a game list: from here on a plain file. */
      if (!uf_sink_write_file(s, s->mem, s->mem_len))
      {
         s->write_err = true;
         return false;
      }
      free(s->mem);
      s->mem     = NULL;
      s->mem_len = 0;
      s->mem_cap = 0;
   }
   if (!uf_sink_write_file(s, data, len))
   {
      s->write_err = true;
      return false;
   }
   return true;
}

/* Blocking GET. Fills r; returns false only if no request could be
 * made at all (bad URL / out of memory). A 2xx body goes to part_path
 * as it arrives (see struct uf_sink); r->len counts it. An https
 * request needs a verified certificate unless skip_verify is set, and
 * always when it carries a login or keys (non-empty auth): without
 * one nothing is sent and r->tls_error says why. */
static bool uf_get(const char *url, const char *auth_or_null,
      bool accept_json, bool skip_verify, const char *part_path,
      struct uf_response *r, url_fetch_progress_t cb, void *ud)
{
   bool verify = string_starts_with_case_insensitive(url, "https://")
      && (!skip_verify || (auth_or_null && *auth_or_null));
   char headers[UF_HDR_LEN + sizeof(UF_ACCEPT)];
   struct http_connection_t *conn;
   struct http_t *http;
   retro_time_t last_change;
   size_t last_done = (size_t)-1;

   memset(r, 0, sizeof(*r));
   r->status         = -1;
   r->sink.part_path = part_path;

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
   net_http_connection_set_body_cb(conn, uf_sink_cb, r);
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

   r->too_big   = net_http_too_big(http) && !r->sink.write_err;
   r->tls_error = net_http_tls_error(http);
   r->status  = net_http_status(http);
   r->err     = r->err || r->status < 0 || r->sink.write_err;
   r->headers = net_http_response_headers(http);
   /* net_http_delete frees neither the (here empty) body buffer nor
    * the header list. */
   free(net_http_data(http, NULL, true));
   net_http_delete(http);
   /* Flush the file now, so a write error shows up here. */
   if (r->sink.file && filestream_flush(r->sink.file) != 0)
      r->err = r->sink.write_err = true;
   return true;
}

/* True if the file at path was last written more than
 * UF_PART_STALE_SEC ago. False if that can't be told. */
static bool uf_part_is_stale(const char *path)
{
#if defined(_WIN32)
   struct _stat64 st;
   int rc;
   wchar_t *w = utf8_to_utf16_string_alloc(path);
   if (!w)
      return false;
   rc = _wstat64(w, &st);
   free(w);
#else
   struct stat st;
   int rc = stat(path, &st);
#endif
   if (rc != 0)
      return false;
   return difftime(time(NULL), st.st_mtime) > UF_PART_STALE_SEC;
}

/* <prefix>.unverified marks a link's cached file as taken with
 * "Download anyway (unsafe)": no certificate check. A cache name never
 * starts its name part with a dot, so it can't clash with a file. */
#define UF_UNVERIFIED_SUFFIX ".unverified"

static void uf_marker_path(char *out, size_t len, const char *cache_dir,
      const char *prefix)
{
   fill_pathname_join_special(out, cache_dir, prefix, len);
   strlcat(out, UF_UNVERIFIED_SUFFIX, len);
}

/* Scans cache_dir for <prefix>*: deletes leftover (stale) .part files
 * and copies the first finished file name into found (empty if none).
 * The unverified marker is never a found file. */
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
      if (string_is_equal(name + UF_PREFIX_LEN, UF_UNVERIFIED_SUFFIX))
         continue;
      if (string_ends_with(name, ".part"))
      {
         if (delete_parts)
         {
            char path[PATH_MAX_LENGTH];
            fill_pathname_join_special(path, cache_dir, name, sizeof(path));
            if (uf_part_is_stale(path))
               filestream_delete(path);
         }
      }
      else if (!found[0])
         strlcpy(found, name, found_len);
   }
   retro_closedir(dir);
}

/* Picks the Authorization header for one hop. A login (typed or
 * saved) goes only to its own site (scheme, host and port), only over
 * https, as Basic right away (and as Digest if the 401 asks for it);
 * never over plain http. archive.org keys go only to archive.org hosts
 * over https. Nothing once an https hop led to plain http. Every
 * request that carries either is certificate-checked (uf_get). */
static enum uf_auth uf_pick_auth(char *auth, size_t auth_len,
      const char *hop_url, const char *hop_host, const char *hop_site,
      const char *login_site, bool downgraded, bool have_login,
      const char *user, const char *pass)
{
   bool https = string_starts_with_case_insensitive(hop_url, "https://");
   auth[0] = '\0';
   if (downgraded || !https)
      return UF_AUTH_NONE;
   if (have_login && login_site[0] && string_is_equal(hop_site, login_site))
   {
      url_content_basic_auth_header(auth, auth_len, user, pass);
      return UF_AUTH_BASIC;
   }
   if (uf_host_is_archive(hop_host))
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

/* Up to len first bytes of the file at path (fewer if it is shorter).
 * Returns how many, (size_t)-1 if it can't be read. */
static size_t uf_read_head(const char *path, uint8_t *head, size_t len)
{
   int64_t got;
   RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!f)
      return (size_t)-1;
   got = filestream_read(f, head, (int64_t)len);
   filestream_close(f);
   return got < 0 ? (size_t)-1 : (size_t)got;
}

/* The not-a-game and archive checks on a finished cache file (name =
 * its cache name, head = its first bytes). A refused file is deleted
 * and err says why. */
static enum url_fetch_result uf_check_safety(const char *path,
      const char *name, const uint8_t *head, size_t head_len,
      char *err, size_t err_len)
{
   enum url_safety_verdict v = url_safety_check_file(name, head, head_len);
   if (v == URL_SAFE_OK)
      v = url_safety_check_archive(path);
   if (v == URL_SAFE_OK)
      return URL_FETCH_OK;
   filestream_delete(path);
   switch (v)
   {
      case URL_SAFE_PROGRAM:
         uf_set_err(err, err_len, "Blocked: this link gives a program, not a game.");
         return URL_FETCH_BLOCKED_PROGRAM;
      case URL_SAFE_ARCHIVE_TOO_BIG:
         uf_set_err(err, err_len, "Blocked: this archive unpacks to more than 2 GB.");
         return URL_FETCH_BLOCKED_TOO_BIG;
      default:
         uf_set_err(err, err_len, "Blocked: this archive contains a program or an unsafe file path.");
         return URL_FETCH_BLOCKED_ARCHIVE;
   }
}

#define UF_MSG_MISMATCH "Blocked: this file is not the one that was shared (fingerprint mismatch)."

/* True if the file at path hashes to fp. False if not or unreadable. */
static bool uf_file_matches(const char *path, const uint8_t fp[32])
{
   uint8_t d[32];
   return sha256_calculate(path, d) == 0 && memcmp(d, fp, sizeof(d)) == 0;
}

/* True if the bytes hash to fp. */
static bool uf_mem_matches(const uint8_t *data, size_t len,
      const uint8_t fp[32])
{
   char hex[65];
   char want[65];
   size_t i;
   sha256_hash(hex, data, len);
   for (i = 0; i < 32; i++)
      snprintf(want + 2 * i, 3, "%02x", (unsigned)fp[i]);
   return string_is_equal(hex, want);
}

enum url_fetch_result url_fetch_to_cache(const char *url, const char *cache_dir,
      const char *user_or_null, const char *pass_or_null,
      char *out_path, size_t out_len, char *challenge, size_t ch_len,
      char *err, size_t err_len, url_fetch_progress_t cb, void *ud,
      struct url_catalog *catalog_or_null)
{
   return url_fetch_to_cache_ex(url, cache_dir, user_or_null, pass_or_null,
         out_path, out_len, challenge, ch_len, err, err_len, cb, ud,
         catalog_or_null, false, NULL, 0);
}

enum url_fetch_result url_fetch_to_cache_ex(const char *url,
      const char *cache_dir,
      const char *user_or_null, const char *pass_or_null,
      char *out_path, size_t out_len, char *challenge, size_t ch_len,
      char *err, size_t err_len, url_fetch_progress_t cb, void *ud,
      struct url_catalog *catalog_or_null, bool download_anyway,
      char *login_site_out, size_t ls_len)
{
   char link[UF_URL_LEN];
   char site[UF_SITE_LEN];        /* scheme://host[:port] of the link */
   char hop_site[UF_SITE_LEN];
   char login_site[UF_SITE_LEN];  /* the https site a login may go to */
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
   char marker[PATH_MAX_LENGTH];  /* <prefix>.unverified */
   const char *disp;
   const char *clen;
   /* On the heap: the sink holds the first 64 KiB of the body. */
   struct uf_response *r     = (struct uf_response*)calloc(1, sizeof(*r));
   enum url_fetch_result ret = URL_FETCH_FAILED;
   bool have_login           = false;
   bool downgraded           = false;
   bool has_fp               = false;
   bool skip_verify          = false;
   bool unverified           = false; /* taken with "Download anyway" */
   bool tls_auth             = false; /* the refused request had a login */
   bool typed_login          = false;
   bool login_sent           = false;
   bool crossed              = false; /* a hop left the link's host */
   uint8_t fp[32];
   unsigned hops             = 0;

   user[0] = '\0';
   pass[0] = '\0';
   auth[0] = '\0';

   if (out_path && out_len)
      out_path[0] = '\0';
   if (challenge && ch_len)
      challenge[0] = '\0';
   if (err && err_len)
      err[0] = '\0';
   if (login_site_out && ls_len)
      login_site_out[0] = '\0';
   login_site[0] = '\0';
   /* Always safe to url_catalog_free afterwards. */
   if (catalog_or_null)
      memset(catalog_or_null, 0, sizeof(*catalog_or_null));

   if (!r)
   {
      uf_set_err(err, err_len, "Out of memory.");
      return URL_FETCH_TOO_BIG;
   }

   if (string_is_empty(url) || string_is_empty(cache_dir))
   {
      uf_set_err(err, err_len, "No URL or cache folder.");
      goto end;
   }

   /* The fragment (and a #sha256= fingerprint in it) never goes to the
    * server and is not part of the cache name. */
   if (!url_content_split_fingerprint(link, sizeof(link), fp, &has_fp, url))
   {
      uf_set_err(err, err_len, "This link's fingerprint is broken: #sha256= needs 64 hex digits.");
      goto end;
   }
   url_content_fix_link(fixed, sizeof(fixed), link);
   url_content_host(host, sizeof(host), fixed);
   url_content_origin(site, sizeof(site), fixed);
   /* Logins only ever go to an https site: the link's own, or (below)
    * the https site the server moves an http link to on the same
    * host. */
   if (string_starts_with_case_insensitive(site, "https://"))
      strlcpy(login_site, site, sizeof(login_site));

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

   uf_marker_path(marker, sizeof(marker), cache_dir, prefix);
   uf_scan_cache(cache_dir, prefix, true, found, sizeof(found));
   /* A file taken with "Download anyway" was never checked: only a
    * fingerprint can vouch for it. Otherwise it is downloaded again
    * (with the certificate check), never reused silently. */
   if (found[0] && !has_fp && path_is_valid(marker))
   {
      fill_pathname_join_special(final_path, cache_dir, found,
            sizeof(final_path));
      filestream_delete(final_path);
      found[0] = '\0';
   }
   if (found[0])
   {
      /* Checked again on every use: the file may predate the checks
       * or have been changed since. */
      size_t n;
      fill_pathname_join_special(final_path, cache_dir, found,
            sizeof(final_path));
      n = uf_read_head(final_path, r->sink.head, sizeof(r->sink.head));
      if (n == (size_t)-1)
      {
         uf_set_err(err, err_len, "Cannot read the cache file.");
         goto end;
      }
      if (has_fp && !uf_file_matches(final_path, fp))
      {
         filestream_delete(final_path);
         uf_set_err(err, err_len, UF_MSG_MISMATCH);
         ret = URL_FETCH_BLOCKED_FINGERPRINT;
         goto end;
      }
      ret = uf_check_safety(final_path, found, r->sink.head, n,
            err, err_len);
      if (ret != URL_FETCH_OK)
         goto end;
      /* Files from older builds have no mark yet. */
      url_safety_mark_downloaded(final_path, fixed);
      if (out_path && out_len)
         strlcpy(out_path, final_path, out_len);
      ret = URL_FETCH_CACHED;
      goto end;
   }

   /* The body is written here as it arrives; renamed once complete
    * and checked. The name is this download's own, so two downloads
    * of one link at the same time (two threads or two RetroArch
    * instances) never share or delete each other's file. Leftovers
    * are removed by uf_scan_cache once stale. */
   {
      static unsigned counter = 0;
      char tag[64];
      snprintf(tag, sizeof(tag), "dl%llx%x%lx.part",
            (unsigned long long)cpu_features_get_time_usec(),
            ++counter, (unsigned long)((size_t)r & 0xFFFFFFFFu));
      fill_pathname_join_special(part_path, cache_dir, prefix,
            sizeof(part_path));
      strlcat(part_path, tag, sizeof(part_path));
   }

   if (!string_is_empty(user_or_null))
   {
      strlcpy(user, user_or_null, sizeof(user));
      if (pass_or_null)
         strlcpy(pass, pass_or_null, sizeof(pass));
      have_login  = true;
      typed_login = true;
   }
   else if (login_site[0])
      have_login = url_logins_get(login_site, user, sizeof(user),
            pass, sizeof(pass));

   /* A fingerprint protects the file, so its certificate need not.
    * "Download anyway" never applies to a download that uses a login
    * (typed or saved), not even for its hops without one. Requests
    * that carry a login or keys are always checked (uf_get). */
   skip_verify = has_fp || (download_anyway && !have_login);

   /* Redirects are followed here, not by net_http, so that the
    * credentials can be chosen again for every hop. */
   strlcpy(cur, fixed, sizeof(cur));
   for (;;)
   {
      enum uf_auth kind;
      const char *loc;

      url_content_host(hop_host, sizeof(hop_host), cur);
      url_content_origin(hop_site, sizeof(hop_site), cur);
      if (!string_is_equal(hop_host, host))
         crossed = true;
      /* http link, moved to https on the same host (and nowhere else
       * before): that https site is the one a login belongs to. */
      if (     !login_site[0] && !crossed && !downgraded
            && string_starts_with_case_insensitive(hop_site, "https://"))
      {
         strlcpy(login_site, hop_site, sizeof(login_site));
         if (!typed_login)
         {
            have_login = url_logins_get(login_site, user, sizeof(user),
                  pass, sizeof(pass));
            if (have_login)
               skip_verify = has_fp;
         }
      }
      kind = uf_pick_auth(auth, sizeof(auth), cur, hop_host, hop_site,
            login_site, downgraded, have_login, user, pass);

      uf_response_free(r);
      tls_auth = auth[0] != '\0';
      if (kind == UF_AUTH_BASIC)
         login_sent = true;
      if (!uf_get(cur, auth, true, skip_verify, part_path, r, cb, ud))
      {
         uf_set_err(err, err_len, "Bad URL.");
         goto end;
      }
      if (r->tls_error)
         break;

      /* Basic was refused over https: answer a Digest challenge once
       * (also over the checked connection). */
      if (r->status == 401 && kind == UF_AUTH_BASIC)
      {
         const char *digest = uf_challenge(r->headers, "Digest");
         uf_request_uri(uri, sizeof(uri), cur);
         if (digest && url_content_digest_auth_header(auth, sizeof(auth),
                  digest, "GET", uri, user, pass))
         {
            uf_response_free(r);
            tls_auth = true;
            if (!uf_get(cur, auth, true, skip_verify, part_path, r, cb, ud))
            {
               uf_set_err(err, err_len, "Bad URL.");
               goto end;
            }
            if (r->tls_error)
               break;
         }
      }

      if (     r->err
            || r->status < 300 || r->status > 399
            || !(loc = uf_header(r->headers, "Location")))
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

   if (r->tls_error)
   {
      if (!tls_auth && !have_login)
      {
         /* Nothing was sent that needs protecting: the caller may offer
          * "Download anyway (unsafe)" (never for a login). */
         uf_set_err(err, err_len, "This site's security certificate is not valid. Someone may be changing the file on the way.");
         ret = URL_FETCH_CERT_INVALID;
         goto end;
      }
      uf_set_err(err, err_len, r->tls_error == NET_HTTP_TLS_NO_CA
            ? "Can't check this site's security certificate, so the login was not sent."
            : "This site's security certificate is not valid, so the login was not sent.");
      goto end;
   }

   /* A password over plain http could be read (Basic) or cracked
    * (Digest) on the way: never sent, and no login prompt. */
   if (     r->status == 401
         && !string_starts_with_case_insensitive(cur, "https://"))
   {
      uf_set_err(err, err_len, UF_MSG_LOGIN_HTTP);
      ret = URL_FETCH_LOGIN_HTTP;
      goto end;
   }

   if (r->status == 401)
   {
      const char *ch = uf_challenge(r->headers, "Digest");
      if (!ch)
         ch = uf_challenge(r->headers, NULL);
      if (ch && challenge && ch_len)
         strlcpy(challenge, ch, ch_len);
      uf_set_err(err, err_len, "Login needed.");
      ret = URL_FETCH_NEED_LOGIN;
      goto end;
   }

   if (r->too_big)
   {
      uf_set_err(err, err_len, "File is too big.");
      ret = URL_FETCH_TOO_BIG;
      goto end;
   }

   if (r->sink.write_err)
   {
      uf_set_err(err, err_len, "Cannot write the cache file.");
      goto end;
   }

   if (r->err)
   {
      uf_set_err(err, err_len, "Download failed.");
      goto end;
   }

   if (r->status < 200 || r->status > 299)
   {
      char msg[64];
      snprintf(msg, sizeof(msg), "HTTP error %d.", r->status);
      uf_set_err(err, err_len, msg);
      goto end;
   }

   if (!r->len)
   {
      uf_set_err(err, err_len, "The server sent an empty file.");
      goto end;
   }

   clen = uf_header(r->headers, "Content-Length");
   if (clen && !uf_length_matches(clen, r->len))
   {
      uf_set_err(err, err_len, "Download was cut short.");
      goto end;
   }

   /* A game list (Caddy folder listing): nothing goes to the cache.
    * cur is the URL after redirects, so the entries resolve against
    * the folder that actually answered. JSON that is not a listing
    * falls through and is saved as a file, as before. */
   if (r->sink.json && r->sink.mem)
   {
      struct url_catalog tmp;
      memset(&tmp, 0, sizeof(tmp));
      /* A fingerprinted link must match whatever it gives, a list too. */
      if (has_fp && !uf_mem_matches(r->sink.mem, r->sink.mem_len, fp))
      {
         uf_set_err(err, err_len, UF_MSG_MISMATCH);
         ret = URL_FETCH_BLOCKED_FINGERPRINT;
         goto end;
      }
      if (url_catalog_parse((const char*)r->sink.mem, r->sink.mem_len,
               cur, &tmp))
      {
         if (login_sent && login_site_out && ls_len)
            strlcpy(login_site_out, login_site, ls_len);
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
      if (!uf_sink_write_file(&r->sink, r->sink.mem, r->sink.mem_len))
      {
         uf_set_err(err, err_len, "Cannot write the cache file.");
         goto end;
      }
   }

   if (url_content_looks_like_html(r->sink.head, r->sink.head_len))
   {
      uf_set_err(err, err_len, "Got a web page, not a file.");
      ret = URL_FETCH_NOT_A_FILE;
      goto end;
   }

   name[0] = '\0';
   disp    = uf_header(r->headers, "Content-Disposition");
   if (disp)
      url_content_name_from_disposition(name, sizeof(name), disp);
   url_content_cache_name(found, sizeof(found), fixed,
         name[0] ? name : NULL);

   fill_pathname_join_special(final_path, cache_dir, found,
         sizeof(final_path));

   /* Keep the finished .part (closed, not deleted), check its
    * fingerprint, then rename: a wrong file never gets the real name. */
   unverified = skip_verify && !has_fp;
   if (r->sink.file)
   {
      bool ok = filestream_close(r->sink.file) == 0;
      r->sink.file = NULL;
      if (ok && has_fp && !uf_file_matches(part_path, fp))
      {
         filestream_delete(part_path);
         uf_set_err(err, err_len, UF_MSG_MISMATCH);
         ret = URL_FETCH_BLOCKED_FINGERPRINT;
         goto end;
      }
      /* Taken without the certificate check (and no fingerprint):
       * marked before the file gets its name, so it is never seen
       * as a checked file. */
      if (     ok && unverified
            && !filestream_write_file(marker, "", 0))
         ok = false;
      if (!ok || filestream_rename(part_path, final_path) != 0)
      {
         filestream_delete(part_path);
         uf_set_err(err, err_len, "Cannot write the cache file.");
         goto end;
      }
      /* A checked (or fingerprinted) file replaces an older mark. */
      if (!unverified && path_is_valid(marker))
         filestream_delete(marker);
   }
   else
   {
      uf_set_err(err, err_len, "Cannot write the cache file.");
      goto end;
   }

   ret = uf_check_safety(final_path, found, r->sink.head, r->sink.head_len,
         err, err_len);
   if (ret != URL_FETCH_OK)
      goto end;

   url_safety_mark_downloaded(final_path, fixed);
   if (out_path && out_len)
      strlcpy(out_path, final_path, out_len);
   if (login_sent && login_site_out && ls_len)
      strlcpy(login_site_out, login_site, ls_len);
   ret = URL_FETCH_OK;

end:
   uf_response_free(r);
   free(r);
   url_content_wipe(user, sizeof(user));
   url_content_wipe(pass, sizeof(pass));
   url_content_wipe(auth, sizeof(auth));
   url_content_wipe(link, sizeof(link));
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
