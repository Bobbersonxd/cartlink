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
#include <formats/rjson.h>

#include "url_content.h"
#include "url_catalog.h"

/* ---------- small helpers ---------- */

static char cat_lower(char c)
{
   if (c >= 'A' && c <= 'Z')
      return (char)(c - 'A' + 'a');
   return c;
}

/* First character after "scheme://authority": '/', '?', '#' or '\0';
 * NULL if url has no "://". */
static const char *cat_path(const char *url)
{
   const char *p = strstr(url, "://");
   if (!p)
      return NULL;
   p += 3;
   while (*p && *p != '/' && *p != '?' && *p != '#')
      p++;
   return p;
}

static bool cat_has_ctrl(const char *s)
{
   for (; *s; s++)
      if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f)
         return true;
   return false;
}

/* Case-insensitive substring test */
static bool cat_contains_ci(const char *s, const char *needle)
{
   size_t n = strlen(needle);
   for (; *s; s++)
   {
      size_t i;
      for (i = 0; i < n && s[i] && cat_lower(s[i]) == cat_lower(needle[i]); i++);
      if (i == n)
         return true;
   }
   return false;
}

/* Removes "." and ".." path segments of an absolute URL in place
 * (RFC 3986 5.2.4); ".." at the root stays at the root. */
static bool cat_remove_dots(char *url, size_t len)
{
   char buf[URL_CATALOG_URL_LEN];
   char *path = (char*)cat_path(url);
   const char *end;
   const char *p;
   size_t o = 0;
   size_t prefix;

   if (!path || *path != '/')
      return true;
   end = path + strcspn(path, "?#");
   for (p = path; p < end; )
   {
      const char *s = p + 1;
      const char *e = s;
      size_t n;
      while (e < end && *e != '/')
         e++;
      n = (size_t)(e - s);
      if (n == 2 && s[0] == '.' && s[1] == '.')
      {
         while (o > 0 && buf[o - 1] != '/')
            o--;
         if (o > 0)
            o--;
         if (e == end)
            buf[o++] = '/';
      }
      else if (n == 1 && s[0] == '.')
      {
         if (e == end)
            buf[o++] = '/';
      }
      else
      {
         if (o + 1 + n >= sizeof(buf))
            return false;
         buf[o++] = '/';
         memcpy(buf + o, s, n);
         o += n;
      }
      p = e;
   }
   if (!o)
      buf[o++] = '/';
   buf[o] = '\0';

   prefix = (size_t)(path - url);
   if (prefix + o + strlen(end) + 1 > len)
      return false;
   memmove(path + o, end, strlen(end) + 1);
   memcpy(path, buf, o);
   return true;
}

/* Builds the entry URL in out: ref resolved against base, dot segments
 * removed, same scheme + host + port as base and strictly below base's
 * folder. False means the entry is unsafe and must be skipped. */
static bool cat_entry_url(char *out, size_t len, const char *base,
      const char *ref)
{
   const char *bpath = cat_path(base);
   const char *opath;
   const char *bdir_end;
   const char *q;
   size_t site;
   size_t dir;

   if (!bpath || !*ref || cat_has_ctrl(ref) || strchr(ref, '\\')
         /* encoded dots, slashes and backslashes could climb out of
          * the folder once the server decodes them */
         || cat_contains_ci(ref, "%2e") || cat_contains_ci(ref, "%2f")
         || cat_contains_ci(ref, "%5c"))
      return false;
   if (!url_content_resolve(out, len, base, ref)
         || !cat_remove_dots(out, len))
      return false;
   if (!(opath = cat_path(out)))
      return false;

   /* scheme + authority must match (ASCII case-insensitive) */
   site = (size_t)(bpath - base);
   if ((size_t)(opath - out) != site)
      return false;
   for (q = base; q < bpath; q++)
      if (cat_lower(*q) != cat_lower(out[q - base]))
         return false;

   /* folder of base: up to and including the last '/' of its path */
   bdir_end = bpath;
   for (q = bpath; *q && *q != '?' && *q != '#'; q++)
      if (*q == '/')
         bdir_end = q + 1;
   dir = (size_t)(bdir_end - base);
   if (dir == site)
   {
      /* "https://h" has the root folder "/" */
      if (*opath != '/')
         return false;
      dir = site + 1;
   }
   else if (strncmp(out + site, base + site, dir - site) != 0)
      return false;
   /* something must follow the folder: never the folder itself */
   return out[dir] && out[dir] != '?' && out[dir] != '#';
}

