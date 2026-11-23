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

#include <compat/strl.h>
#include <encodings/base64.h>
#include <lrc_hash.h>
#include <formats/rjson.h>

#include "url_content.h"

#define UC_FIELD_LEN     256
/* Longest sanitized file name kept (bytes). Leaves room for the cache
 * folder and the 17-byte hash prefix under Windows' 260-char paths. */
#define UC_NAME_MAX      100

static char uc_test_cnonce[64] = {0};

void url_content_wipe(void *p, size_t len)
{
   volatile unsigned char *b = (volatile unsigned char*)p;
   if (!p)
      return;
   while (len--)
      *b++ = 0;
}

/* ---------- small string helpers ---------- */

static char uc_lower(char c)
{
   if (c >= 'A' && c <= 'Z')
      return (char)(c - 'A' + 'a');
   return c;
}

static bool uc_is_space(char c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n'
       || c == '\f' || c == '\v';
}

/* Case-insensitive prefix test */
static bool uc_starts_with_ci(const char *s, const char *prefix)
{
   while (*prefix)
   {
      if (uc_lower(*s) != uc_lower(*prefix))
         return false;
      s++;
      prefix++;
   }
   return true;
}

/* Case-insensitive substring search */
static const char *uc_find_ci(const char *s, const char *needle)
{
   for (; *s; s++)
      if (uc_starts_with_ci(s, needle))
         return s;
   return NULL;
}

/* Copies at most n bytes of src into out (always terminated) */
static void uc_copy_n(char *out, size_t len, const char *src, size_t n)
{
   if (!len)
      return;
   if (n >= len)
      n = len - 1;
   memcpy(out, src, n);
   out[n] = '\0';
}

