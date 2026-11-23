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

#ifdef _WIN32
/* Saved values are sealed with DPAPI for the current Windows user. */
#define URL_LOGINS_DPAPI
#include <windows.h>
#include <wincrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif
#endif

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) \
   || defined(__OpenBSD__) || defined(__NetBSD__)
#define URL_LOGINS_POSIX_MODE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <compat/fopen_utf8.h>
#include <compat/strl.h>
#include <encodings/base64.h>
#include <file/config_file.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "url_content.h"
#include "url_logins.h"

#define URL_LOGINS_KEY_LEN 320
#define URL_LOGINS_SITE_LEN 300
/* First line of a file whose values are DPAPI-sealed (Windows). */
#define URL_LOGINS_MARKER "# cartlink-logins v2 dpapi"

static char url_logins_path[4096] = {0};

#ifdef URL_LOGINS_DPAPI
static void url_logins_seal_existing(void);
#endif

void url_logins_set_path(const char *path)
{
   if (path)
      strlcpy(url_logins_path, path, sizeof(url_logins_path));
   else
      url_logins_path[0] = '\0';
#ifdef URL_LOGINS_DPAPI
   /* A plain file from an older build is sealed right away, not only
    * at the next save (called on the main thread, at config load). */
   url_logins_seal_existing();
#endif
}

bool url_logins_base64_ok(const char *s)
{
   size_t i;
   size_t n;
   size_t pad = 0;
   if (!s)
      return false;
   n = strlen(s);
   if (n < 4 || (n % 4) != 0)
      return false;
   if (s[n - 1] == '=')
      pad = (s[n - 2] == '=') ? 2 : 1;
   for (i = 0; i < n - pad; i++)
   {
      char c = s[i];
      if (!(     (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
               || (c >= '0' && c <= '9') || c == '+' || c == '/'))
         return false;
   }
   return true;
}

/* The site of url ("https://host[:port]", see url_content_origin).
 * Logins are only ever sent (so only kept) for https sites. */
static bool url_logins_site(char *out, size_t len, const char *url)
{
   if (!url_content_origin(out, len, url))
      return false;
   /* https only, and only characters a config_file key can hold. */
   if (strncmp(out, "https://", 8) != 0
         || out[strspn(out, "abcdefghijklmnopqrstuvwxyz0123456789"
               "-._~%:/[]")] != '\0')
   {
      out[0] = '\0';
      return false;
   }
   return true;
}

/* "url_login_<site>_<field>". */
static bool url_logins_site_key(char *out, size_t len,
      const char *site, const char *field)
{
   int n = snprintf(out, len, "url_login_%s_%s", site, field);
   return n > 0 && (size_t)n < len;
}

/* Key used by older builds for the site: "url_login_<host>_<field>",
 * host lower-cased and every character other than [a-z0-9-] turned
 * into '_', no scheme, no port. Such an entry is read as
 * https://<host> (default port) only. False if the site has a port. */
static bool url_logins_legacy_key(char *out, size_t len,
      const char *site, const char *field)
{
   size_t i;
   size_t n;
   const char *host = site + STRLEN_CONST("https://");

   if (     strncmp(site, "https://", 8) != 0
         || string_is_empty(host))
      return false;
   /* An IPv6 host ends in ']'; any ':' after that is a port. */
   {
      const char *after = host;
      if (*host == '[' && !(after = strchr(host, ']')))
         return false;
      if (strchr(after, ':'))
         return false;
   }

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

/* True if the file at path starts with URL_LOGINS_MARKER. */
static bool url_logins_has_marker(const char *path)
{
   char head[sizeof(URL_LOGINS_MARKER)];
   int64_t got;
   RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!f)
      return false;
   got = filestream_read(f, head, (int64_t)(sizeof(head) - 1));
   filestream_close(f);
   return got == (int64_t)(sizeof(head) - 1)
      && memcmp(head, URL_LOGINS_MARKER, sizeof(head) - 1) == 0;
}

/* Existing file, or an empty config if there is none yet. NULL if the
 * file exists but cannot be read (never clobber it). *sealed: the file
 * has the DPAPI marker (its values are sealed). */
static config_file_t *url_logins_open(bool create, bool *sealed)
{
   char        tmp[sizeof(url_logins_path) + 8];
   char        bak[sizeof(url_logins_path) + 8];
   const char *src = url_logins_path;

   *sealed = false;
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
   *sealed = url_logins_has_marker(src);
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

#ifdef URL_LOGINS_DPAPI
/* DPAPI-sealed (current Windows user) and base64-coded copy of plain,
 * malloc'd. NULL on failure. */
static char *url_logins_seal(const char *plain)
{
   DATA_BLOB in;
   DATA_BLOB out;
   char *coded = NULL;
   int coded_len = 0;

   in.pbData = (BYTE*)plain;
   in.cbData = (DWORD)strlen(plain);
   out.pbData = NULL;
   out.cbData = 0;
   if (!CryptProtectData(&in, L"Cartlink login", NULL, NULL, NULL,
            CRYPTPROTECT_UI_FORBIDDEN, &out))
      return NULL;
   coded = base64(out.pbData, (int)out.cbData, &coded_len);
   LocalFree(out.pbData);
   return coded;
}

/* Opens a value sealed by url_logins_seal into out. False if it is not
 * one, was sealed by another user/PC, or does not fit. */
static bool url_logins_unseal(const char *stored, char *out, size_t len)
{
   DATA_BLOB in;
   DATA_BLOB plain;
   int raw_len = 0;
   bool ok     = false;
   unsigned char *raw;

   /* libretro's unbase64 trusts its input: a damaged value (wrong
    * length or padding) would make it write past its buffer. */
   if (     !url_logins_base64_ok(stored)
         || strlen(stored) > 65536
         || !(raw = unbase64(stored, (int)strlen(stored), &raw_len)))
      return false;
   in.pbData    = raw;
   in.cbData    = (DWORD)raw_len;
   plain.pbData = NULL;
   plain.cbData = 0;
   if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
            CRYPTPROTECT_UI_FORBIDDEN, &plain))
   {
      if (plain.cbData < len && !memchr(plain.pbData, '\0', plain.cbData))
      {
         memcpy(out, plain.pbData, plain.cbData);
         out[plain.cbData] = '\0';
         ok = true;
      }
      SecureZeroMemory(plain.pbData, plain.cbData);
      LocalFree(plain.pbData);
   }
   free(raw);
   return ok;
}