/* Copies a JSON name into out, cut on a UTF-8 boundary. False for
 * names that cannot be shown or used safely. */
static bool cat_entry_name(char *out, size_t len, const char *name)
{
   size_t n;
   if (     !*name
         || !strcmp(name, ".") || !strcmp(name, "..")
         || strchr(name, '/') || strchr(name, '\\')
         || cat_has_ctrl(name))
      return false;
   n = strlen(name);
   if (n >= len)
   {
      n = len - 1;
      /* back off continuation bytes (10xxxxxx) to a character start */
      while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80)
         n--;
   }
   memcpy(out, name, n);
   out[n] = '\0';
   return true;
}

/* Non-negative JSON integer; anything else (fraction, exponent,
 * negative, too big) is 0. */
static uint64_t cat_size(const char *s)
{
   uint64_t v = 0;
   if (*s < '0' || *s > '9')
      return 0;
   for (; *s >= '0' && *s <= '9'; s++)
   {
      uint64_t d = (uint64_t)(*s - '0');
      if (v > (((uint64_t)-1) - d) / 10)
         return 0;
      v = v * 10 + d;
   }
   return *s ? 0 : v;
}

static int cat_compare(const void *a, const void *b)
{
   const struct url_catalog_entry *x = (const struct url_catalog_entry*)a;
   const struct url_catalog_entry *y = (const struct url_catalog_entry*)b;
   const unsigned char *p = (const unsigned char*)x->name;
   const unsigned char *q = (const unsigned char*)y->name;

   if (x->is_dir != y->is_dir)
      return x->is_dir ? -1 : 1;
   for (; *p && cat_lower((char)*p) == cat_lower((char)*q); p++, q++);
   if (*p || *q)
   {
      unsigned char lp = (unsigned char)cat_lower((char)*p);
      unsigned char lq = (unsigned char)cat_lower((char)*q);
      if (lp != lq)
         return lp < lq ? -1 : 1;
   }
   return strcmp(x->name, y->name);
}

/* ---------- parser ---------- */

/* One listing element being read. */
struct cat_elem
{
   char name[URL_CATALOG_NAME_LEN * 2];
   char url[URL_CATALOG_URL_LEN];
   uint64_t size;
   bool is_dir;
   bool has_name, has_dir, has_url;
   bool bad;      /* a known member had the wrong type */
   bool long_url; /* url did not fit: skip the entry */
};

/* Adds e if it is safe (unsafe entries are skipped). False only when
 * out of memory. */
static bool cat_add(struct url_catalog *c, size_t *cap,
      const struct cat_elem *e)
{
   struct url_catalog_entry ent;

   if (     e->long_url
         || !cat_entry_name(ent.name, sizeof(ent.name), e->name)
         || !cat_entry_url(ent.url, sizeof(ent.url), c->base, e->url))
      return true;
   if (c->count >= URL_CATALOG_MAX)
   {
      c->truncated = true;
      return true;
   }
   if (c->count == *cap)
   {
      size_t n = *cap ? *cap * 2 : 64;
      struct url_catalog_entry *grown;
      if (n > URL_CATALOG_MAX)
         n = URL_CATALOG_MAX;
      if (!(grown = (struct url_catalog_entry*)
               realloc(c->entries, n * sizeof(*grown))))
         return false;
      c->entries = grown;
      *cap       = n;
   }
   ent.is_dir             = e->is_dir;
   ent.size               = e->size;
   c->entries[c->count++] = ent;
   return true;
}

bool url_catalog_parse(const char *json, size_t len, const char *base_url,
      struct url_catalog *out)
{
   struct cat_elem e;
   rjson_t *rj;
   size_t cap     = 0;
   unsigned depth = 0;    /* 1 = the listing array, 2 = an element */
   bool want_key  = true; /* at depth 2: next string is a member name */
   int key        = 0;    /* 1 name, 2 is_dir, 3 url, 4 size, 0 other */
   bool closed    = false; /* the listing array has ended */
   bool ok        = false;