static int uc_hex_val(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* Percent-decodes n bytes of src into out. Invalid escapes are kept. */
static void uc_pct_decode(char *out, size_t len, const char *src, size_t n)
{
   size_t i = 0;
   size_t o = 0;

   if (!len)
      return;

   while (i < n && o + 1 < len)
   {
      if (src[i] == '%' && i + 2 < n
            && uc_hex_val(src[i + 1]) >= 0 && uc_hex_val(src[i + 2]) >= 0)
      {
         out[o++] = (char)(uc_hex_val(src[i + 1]) * 16
               + uc_hex_val(src[i + 2]));
         i       += 3;
      }
      else
         out[o++] = src[i++];
   }
   out[o] = '\0';
}

static void uc_hex(char *out, const uint8_t *data, size_t n)
{
   static const char digits[] = "0123456789abcdef";
   size_t i;
   for (i = 0; i < n; i++)
   {
      out[i * 2]     = digits[data[i] >> 4];
      out[i * 2 + 1] = digits[data[i] & 0x0f];
   }
   out[n * 2] = '\0';
}

/* ---------- URL parsing ---------- */

/* Start of the authority (after "scheme://"), or NULL */
static const char *uc_authority(const char *url)
{
   const char *p = strstr(url, "://");
   if (!p)
      return NULL;
   return p + 3;
}

/* First character after the authority: '/', '?', '#' or '\0' */
static const char *uc_authority_end(const char *auth)
{
   while (*auth && *auth != '/' && *auth != '?' && *auth != '#')
      auth++;
   return auth;
}

/* End of the path part: '?', '#' or '\0' */
static const char *uc_path_end(const char *path)
{
   while (*path && *path != '?' && *path != '#')
      path++;
   return path;
}

bool url_content_resolve(char *out, size_t len, const char *base,
      const char *loc)
{
   const char *auth;
   const char *path;
   size_t n;

   while (*loc == ' ' || *loc == '\t')
      loc++;
   if (!*loc)
      return false;
   if (strstr(loc, "://"))
   {
      if (     !uc_starts_with_ci(loc, "http://")
            && !uc_starts_with_ci(loc, "https://"))
         return false;
      return strlcpy(out, loc, len) < len;
   }

   if (!(auth = strstr(base, "://")))
      return false;
   auth += 3;
   if (loc[0] == '/' && loc[1] == '/')
   {
      n = (size_t)(auth - base) - 2; /* "http:" */
      if (n + strlen(loc) + 1 > len)
         return false;
      memcpy(out, base, n);
      out[n] = '\0';
      return strlcat(out, loc, len) < len;
   }

   path = auth;
   while (*path && *path != '/' && *path != '?' && *path != '#')
      path++;
   if (loc[0] == '/')
      n = (size_t)(path - base);
   else
   {
      /* Directory of the current path (query and fragment dropped). */
      const char *end   = path + strcspn(path, "?#");
      const char *slash = NULL;
      const char *p;
      for (p = path; p < end; p++)
         if (*p == '/')
            slash = p;
      n = slash ? (size_t)(slash + 1 - base) : (size_t)(path - base);
   }
   if (n + 2 > len)
      return false;
   memcpy(out, base, n);
   out[n] = '\0';
   if (loc[0] != '/' && (!n || out[n - 1] != '/'))
      strlcat(out, "/", len);
   return strlcat(out, loc, len) < len;
}

/* True if host equals domain or is a subdomain of it */
static bool uc_host_is(const char *host, const char *domain)
{
   size_t lh = strlen(host);
   size_t ld = strlen(domain);
   if (lh == ld)
      return strcmp(host, domain) == 0;
   if (lh > ld)
      return host[lh - ld - 1] == '.'
         && strcmp(host + lh - ld, domain) == 0;
   return false;
}

void url_content_host(char *out, size_t len, const char *url)
{
   const char *auth;
   const char *end;
   const char *at;
   const char *p;
   size_t i, n;

   if (!len)
      return;
   out[0] = '\0';
   if (!url || !(auth = uc_authority(url)))
      return;

   end = uc_authority_end(auth);

   /* Skip "user:pass@" */
   at  = NULL;
   for (p = auth; p < end; p++)
      if (*p == '@')
         at = p;
   if (at)
      auth = at + 1;

   if (*auth == '[')
   {
      /* IPv6 literal: keep the brackets, drop the port */
      p = auth;
      while (p < end && *p != ']')
         p++;
      if (p < end)
         p++;
   }
   else
   {
      p = auth;
      while (p < end && *p != ':')
         p++;
   }

   n = (size_t)(p - auth);
   uc_copy_n(out, len, auth, n);
   for (i = 0; out[i]; i++)
      out[i] = uc_lower(out[i]);
}

bool url_content_origin(char *out, size_t len, const char *url)
{
   const char *scheme;
   const char *auth;
   const char *end;
   const char *p;
   char host[256];
   unsigned long port = 0;
   bool has_port      = false;
   int n;

   if (!out || !len)
      return false;
   out[0] = '\0';
   if (!url)
      return false;
   if (uc_starts_with_ci(url, "https://"))
      scheme = "https";
   else if (uc_starts_with_ci(url, "http://"))
      scheme = "http";
   else
      return false;

   url_content_host(host, sizeof(host), url);
   if (!host[0])
      return false;

   /* The port: after the host (and after "]" for IPv6). */
   auth = uc_authority(url);
   end  = uc_authority_end(auth);
   for (p = auth; p < end; p++)
      if (*p == '@')
         auth = p + 1;
   p = auth;
   if (*p == '[')
   {
      while (p < end && *p != ']')
         p++;
      if (p < end)
         p++;
   }
   else
      while (p < end && *p != ':')
         p++;
   if (p < end)
   {
      if (*p != ':')
         return false;
      for (p++; p < end; p++)
      {
         if (*p < '0' || *p > '9')
            return false;
         port     = port * 10 + (unsigned long)(*p - '0');
         has_port = true;
         if (port > 65535)
            return false;
      }
   }
   if (has_port && port == (scheme[4] == 's' ? 443UL : 80UL))
      has_port = false;

   if (has_port)
      n = snprintf(out, len, "%s://%s:%lu", scheme, host, port);
   else
      n = snprintf(out, len, "%s://%s", scheme, host);
   if (n < 0 || (size_t)n >= len)
   {
      out[0] = '\0';
      return false;
   }
   return true;
}

bool url_content_is_downloadable(const char *path)
{
   static const char *media_exts[] = {
      "mp4", "mkv", "avi", "webm", "flv", "ogm", "3gp", "3g2",
      "mp3", "ogg", "flac", "wav", "m3u8", NULL
   };
   const char *auth;
   const char *p;
   const char *end;
   const char *dot = NULL;
   char ext[16];
   size_t i, n;

   if (!path)
      return false;
   if (   !uc_starts_with_ci(path, "http://")
       && !uc_starts_with_ci(path, "https://"))
      return false;

   auth = uc_authority(path);
   p    = uc_authority_end(auth);
   end  = uc_path_end(p);

   for (; p < end; p++)
   {
      if (*p == '/')
         dot = NULL;
      else if (*p == '.')
         dot = p;
   }
   if (!dot)
      return true;

   n = (size_t)(end - dot - 1);
   if (n == 0 || n >= sizeof(ext))
      return true;
   for (i = 0; i < n; i++)
      ext[i] = uc_lower(dot[1 + i]);
   ext[n] = '\0';

   for (i = 0; media_exts[i]; i++)
      if (strcmp(ext, media_exts[i]) == 0)
         return false;
   return true;
}

/* ---------- link fixer ---------- */

static bool uc_is_drive_id_char(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
       || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/* Finds the value of a query parameter (exact name), or NULL */
static const char *uc_query_param(const char *url, const char *name)
{
   size_t      nlen = strlen(name);
   const char *q    = strchr(url, '?');
   const char *frag = strchr(url, '#');

   if (!q || (frag && frag < q))
      return NULL;
   q++;
   while (*q && *q != '#')
   {
      if (strncmp(q, name, nlen) == 0 && q[nlen] == '=')
         return q + nlen + 1;
      while (*q && *q != '&' && *q != '#')
         q++;
      if (*q == '&')
         q++;
   }
   return NULL;
}

static bool uc_fix_google_drive(char *out, size_t len,
      const char *url, const char *path)
{
   const char *id = NULL;
   size_t      n  = 0;

   if (strncmp(path, "/file/d/", 8) == 0)
      id = path + 8;
   else
      id = uc_query_param(url, "id");

   if (!id)
      return false;
   while (uc_is_drive_id_char(id[n]))
      n++;
   if (!n)
      return false;

   snprintf(out, len,
         "https://drive.usercontent.google.com/download?id=%.*s"
         "&export=download&confirm=t", (int)n, id);
   return true;
}

static void uc_fix_dropbox(char *out, size_t len, const char *url)
{
   const char *dl   = uc_query_param(url, "dl");
   const char *frag = strchr(url, '#');
   const char *q    = strchr(url, '?');
   size_t      pre;

   if (dl)
   {
      const char *rest = dl;
      while (*rest && *rest != '&' && *rest != '#')
         rest++;
      snprintf(out, len, "%.*s1%s", (int)(dl - url), url, rest);
      return;
   }

   if (q && frag && frag < q)
      q = NULL;
   pre = frag ? (size_t)(frag - url) : strlen(url);
   snprintf(out, len, "%.*s%s%s", (int)pre, url,
         q ? "&dl=1" : "?dl=1", frag ? frag : "");
}

static void uc_fix_onedrive(char *out, size_t len, const char *url)
{
   int   flen = 0;
   char *b64  = base64(url, (int)strlen(url), &flen);
   int   i, o;

   if (!b64)
   {
      strlcpy(out, url, len);
      return;
   }

   /* base64 -> base64url, no padding */
   for (i = 0, o = 0; i < flen; i++)
   {
      char c = b64[i];
      if (c == '=')
         break;
      if (c == '+')
         c = '-';
      else if (c == '/')
         c = '_';
      b64[o++] = c;
   }
   b64[o] = '\0';

   snprintf(out, len, "https://api.onedrive.com/v1.0/shares/u!%s/root/content",
         b64);
   free(b64);
}

bool url_content_split_fingerprint(char *url_out, size_t len,
      uint8_t sha256_out[32], bool *has_fp, const char *url)
{
   const char *hash;
   size_t      n;
   size_t      i;

   /* url_out may be url itself: it is only written at the end. */
   if (has_fp)
      *has_fp = false;
   if (!url_out || !len)
      return false;
   if (!url)
      goto fail;

   hash = strchr(url, '#');
   n    = hash ? (size_t)(hash - url) : strlen(url);
   if (n >= len)
      goto fail;

   if (     hash
         && !uc_starts_with_ci(hash + 1, "sha256=")
         && (     uc_find_ci(hash + 1, "sha256")
               || uc_find_ci(hash + 1, "sha-256")
               || uc_find_ci(hash + 1, "sha_256")))
      goto fail; /* a fingerprint in another form: fail closed (R8) */

   if (hash && uc_starts_with_ci(hash + 1, "sha256="))
   {
      const char *hex = hash + 1 + 7; /* "sha256=" */
      uint8_t     fp[32];
      for (i = 0; i < 64; i++)
         if (uc_hex_val(hex[i]) < 0)
            goto fail;       /* also stops at the end of the string */
      if (hex[64] != '\0')
         goto fail;
      for (i = 0; i < 32; i++)
         fp[i] = (uint8_t)((uc_hex_val(hex[2 * i]) << 4)
               | uc_hex_val(hex[2 * i + 1]));
      if (sha256_out)
         memcpy(sha256_out, fp, sizeof(fp));
      if (has_fp)
         *has_fp = true;
   }

   memmove(url_out, url, n);
   url_out[n] = '\0';
   return true;

fail:
   url_out[0] = '\0';
   return false;
}

void url_content_fix_link(char *out, size_t len, const char *url)
{
   char        buf[2048];
   char        host[UC_FIELD_LEN];
   const char *auth;
   const char *path;
   const char *s;
   size_t      n;

   if (!len)
      return;
   out[0] = '\0';
   if (!url)
      return;

   /* Trim surrounding whitespace (pasted links) */
   s = url;
   while (uc_is_space(*s))
      s++;
   n = strlen(s);
   while (n && uc_is_space(s[n - 1]))
      n--;
   uc_copy_n(buf, sizeof(buf), s, n);

   if (!(auth = uc_authority(buf)))
   {
      strlcpy(out, buf, len);
      return;
   }
   path = uc_authority_end(auth);
   url_content_host(host, sizeof(host), buf);

   if (   uc_host_is(host, "drive.google.com")
       || uc_host_is(host, "docs.google.com"))
   {
      if (uc_fix_google_drive(out, len, buf, path))
         return;
   }
   else if (uc_host_is(host, "dropbox.com"))
   {
      uc_fix_dropbox(out, len, buf);
      return;
   }
   else if (uc_host_is(host, "1drv.ms")
         || uc_host_is(host, "onedrive.live.com"))
   {
      uc_fix_onedrive(out, len, buf);
      return;
   }
   else if (uc_host_is(host, "archive.org"))
   {
      /* archive.org keys are only ever sent over https, so plain http
       * links are moved to https (archive.org redirects them anyway). */
      if (uc_starts_with_ci(buf, "http://"))
      {
         char tmp[2048];
         strlcpy(tmp, "https://", sizeof(tmp));
         strlcat(tmp, buf + 7, sizeof(tmp));
         strlcpy(buf, tmp, sizeof(buf));
         auth = uc_authority(buf);
         path = uc_authority_end(auth);
      }
      if (strncmp(path, "/details/", 9) == 0)
      {
         snprintf(out, len, "%.*s/download/%s",
               (int)(path - buf), buf, path + 9);
         return;
      }
   }

   strlcpy(out, buf, len);
}

/* ---------- file names ---------- */

static bool uc_is_bad_name_char(unsigned char c)
{
   return c < 0x20 || c == 0x7f || c == ':' || c == '*' || c == '?'
       || c == '"' || c == '<' || c == '>' || c == '|';
}

/* In place: collapses ".." runs to "." and drops trailing dots and
 * spaces (Windows cannot have them). */
static void uc_tidy_name(char *s)
{
   size_t i;
   size_t o = 0;
   for (i = 0; s[i]; i++)
   {
      if (s[i] == '.' && o && s[o - 1] == '.')
         continue;
      s[o++] = s[i];
   }
   while (o && (s[o - 1] == '.' || s[o - 1] == ' '))
      o--;
   s[o] = '\0';
}

/* Reduces name (in place) to a safe single file name */
static void uc_sanitize_name(char *name, size_t size)
{
   const char *base = name;
   const char *p;
   char        tmp[UC_FIELD_LEN];
   size_t      o = 0;

   for (p = name; *p; p++)
      if (*p == '/' || *p == '\\')
         base = p + 1;

   /* No leading dots or spaces (hidden files, "..") */
   while (*base == '.' || *base == ' ')
      base++;

   for (p = base; *p && o + 1 < sizeof(tmp); p++)
   {
      unsigned char c = (unsigned char)*p;
      if (c == '.' && o && tmp[o - 1] == '.')
         continue; /* collapse ".." */
      tmp[o++] = uc_is_bad_name_char(c) ? '_' : (char)c;
   }

   tmp[o] = '\0';
   uc_tidy_name(tmp);
   o = strlen(tmp);

   if (o > UC_NAME_MAX)
   {
      /* Shorten, keeping a short extension. The cut is moved back to
       * the start of a UTF-8 character, then the name is tidied again:
       * the cut can leave ".." or a trailing dot or space. */
      char  *dot  = strrchr(tmp, '.');
      size_t elen = dot ? strlen(dot) : 0;
      size_t keep;
      if (elen > 16)
         elen = 0;
      keep = UC_NAME_MAX - elen;
      while (keep && ((unsigned char)tmp[keep] & 0xC0) == 0x80)
         keep--;
      memmove(tmp + keep, tmp + o - elen, elen + 1);
      uc_tidy_name(tmp);
   }

   if (!tmp[0])
      strlcpy(tmp, "download.bin", sizeof(tmp));

   strlcpy(name, tmp, size);
}

void url_content_cache_name(char *out, size_t len,
      const char *fixed_url, const char *name_or_null)
{
   uint8_t digest[20];
   char    hex[41];
   char    name[UC_FIELD_LEN];

   if (!len)
      return;
   if (!fixed_url)
      fixed_url = "";

   SHA1Digest((const uint8_t*)fixed_url, strlen(fixed_url), digest);
   uc_hex(hex, digest, 8);

   name[0] = '\0';
   if (name_or_null && *name_or_null)
      strlcpy(name, name_or_null, sizeof(name));
   else
   {
      const char *auth = uc_authority(fixed_url);
      const char *p    = auth ? uc_authority_end(auth) : fixed_url;
      const char *end  = uc_path_end(p);
      const char *seg  = p;
      const char *q;

      for (q = p; q < end; q++)
         if (*q == '/')
            seg = q + 1;
      uc_pct_decode(name, sizeof(name), seg, (size_t)(end - seg));
   }

   uc_sanitize_name(name, sizeof(name));
   snprintf(out, len, "%s_%s", hex, name);
}

/* Finds "param=" in a header at a parameter boundary */
static const char *uc_find_header_param(const char *s, const char *param)
{
   const char *start = s;
   const char *p     = s;

   while ((p = uc_find_ci(p, param)))
   {
      char prev = (p == start) ? ';' : p[-1];
      if (prev == ';' || prev == ':' || uc_is_space(prev))
         return p + strlen(param);
      p++;
   }
   return NULL;
}

bool url_content_name_from_disposition(char *out, size_t len,
      const char *header_line)
{
   const char *v;
   size_t      n = 0;

   if (!len)
      return false;
   out[0] = '\0';
   if (!header_line)
      return false;

   if ((v = uc_find_header_param(header_line, "filename*=")))
   {
      /* RFC 5987: charset'lang'pct-encoded */
      const char *q1;
      const char *end = v;
      while (*end && *end != ';' && !uc_is_space(*end))
         end++;
      if (*v == '"' && end > v + 1 && end[-1] == '"')
      {
         v++;
         end--;
      }
      q1 = v;
      while (q1 < end && *q1 != '\'')
         q1++;
      if (q1 < end)
      {
         const char *q2 = q1 + 1;
         while (q2 < end && *q2 != '\'')
            q2++;
         if (q2 < end)
            v = q2 + 1;
      }
      uc_pct_decode(out, len, v, (size_t)(end - v));
      if (out[0])
         return true;
   }

   if ((v = uc_find_header_param(header_line, "filename=")))
   {
      if (*v == '"')
      {
         v++;
         while (*v && *v != '"' && n + 1 < len)
         {
            if (*v == '\\' && v[1])
               v++;
            out[n++] = *v++;
         }
         out[n] = '\0';
      }
      else
      {
         const char *end = v;
         while (*end && *end != ';' && !uc_is_space(*end))
            end++;
         uc_copy_n(out, len, v, (size_t)(end - v));
      }
   }

   return out[0] != '\0';
}

/* ---------- auth headers ---------- */

void url_content_basic_auth_header(char *out, size_t len,
      const char *user, const char *pass)
{
   char  creds[1024];
   int   n, flen = 0;
   char *b64;

   if (!len)
      return;
   out[0] = '\0';

   n = snprintf(creds, sizeof(creds), "%s:%s",
         user ? user : "", pass ? pass : "");
   if (n < 0)
      return;
   if ((size_t)n >= sizeof(creds))
      n = (int)sizeof(creds) - 1;

   if (!(b64 = base64(creds, n, &flen)))
      return;
   snprintf(out, len, "Authorization: Basic %s\r\n", b64);
   url_content_wipe(b64, (size_t)flen);
   url_content_wipe(creds, sizeof(creds));
   free(b64);
}

bool url_content_host_is_archive(const char *host)
{
   char   low[256];
   size_t i;
   if (!host || !*host)
      return false;
   strlcpy(low, host, sizeof(low));
   for (i = 0; low[i]; i++)
      if (low[i] >= 'A' && low[i] <= 'Z')
         low[i] = (char)(low[i] - 'A' + 'a');
   return uc_host_is(low, "archive.org");
}

void url_content_archive_auth_header(char *out, size_t len,
      const char *access, const char *secret)
{
   if (!len)
      return;
   snprintf(out, len, "Authorization: LOW %s:%s\r\n",
         access ? access : "", secret ? secret : "");
}

static void uc_md5_add(MD5_CTX *ctx, const char *s, bool colon)
{
   if (colon)
      MD5_Update(ctx, ":", 1);
   MD5_Update(ctx, s, (unsigned long)strlen(s));
}

static void uc_md5_done(MD5_CTX *ctx, char hex[33])
{
   unsigned char digest[16];
   MD5_Final(digest, ctx);
   uc_hex(hex, digest, 16);
}

/* Copies one challenge value (quoted or token) and returns the
 * position after it */
static const char *uc_digest_value(const char *p, char *out, size_t len)
{
   size_t n = 0;

   if (*p == '"')
   {
      p++;
      while (*p && *p != '"')
      {
         if (*p == '\\' && p[1])
            p++;
         if (n + 1 < len)
            out[n++] = *p;
         p++;
      }
      if (*p == '"')
         p++;
   }
   else
   {
      while (*p && *p != ',' && !uc_is_space(*p))
      {
         if (n + 1 < len)
            out[n++] = *p;
         p++;
      }
   }
   out[n] = '\0';
   return p;
}

/* True if the comma separated list contains the token "auth" */
static bool uc_qop_has_auth(const char *qop)
{
   const char *p = qop;
   while (*p)
   {
      const char *end;
      while (*p == ',' || uc_is_space(*p))
         p++;
      end = p;
      while (*end && *end != ',' && !uc_is_space(*end))
         end++;
      if (end - p == 4 && uc_starts_with_ci(p, "auth"))
         return true;
      p = end;
   }
   return false;
}

static void uc_make_cnonce(char *out, size_t len)
{
   static unsigned counter = 0;
   MD5_CTX         ctx;
   char            seed[128];
   char            hex[33];

   if (uc_test_cnonce[0])
   {
      strlcpy(out, uc_test_cnonce, len);
      return;
   }

   snprintf(seed, sizeof(seed), "%lu-%lu-%u-%d-%p",
         (unsigned long)time(NULL), (unsigned long)clock(),
         ++counter, rand(), (void*)&ctx);
   MD5_Init(&ctx);
   uc_md5_add(&ctx, seed, false);
   uc_md5_done(&ctx, hex);
   hex[16] = '\0';
   strlcpy(out, hex, len);
}

void url_content_set_test_cnonce(const char *cnonce)
{
   if (cnonce)
      strlcpy(uc_test_cnonce, cnonce, sizeof(uc_test_cnonce));
   else
      uc_test_cnonce[0] = '\0';
}

bool url_content_digest_auth_header(char *out, size_t len,
      const char *www_authenticate, const char *method, const char *uri,
      const char *user, const char *pass)
{
   char        realm[UC_FIELD_LEN];
   char        nonce[UC_FIELD_LEN];
   char        opaque[UC_FIELD_LEN];
   char        qop[UC_FIELD_LEN];
   char        algo[64];
   char        cnonce[64];
   char        ha1[33];
   char        ha2[33];
   char        resp[33];
   char        extra[UC_FIELD_LEN * 2];
   const char *nc        = "00000001";
   const char *p;
   bool        have_nonce = false;
   bool        use_qop    = false;
   bool        sess       = false;
   MD5_CTX     ctx;
   int         n;

   if (!len)
      return false;
   out[0] = '\0';
   if (!www_authenticate || !method || !uri)
      return false;
   if (!user)
      user = "";
   if (!pass)
      pass = "";

   realm[0] = nonce[0] = opaque[0] = qop[0] = algo[0] = '\0';

   p = www_authenticate;
   while (uc_is_space(*p))
      p++;
   if (uc_starts_with_ci(p, "www-authenticate:"))
      p += 17;
   while (uc_is_space(*p))
      p++;
   if (!uc_starts_with_ci(p, "digest") || !uc_is_space(p[6]))
      return false;
   p += 6;

   for (;;)
   {
      const char *name;
      size_t      nlen;
      char        dummy[UC_FIELD_LEN];

      while (*p == ',' || uc_is_space(*p))
         p++;
      if (!*p)
         break;

      name = p;
      while (*p && *p != '=' && *p != ',' && !uc_is_space(*p))
         p++;
      nlen = (size_t)(p - name);
      while (uc_is_space(*p))
         p++;
      if (*p != '=')
         continue; /* bare token, ignore */
      p++;
      while (uc_is_space(*p))
         p++;

#define UC_IS_PARAM(s) (nlen == sizeof(s) - 1 && uc_starts_with_ci(name, s))
      if (UC_IS_PARAM("realm"))
         p = uc_digest_value(p, realm, sizeof(realm));
      else if (UC_IS_PARAM("nonce"))
      {
         p          = uc_digest_value(p, nonce, sizeof(nonce));
         have_nonce = true;
      }
      else if (UC_IS_PARAM("opaque"))
         p = uc_digest_value(p, opaque, sizeof(opaque));
      else if (UC_IS_PARAM("qop"))
         p = uc_digest_value(p, qop, sizeof(qop));
      else if (UC_IS_PARAM("algorithm"))
         p = uc_digest_value(p, algo, sizeof(algo));
      else
         p = uc_digest_value(p, dummy, sizeof(dummy));
#undef UC_IS_PARAM
   }

   if (!have_nonce || !nonce[0])
      return false;

   if (algo[0])
   {
      if (uc_starts_with_ci(algo, "md5-sess") && !algo[8])
         sess = true;
      else if (!(uc_starts_with_ci(algo, "md5") && !algo[3]))
         return false;
   }

   if (qop[0])
   {
      if (!uc_qop_has_auth(qop))
         return false; /* auth-int only: not supported */
      use_qop = true;
   }

   uc_make_cnonce(cnonce, sizeof(cnonce));

   MD5_Init(&ctx);
   uc_md5_add(&ctx, user,  false);
   uc_md5_add(&ctx, realm, true);
   uc_md5_add(&ctx, pass,  true);
   uc_md5_done(&ctx, ha1);

   if (sess)
   {
      MD5_Init(&ctx);
      uc_md5_add(&ctx, ha1,    false);
      uc_md5_add(&ctx, nonce,  true);
      uc_md5_add(&ctx, cnonce, true);
      uc_md5_done(&ctx, ha1);
   }

   MD5_Init(&ctx);
   uc_md5_add(&ctx, method, false);
   uc_md5_add(&ctx, uri,    true);
   uc_md5_done(&ctx, ha2);

   MD5_Init(&ctx);
   uc_md5_add(&ctx, ha1,   false);
   uc_md5_add(&ctx, nonce, true);
   if (use_qop)
   {
      uc_md5_add(&ctx, nc,     true);
      uc_md5_add(&ctx, cnonce, true);
      uc_md5_add(&ctx, "auth", true);
   }
   uc_md5_add(&ctx, ha2, true);
   uc_md5_done(&ctx, resp);

   extra[0] = '\0';
   if (use_qop)
      snprintf(extra, sizeof(extra),
            "qop=auth, nc=%s, cnonce=\"%s\", ", nc, cnonce);
   else if (sess)
      snprintf(extra, sizeof(extra), "cnonce=\"%s\", ", cnonce);

   n = snprintf(out, len,
         "Authorization: Digest username=\"%s\", realm=\"%s\", "
         "nonce=\"%s\", uri=\"%s\", %s%s%s%sresponse=\"%s\"%s%s%s\r\n",
         user, realm, nonce, uri,
         algo[0] ? "algorithm=" : "", algo, algo[0] ? ", " : "",
         extra, resp,
         opaque[0] ? ", opaque=\"" : "", opaque, opaque[0] ? "\"" : "");

   if (n < 0 || (size_t)n >= len)
   {
      out[0] = '\0';
      return false;
   }
   return true;
}

/* ---------- content sniffing ---------- */

bool url_content_looks_like_html(const uint8_t *data, size_t len)
{
   size_t i = 0;

   if (!data)
      return false;
   if (len > 512)
      len = 512;

   if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
      i = 3;
   while (i < len && uc_is_space((char)data[i]))
      i++;

   if (len - i >= 14)
   {
      static const char doctype[] = "<!doctype html";
      size_t k;
      for (k = 0; k < 14; k++)
         if (uc_lower((char)data[i + k]) != doctype[k])
            break;
      if (k == 14)
         return true;
   }
   if (len - i >= 5)
   {
      static const char html[] = "<html";
      size_t k;
      for (k = 0; k < 5; k++)
         if (uc_lower((char)data[i + k]) != html[k])
            break;
      if (k == 5)
         return true;
   }
   return false;
}

/* Case-insensitive substring test */
static bool uc_contains_ci(const char *s, const char *needle)
{
   for (; *s; s++)
      if (uc_starts_with_ci(s, needle))
         return true;
   return false;
}

bool url_content_cli_should_fetch(bool first_pass, const char *content,
      const char *core_path, bool builtin_player)
{
   const char *name;
   const char *p;

   if (!first_pass || !url_content_is_downloadable(content))
      return false;
   if (!core_path || !*core_path)
      return !builtin_player;

   name = core_path;
   for (p = core_path; *p; p++)
      if (*p == '/' || *p == '\\')
         name = p + 1;
   return !uc_contains_ci(name, "ffmpeg") && !uc_contains_ci(name, "mpv");
}

/* ---------- form encoding ---------- */

bool url_content_form_encode(char *out, size_t len, const char *s)
{
   static const char hex[] = "0123456789ABCDEF";
   size_t o = 0;

   if (!out || !len)
      return false;
   out[0] = '\0';
   if (!s)
      return true;

   for (; *s; s++)
   {
      unsigned char c = (unsigned char)*s;
      bool plain      = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
         || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
         || c == '~';
      size_t need     = (plain || c == ' ') ? 1 : 3;

      if (o + need >= len)
      {
         out[0] = '\0';
         return false;
      }
      if (plain)
         out[o++] = (char)c;
      else if (c == ' ')
         out[o++] = '+';
      else
      {
         out[o++] = '%';
         out[o++] = hex[c >> 4];
         out[o++] = hex[c & 15];
      }
   }
   out[o] = '\0';
   return true;
}

/* ---------- archive.org xauthn reply ---------- */

#define UC_JSON_DEPTH 8
#define UC_JSON_KEY   32

/* True if the member path of the current value is exactly a/b/c
 * (NULL ends the path early). */
static bool uc_json_path(char keys[][UC_JSON_KEY], unsigned depth,
      const char *a, const char *b, const char *c)
{
   const char *want[3];
   unsigned n = 0;
   unsigned i;

   want[0] = a;
   want[1] = b;
   want[2] = c;
   while (n < 3 && want[n])
      n++;
   if (depth != n)
      return false;
   for (i = 0; i < n; i++)
      if (strcmp(keys[i + 1], want[i]) != 0)
         return false;
   return true;
}

bool url_content_parse_archive_login(const char *json, size_t json_len,
      char *access, size_t alen, char *secret, size_t slen,
      char *err, size_t elen)
{
   /* keys[d] = member name of the current value at object depth d;
    * "[" marks an array level, so nothing below it can match. */
   char keys[UC_JSON_DEPTH + 1][UC_JSON_KEY];
   char reason[128];
   char error[128];
   rjson_t *rj;
   int success     = -1;   /* -1 unseen, 0 false, 1 true */
   bool broken     = false;
   bool too_long   = false;
   bool got_access = false;
   bool got_secret = false;

   if (access && alen)
      access[0] = '\0';
   if (secret && slen)
      secret[0] = '\0';
   if (err && elen)
      err[0] = '\0';
   reason[0] = '\0';
   error[0]  = '\0';
   memset(keys, 0, sizeof(keys));

   if (!json || !json_len || !access || !alen || !secret || !slen
         || !(rj = rjson_open_buffer(json, json_len)))
   {
      if (err && elen)
         strlcpy(err, "unexpected reply", elen);
      return false;
   }

   for (;;)
   {
      enum rjson_type t = rjson_next(rj);
      unsigned depth;
      bool in_object;
      bool is_name;

      if (t == RJSON_DONE)
         break;
      if (t == RJSON_ERROR)
      {
         broken = true;
         break;
      }

      depth     = rjson_get_context_depth(rj);
      in_object = rjson_get_context_type(rj) == RJSON_OBJECT;
      is_name   = in_object && t == RJSON_STRING
         && (rjson_get_context_count(rj) & 1);

      if (t == RJSON_OBJECT || t == RJSON_ARRAY)
      {
         /* depth is now the new level; values of the parent were
          * named by keys[depth - 1]. */
         if (depth <= UC_JSON_DEPTH)
         {
            if (t == RJSON_ARRAY)
               strlcpy(keys[depth], "[", UC_JSON_KEY);
            else
               keys[depth][0] = '\0';
         }
         continue;
      }
      if (depth == 0 || depth > UC_JSON_DEPTH)
         continue;
      if (is_name)
      {
         const char *name = rjson_get_string(rj, NULL);
         if (strlcpy(keys[depth], name, UC_JSON_KEY) >= UC_JSON_KEY)
            strlcpy(keys[depth], "?", UC_JSON_KEY);
         continue;
      }
      if (!in_object)
         continue;

      /* A value; its parents must all be objects (no "[" on the path). */
      if (t == RJSON_TRUE || t == RJSON_FALSE)
      {
         if (uc_json_path(keys, depth, "success", NULL, NULL))
            success = (t == RJSON_TRUE) ? 1 : 0;
      }
      else if (t == RJSON_STRING)
      {
         const char *v = rjson_get_string(rj, NULL);
         if (uc_json_path(keys, depth, "values", "s3", "access"))
         {
            if (strlcpy(access, v, alen) >= alen)
               too_long = true;
            got_access = access[0] != '\0';
         }
         else if (uc_json_path(keys, depth, "values", "s3", "secret"))
         {
            if (strlcpy(secret, v, slen) >= slen)
               too_long = true;
            got_secret = secret[0] != '\0';
         }
         else if (uc_json_path(keys, depth, "values", "reason", NULL))
            strlcpy(reason, v, sizeof(reason));
         else if (uc_json_path(keys, depth, "error", NULL, NULL))
            strlcpy(error, v, sizeof(error));
      }
   }
   rjson_free(rj);

   if (!broken && success == 1 && got_access && got_secret && !too_long)
      return true;

   url_content_wipe(access, alen);
   url_content_wipe(secret, slen);
   if (err && elen)
   {
      if (broken || success == -1)
         strlcpy(err, "unexpected reply", elen);
      else if (success == 1)
         strlcpy(err, too_long ? "keys too long" : "no keys in the reply",
               elen);
      else if (strcmp(reason, "account_bad_password") == 0)
         strlcpy(err, "wrong email or password", elen);
      else if (strcmp(reason, "account_not_found") == 0)
         strlcpy(err, "no account with that email", elen);
      else if (reason[0])
         strlcpy(err, reason, elen);
      else if (error[0])
         strlcpy(err, error, elen);
      else
         strlcpy(err, "sign-in refused", elen);
   }
   return false;
}

#endif /* HAVE_NETWORKING */