/* Seals every value of a plain (older or new) file in place, so the
 * whole file is written sealed. */
static bool url_logins_seal_all(config_file_t *conf)
{
   struct config_entry_list *e;
   for (e = conf->entries; e; e = e->next)
   {
      char *sealed;
      if (!e->key || !e->value)
         continue;
      if (!(sealed = url_logins_seal(e->value)))
         return false;
      url_content_wipe(e->value, strlen(e->value));
      free(e->value);
      e->value     = sealed;
      conf->flags |= CONF_FILE_FLG_MODIFIED;
   }
   return true;
}
#endif

/* Reads one value into out (unsealed on Windows). */
static bool url_logins_read(config_file_t *conf, bool sealed,
      const char *key, char *out, size_t len)
{
   if (!sealed)
      return config_get_array(conf, key, out, len);
#ifdef URL_LOGINS_DPAPI
   {
      const char *stored = NULL;
      struct config_entry_list *e = config_get_entry(conf, key);
      if (e)
         stored = e->value;
      return url_logins_unseal(stored, out, len);
   }
#else
   /* Sealed on Windows: unreadable here. */
   return false;
#endif
}

/* Stores one value (sealed on Windows). */
static bool url_logins_store(config_file_t *conf, const char *key,
      const char *value)
{
#ifdef URL_LOGINS_DPAPI
   char *sealed = url_logins_seal(value);
   if (!sealed)
      return false;
   config_set_string(conf, key, sealed);
   free(sealed);
#else
   config_set_string(conf, key, value);
#endif
   return true;
}

/* Writes conf to f: the DPAPI marker first on Windows. */
static bool url_logins_dump(config_file_t *conf, const char *path)
{
   bool ok;
   FILE *f = (FILE*)fopen_utf8(path, "wb");
   if (!f)
      return false;
#ifdef URL_LOGINS_DPAPI
   fputs(URL_LOGINS_MARKER "\n", f);
#endif
   config_file_dump(conf, f, true);
   ok = !ferror(f);
   if (fclose(f) != 0)
      ok = false;
   return ok;
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
         || !url_logins_dump(conf, tmp)
         || !path_is_valid(tmp))
   {
      filestream_delete(tmp);
      return false;
   }
   return url_logins_replace(tmp, bak);
}

/* The file opened for a change: on Windows a plain file is sealed
 * first (it is written sealed). Elsewhere a sealed file (moved from
 * Windows) is never changed. NULL on failure. */
static config_file_t *url_logins_open_for_write(void)
{
   bool sealed;
   config_file_t *conf = url_logins_open(true, &sealed);
   if (!conf)
      return NULL;
#ifdef URL_LOGINS_DPAPI
   if (!sealed && !url_logins_seal_all(conf))
   {
      config_file_free(conf);
      return NULL;
   }
#else
   if (sealed)
   {
      config_file_free(conf);
      return NULL;
   }
#endif
   return conf;
}

#ifdef URL_LOGINS_DPAPI
/* Seals a plain logins file in place (no-op if missing, already
 * sealed or unreadable). The plain copy is replaced through .tmp and
 * a rename; no plain .tmp or .bak is left after it works. */
static void url_logins_seal_existing(void)
{
   bool sealed;
   config_file_t *conf;
   if (string_is_empty(url_logins_path))
      return;
   if (!(conf = url_logins_open(false, &sealed)))
      return;
   if (!sealed && url_logins_seal_all(conf))
      url_logins_write(conf);
   config_file_free(conf);
}
#endif

static bool url_logins_get_pair(const char *key_a, char *a, size_t alen,
      const char *key_b, char *b, size_t blen)
{
   bool ok;
   bool sealed;
   config_file_t *conf = url_logins_open(false, &sealed);
   if (!conf)
      return false;
   ok = url_logins_read(conf, sealed, key_a, a, alen)
     && url_logins_read(conf, sealed, key_b, b, blen);
   config_file_free(conf);
   if (!ok)
   {
      url_content_wipe(a, alen);
      url_content_wipe(b, blen);
   }
   return ok;
}