   if (!out)
      return false;
   memset(out, 0, sizeof(*out));
   if (!json || !len || !base_url || !cat_path(base_url)
         || strlcpy(out->base, base_url, sizeof(out->base))
            >= sizeof(out->base)
         || !(rj = rjson_open_buffer(json, len)))
   {
      out->base[0] = '\0';
      return false;
   }

   for (;;)
   {
      enum rjson_type t = rjson_next(rj);

      if (t == RJSON_ERROR)
         break;
      if (t == RJSON_DONE)
      {
         ok = closed;
         break;
      }

      if (depth == 0)
      {
         /* the document must be one array */
         if (t != RJSON_ARRAY || closed)
            break;
         depth = 1;
         continue;
      }

      if (depth == 1)
      {
         if (t == RJSON_ARRAY_END)
         {
            depth  = 0;
            closed = true;
            continue;
         }
         if (t != RJSON_OBJECT)
            break; /* [1,2], [[...]] */
         memset(&e, 0, sizeof(e));
         depth    = 2;
         want_key = true;
         continue;
      }

      if (depth > 2)
      {
         /* skipping a nested value of an unknown member */
         if (t == RJSON_OBJECT || t == RJSON_ARRAY)
            depth++;
         else if (t == RJSON_OBJECT_END || t == RJSON_ARRAY_END)
         {
            if (--depth == 2)
               want_key = true;
         }
         continue;
      }

      /* depth 2: inside one element */
      if (t == RJSON_OBJECT_END)
      {
         if (!e.has_name || !e.has_dir || !e.has_url || e.bad)
            break;
         if (!cat_add(out, &cap, &e))
            break;
         depth = 1;
         continue;
      }
      if (want_key)
      {
         const char *k = rjson_get_string(rj, NULL);
         key      = !strcmp(k, "name")   ? 1
                  : !strcmp(k, "is_dir") ? 2
                  : !strcmp(k, "url")    ? 3
                  : !strcmp(k, "size")   ? 4 : 0;
         want_key = false;
         continue;
      }
      want_key = true;
      if (t == RJSON_OBJECT || t == RJSON_ARRAY)
      {
         if (key >= 1 && key <= 3)
            e.bad = true;
         depth    = 3;
         want_key = false;
         continue;
      }
      switch (key)
      {
         case 1:
            /* cut here, cut again on a UTF-8 boundary by cat_add */
            if (t != RJSON_STRING)
               e.bad = true;
            else
               strlcpy(e.name, rjson_get_string(rj, NULL), sizeof(e.name));
            e.has_name = true;
            break;
         case 2:
            if (t != RJSON_TRUE && t != RJSON_FALSE)
               e.bad = true;
            e.is_dir  = t == RJSON_TRUE;
            e.has_dir = true;
            break;
         case 3:
            if (t != RJSON_STRING)
               e.bad = true;
            else if (strlcpy(e.url, rjson_get_string(rj, NULL),
                     sizeof(e.url)) >= sizeof(e.url))
               e.long_url = true;
            e.has_url = true;
            break;
         case 4:
            e.size = t == RJSON_NUMBER
               ? cat_size(rjson_get_string(rj, NULL)) : 0;
            break;
         default:
            break;
      }
   }
   rjson_free(rj);

   if (!ok)
   {
      url_catalog_free(out);
      return false;
   }
   if (out->count > 1)
      qsort(out->entries, out->count, sizeof(*out->entries), cat_compare);
   return true;
}

void url_catalog_free(struct url_catalog *c)
{
   if (!c)
      return;
   free(c->entries);
   c->entries   = NULL;
   c->count     = 0;
   c->truncated = false;
   c->base[0]   = '\0';
}

bool url_catalog_parent(char *out, size_t len, const char *url)
{
   const char *path = url ? cat_path(url) : NULL;
   const char *end;
   const char *slash = NULL;
   const char *p;
   size_t n;

   if (!path || *path != '/')
      return false;
   end = path + strcspn(path, "?#");
   if (end - path <= 1)
      return false; /* server root */
   if (end[-1] == '/')
      end--;
   for (p = path; p < end; p++)
      if (*p == '/')
         slash = p;
   n = (size_t)(slash + 1 - url);
   if (n + 1 > len)
      return false;
   memcpy(out, url, n);
   out[n] = '\0';
   return true;
}

#endif /* HAVE_NETWORKING */