/* Sets key_a/key_b; also removes old_a/old_b (NULL: none). */
static bool url_logins_set_pair(const char *key_a, const char *a,
      const char *key_b, const char *b,
      const char *old_a, const char *old_b)
{
   bool ok;
   config_file_t *conf;
   if (!url_logins_value_ok(a) || !url_logins_value_ok(b))
      return false;
   if (!(conf = url_logins_open_for_write()))
      return false;
   ok = url_logins_store(conf, key_a, a)
     && url_logins_store(conf, key_b, b);
   if (ok && old_a)
      config_unset(conf, old_a);
   if (ok && old_b)
      config_unset(conf, old_b);
   ok = ok && url_logins_write(conf);
   config_file_free(conf);
   return ok;
}

/* The four keys of a site: new ones, and the older build's ones
 * (empty strings if the site has none). */
static bool url_logins_keys(const char *url, char *site,
      char key_user[URL_LOGINS_KEY_LEN], char key_pass[URL_LOGINS_KEY_LEN],
      char old_user[URL_LOGINS_KEY_LEN], char old_pass[URL_LOGINS_KEY_LEN])
{
   if (     !url_logins_site(site, URL_LOGINS_SITE_LEN, url)
         || !url_logins_site_key(key_user, URL_LOGINS_KEY_LEN, site, "user")
         || !url_logins_site_key(key_pass, URL_LOGINS_KEY_LEN, site, "pass"))
      return false;
   if (     !url_logins_legacy_key(old_user, URL_LOGINS_KEY_LEN, site, "user")
         || !url_logins_legacy_key(old_pass, URL_LOGINS_KEY_LEN, site, "pass"))
      old_user[0] = old_pass[0] = '\0';
   return true;
}

bool url_logins_get(const char *url, char *user, size_t ulen,
      char *pass, size_t plen)
{
   char site[URL_LOGINS_SITE_LEN];
   char key_user[URL_LOGINS_KEY_LEN];
   char key_pass[URL_LOGINS_KEY_LEN];
   char old_user[URL_LOGINS_KEY_LEN];
   char old_pass[URL_LOGINS_KEY_LEN];
   if (!url_logins_keys(url, site, key_user, key_pass, old_user, old_pass))
      return false;
   if (url_logins_get_pair(key_user, user, ulen, key_pass, pass, plen))
      return true;
   return old_user[0]
      && url_logins_get_pair(old_user, user, ulen, old_pass, pass, plen);
}

bool url_logins_set(const char *url, const char *user, const char *pass)
{
   char site[URL_LOGINS_SITE_LEN];
   char key_user[URL_LOGINS_KEY_LEN];
   char key_pass[URL_LOGINS_KEY_LEN];
   char old_user[URL_LOGINS_KEY_LEN];
   char old_pass[URL_LOGINS_KEY_LEN];
   if (!url_logins_keys(url, site, key_user, key_pass, old_user, old_pass))
      return false;
   return url_logins_set_pair(key_user, user, key_pass, pass,
         old_user[0] ? old_user : NULL, old_pass[0] ? old_pass : NULL);
}

bool url_logins_migrate(const char *url)
{
   char site[URL_LOGINS_SITE_LEN];
   char key_user[URL_LOGINS_KEY_LEN];
   char key_pass[URL_LOGINS_KEY_LEN];
   char old_user[URL_LOGINS_KEY_LEN];
   char old_pass[URL_LOGINS_KEY_LEN];
   char user[256];
   char pass[256];
   bool ok;
   if (!url_logins_keys(url, site, key_user, key_pass, old_user, old_pass))
      return false;
   if (     !old_user[0]
         || url_logins_get_pair(key_user, user, sizeof(user),
               key_pass, pass, sizeof(pass))
         || !url_logins_get_pair(old_user, user, sizeof(user),
               old_pass, pass, sizeof(pass)))
   {
      url_content_wipe(user, sizeof(user));
      url_content_wipe(pass, sizeof(pass));
      return true;   /* nothing to move */
   }
   ok = url_logins_set_pair(key_user, user, key_pass, pass,
         old_user, old_pass);
   url_content_wipe(user, sizeof(user));
   url_content_wipe(pass, sizeof(pass));
   return ok;
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
         "url_login_archive_secret", secret, NULL, NULL);
}

bool url_logins_is_private_file(const char *name)
{
   static const char *names[] = {
      "url_logins.cfg", "url_logins.cfg.tmp", "url_logins.cfg.bak",
      "url_servers.cfg", "url_servers.cfg.tmp", "url_servers.cfg.bak", NULL
   };
   const char *base;
   size_t i;
   if (string_is_empty(name))
      return false;
   base = name + strlen(name);
   while (base > name && base[-1] != '/' && base[-1] != '\\')
      base--;
   for (i = 0; names[i]; i++)
      if (string_is_equal_noncase(base, names[i]))
         return true;
   return false;
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
