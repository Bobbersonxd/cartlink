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

/* Unit tests for network/url_content.c (pure logic, no network). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../../network/url_content.h"
#include "../../network/url_logins.h"
#include "../../network/url_catalog.h"
#include "../../network/url_servers.h"
#include "../../network/url_safety.h"
#include "../../frontend/clipboard.h"
#include <lrc_hash.h>
#include <encodings/base64.h>

static int g_checks   = 0;
static int g_failures = 0;

#define EXPECT(x) do { \
   g_checks++; \
   if (!(x)) { \
      g_failures++; \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
   } \
} while (0)

#define EXPECT_STR(a, b) do { \
   const char *ea_ = (a); \
   const char *eb_ = (b); \
   g_checks++; \
   if (!ea_ || !eb_ || strcmp(ea_, eb_) != 0) { \
      g_failures++; \
      printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
            ea_ ? ea_ : "(null)", eb_ ? eb_ : "(null)"); \
   } \
} while (0)

/* Test seam: the build links with -Wl,--wrap=filestream_rename, so
 * every rename the code under test makes comes through here. It can
 * count renames, act like Windows (no rename over an existing file),
 * and fail chosen calls (bit i = fail the i-th call since the reset),
 * e.g. as when antivirus holds a freshly written file. */
int __real_filestream_rename(const char *old_path, const char *new_path);
static int      g_rename_calls   = 0;
static bool     g_rename_windows = false;
static unsigned g_rename_fail    = 0;

static void rename_seam_reset(bool windows, unsigned fail)
{
   g_rename_calls   = 0;
   g_rename_windows = windows;
   g_rename_fail    = fail;
}

int __wrap_filestream_rename(const char *old_path, const char *new_path);
int __wrap_filestream_rename(const char *old_path, const char *new_path)
{
   struct stat st;
   int i = g_rename_calls++;
   if (i < 32 && (g_rename_fail & (1u << i)))
      return -1;
   if (g_rename_windows && stat(new_path, &st) == 0)
      return -1;
   return __real_filestream_rename(old_path, new_path);
}

static bool ends_with(const char *s, const char *suffix)
{
   size_t ls = strlen(s);
   size_t lx = strlen(suffix);
   return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

static bool starts_with_hex16_underscore(const char *s)
{
   int i;
   for (i = 0; i < 16; i++)
   {
      char c = s[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }
   return s[16] == '_';
}

static void test_fix_link(void)
{
   char out[1024];
   const char *gd = "https://drive.usercontent.google.com/download?id=ABC123&export=download&confirm=t";

   url_content_fix_link(out, sizeof(out),
         "https://drive.google.com/file/d/ABC123/view?usp=sharing");
   EXPECT_STR(out, gd);
   url_content_fix_link(out, sizeof(out),
         "https://drive.google.com/open?id=ABC123");
   EXPECT_STR(out, gd);
   url_content_fix_link(out, sizeof(out),
         "https://drive.google.com/uc?id=ABC123&export=download");
   EXPECT_STR(out, gd);
   url_content_fix_link(out, sizeof(out),
         "https://docs.google.com/file/d/ABC123/edit");
   EXPECT_STR(out, gd);

   url_content_fix_link(out, sizeof(out),
         "https://www.dropbox.com/scl/fi/xyz/game.zip?rlkey=k&dl=0");
   EXPECT_STR(out, "https://www.dropbox.com/scl/fi/xyz/game.zip?rlkey=k&dl=1");
   url_content_fix_link(out, sizeof(out),
         "https://www.dropbox.com/scl/fi/xyz/game.zip?dl=0&rlkey=k");
   EXPECT_STR(out, "https://www.dropbox.com/scl/fi/xyz/game.zip?dl=1&rlkey=k");
   url_content_fix_link(out, sizeof(out),
         "https://www.dropbox.com/scl/fi/xyz/game.zip?rlkey=k");
   EXPECT_STR(out, "https://www.dropbox.com/scl/fi/xyz/game.zip?rlkey=k&dl=1");
   url_content_fix_link(out, sizeof(out),
         "https://www.dropbox.com/s/xyz/game.zip");
   EXPECT_STR(out, "https://www.dropbox.com/s/xyz/game.zip?dl=1");
   url_content_fix_link(out, sizeof(out),
         "https://www.dropbox.com/s/xyz/game.zip?dl=1");
   EXPECT_STR(out, "https://www.dropbox.com/s/xyz/game.zip?dl=1");

   url_content_fix_link(out, sizeof(out), "https://1drv.ms/u/s!AbC");
   EXPECT_STR(out, "https://api.onedrive.com/v1.0/shares/"
         "u!aHR0cHM6Ly8xZHJ2Lm1zL3UvcyFBYkM/root/content");

   url_content_fix_link(out, sizeof(out),
         "https://archive.org/details/ITEM/file%20name.zip");
   EXPECT_STR(out, "https://archive.org/download/ITEM/file%20name.zip");

   /* Plain http archive.org links move to https (keys only go over
    * https); other hosts keep http. */
   url_content_fix_link(out, sizeof(out),
         "http://archive.org/details/ITEM/game.zip");
   EXPECT_STR(out, "https://archive.org/download/ITEM/game.zip");
   url_content_fix_link(out, sizeof(out),
         "HTTP://ia800.us.archive.org/12/items/ITEM/game.zip");
   EXPECT_STR(out, "https://ia800.us.archive.org/12/items/ITEM/game.zip");
   url_content_fix_link(out, sizeof(out),
         "http://archive.org/download/ITEM/game.zip");
   EXPECT_STR(out, "https://archive.org/download/ITEM/game.zip");
   url_content_fix_link(out, sizeof(out),
         "http://notarchive.org/details/ITEM");
   EXPECT_STR(out, "http://notarchive.org/details/ITEM");
   url_content_fix_link(out, sizeof(out), "http://example.com/a.sfc");
   EXPECT_STR(out, "http://example.com/a.sfc");

   url_content_fix_link(out, sizeof(out), "https://example.com/a.sfc");
   EXPECT_STR(out, "https://example.com/a.sfc");

   /* A look-alike host must not be rewritten */
   url_content_fix_link(out, sizeof(out),
         "https://notdropbox.com/s/xyz/game.zip");
   EXPECT_STR(out, "https://notdropbox.com/s/xyz/game.zip");
}

static void test_cache_name(void)
{
   char a[512];
   char b[512];
   const char *u = "https://example.com/roms/My%20Game.sfc";

   url_content_cache_name(a, sizeof(a), u, NULL);
   EXPECT(ends_with(a, "_My Game.sfc"));
   EXPECT(starts_with_hex16_underscore(a));
   EXPECT(strlen(a) == 16 + strlen("_My Game.sfc"));

   url_content_cache_name(b, sizeof(b), u, NULL);
   EXPECT_STR(a, b);

   url_content_cache_name(b, sizeof(b),
         "https://example.com/roms/Other.sfc", NULL);
   EXPECT(strncmp(a, b, 16) != 0);

   url_content_cache_name(a, sizeof(a), u, "x/../y?.sfc");
   EXPECT(ends_with(a, "_y_.sfc"));
   EXPECT(strlen(a) == 16 + strlen("_y_.sfc"));
   EXPECT(!strchr(a, '/'));
   EXPECT(!strchr(a, '\\'));
   EXPECT(!strstr(a, ".."));

   url_content_cache_name(a, sizeof(a), u, "..\\..\\a:b*c\"d<e>f|g.zip");
   EXPECT(ends_with(a, "_a_b_c_d_e_f_g.zip"));

   url_content_cache_name(a, sizeof(a), u, "..");
   EXPECT(ends_with(a, "_download.bin"));

   url_content_cache_name(a, sizeof(a), "https://example.com/", NULL);
   EXPECT(ends_with(a, "_download.bin"));
   EXPECT(starts_with_hex16_underscore(a));
   url_content_cache_name(a, sizeof(a), "https://example.com", NULL);
   EXPECT(ends_with(a, "_download.bin"));
   url_content_cache_name(a, sizeof(a), "https://example.com/?q=1", NULL);
   EXPECT(ends_with(a, "_download.bin"));

   url_content_cache_name(a, sizeof(a),
         "https://example.com/dir/file.gba?token=x#frag", NULL);
   EXPECT(ends_with(a, "_file.gba"));

   /* Long names: at most 100 bytes after the prefix, extension kept,
    * no "..", no trailing dot, cut on a UTF-8 boundary. */
   {
      char name[400];
      char want[200];
      const char *n;

      /* Reviewer's case: 175 a, '.', 10 b, ".zip". */
      memset(name, 'a', 175);
      name[175] = '.';
      memset(name + 176, 'b', 10);
      strcpy(name + 186, ".zip");
      url_content_cache_name(a, sizeof(a), u, name);
      n = a + 17;
      EXPECT(strlen(n) <= 100);
      EXPECT(ends_with(a, ".zip"));
      EXPECT(!strstr(a, ".."));
      memset(want, 'a', 96);
      strcpy(want + 96, ".zip");
      EXPECT_STR(n, want);

      /* The cut lands right after a dot: no ".." left behind. */
      memset(name, 'a', 95);
      name[95] = '.';
      memset(name + 96, 'b', 30);
      strcpy(name + 126, ".zip");
      url_content_cache_name(a, sizeof(a), u, name);
      n = a + 17;
      EXPECT(strlen(n) <= 100);
      EXPECT(!strstr(n, ".."));
      memset(want, 'a', 95);
      strcpy(want + 95, ".zip");
      EXPECT_STR(n, want);

      /* The cut lands inside a 2-byte UTF-8 character (e9 = e-acute):
       * the whole character goes. */
      memset(name, 'a', 95);
      name[95] = (char)0xC3;
      name[96] = (char)0xA9;
      memset(name + 97, 'b', 30);
      strcpy(name + 127, ".zip");
      url_content_cache_name(a, sizeof(a), u, name);
      n = a + 17;
      EXPECT_STR(n, want);

      /* No (short) extension: plain cut, no trailing dot or space. */
      memset(name, 'c', 99);
      name[99] = ' ';
      memset(name + 100, 'd', 50);
      name[150] = '\0';
      url_content_cache_name(a, sizeof(a), u, name);
      n = a + 17;
      memset(want, 'c', 99);
      want[99] = '\0';
      EXPECT_STR(n, want);

      /* Exactly 100 bytes stays as is. */
      memset(name, 'e', 96);
      strcpy(name + 96, ".sfc");
      url_content_cache_name(a, sizeof(a), u, name);
      EXPECT_STR(a + 17, name);
   }

   url_content_cache_name(a, sizeof(a), u, "Real.gba");
   EXPECT(ends_with(a, "_Real.gba"));
   url_content_cache_name(a, sizeof(a), u, "");
   EXPECT(ends_with(a, "_My Game.sfc"));
}

static void test_disposition(void)
{
   char out[256];

   EXPECT(url_content_name_from_disposition(out, sizeof(out),
         "Content-Disposition: attachment; filename=\"Game.zip\""));
   EXPECT_STR(out, "Game.zip");

   EXPECT(url_content_name_from_disposition(out, sizeof(out),
         "Content-Disposition: attachment; filename*=UTF-8''Game%20DX.zip"));
   EXPECT_STR(out, "Game DX.zip");

   EXPECT(url_content_name_from_disposition(out, sizeof(out),
         "content-disposition: attachment; filename=\"Old.zip\"; "
         "filename*=UTF-8''New%20One.zip\r\n"));
   EXPECT_STR(out, "New One.zip");

   EXPECT(url_content_name_from_disposition(out, sizeof(out),
         "Content-Disposition: attachment; filename=Plain.7z\r\n"));
   EXPECT_STR(out, "Plain.7z");

   EXPECT(!url_content_name_from_disposition(out, sizeof(out),
         "Content-Disposition: inline"));
   EXPECT(!url_content_name_from_disposition(out, sizeof(out),
         "Content-Disposition: attachment; filename=\"\""));
}

static void test_host(void)
{
   char out[256];
   url_content_host(out, sizeof(out), "https://WWW.Example.COM:8080/a/b");
   EXPECT_STR(out, "www.example.com");
   url_content_host(out, sizeof(out), "http://user:pw@Host.org/x");
   EXPECT_STR(out, "host.org");
   url_content_host(out, sizeof(out), "https://example.com");
   EXPECT_STR(out, "example.com");
   url_content_host(out, sizeof(out), "https://example.com?x=1");
   EXPECT_STR(out, "example.com");
   url_content_host(out, sizeof(out), "http://[::1]:8080/x");
   EXPECT_STR(out, "[::1]");
}

static void test_wipe(void)
{
   char buf[16];
   size_t i;
   bool zero = true;
   memset(buf, 'x', sizeof(buf));
   url_content_wipe(buf, sizeof(buf));
   for (i = 0; i < sizeof(buf); i++)
      if (buf[i])
         zero = false;
   EXPECT(zero);
   url_content_wipe(NULL, 4); /* must not crash */
}

static void test_basic_auth(void)
{
   char out[256];
   url_content_basic_auth_header(out, sizeof(out), "testuser", "pw");
   EXPECT_STR(out, "Authorization: Basic dGVzdHVzZXI6cHc=\r\n");
}

static void test_archive_auth(void)
{
   char hdr[128];
   EXPECT(url_content_host_is_archive("archive.org"));
   EXPECT(url_content_host_is_archive("ia800.us.archive.org"));
   EXPECT(url_content_host_is_archive("Archive.ORG"));
   EXPECT(!url_content_host_is_archive("evilarchive.org"));
   EXPECT(!url_content_host_is_archive("archive.org.evil.com"));
   EXPECT(!url_content_host_is_archive(""));
   EXPECT(!url_content_host_is_archive(NULL));
   url_content_archive_auth_header(hdr, sizeof(hdr), "AKEY", "SKEY");
   EXPECT_STR(hdr, "Authorization: LOW AKEY:SKEY\r\n");
}

static void test_digest_auth(void)
{
   char out[1024];
   const char *chal =
      "WWW-Authenticate: Digest realm=\"testrealm@host.com\", "
      "qop=\"auth,auth-int\", "
      "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
      "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";

   url_content_set_test_cnonce("0a4f113b");

   EXPECT(url_content_digest_auth_header(out, sizeof(out), chal,
         "GET", "/dir/index.html", "Mufasa", "Circle Of Life"));
   EXPECT(strncmp(out, "Authorization: Digest ", 22) == 0);
   EXPECT(strstr(out, "response=\"6629fae49393a05397450978507c4ef1\"") != NULL);
   EXPECT(strstr(out, "username=\"Mufasa\"") != NULL);
   EXPECT(strstr(out, "realm=\"testrealm@host.com\"") != NULL);
   EXPECT(strstr(out, "uri=\"/dir/index.html\"") != NULL);
   EXPECT(strstr(out, "qop=auth") != NULL);
   EXPECT(strstr(out, "nc=00000001") != NULL);
   EXPECT(strstr(out, "cnonce=\"0a4f113b\"") != NULL);
   EXPECT(strstr(out, "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"") != NULL);
   EXPECT(ends_with(out, "\"\r\n"));

   /* Value-only challenge (no header name) also works */
   EXPECT(url_content_digest_auth_header(out, sizeof(out), chal + 18,
         "GET", "/dir/index.html", "Mufasa", "Circle Of Life"));
   EXPECT(strstr(out, "response=\"6629fae49393a05397450978507c4ef1\"") != NULL);

   /* No qop (RFC 2069 style):
    * HA1 = MD5(Mufasa:testrealm@host.com:Circle Of Life)
    * HA2 = MD5(GET:/dir/index.html)
    * response = MD5(HA1:nonce:HA2) */
   EXPECT(url_content_digest_auth_header(out, sizeof(out),
         "Digest realm=\"testrealm@host.com\", "
         "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\"",
         "GET", "/dir/index.html", "Mufasa", "Circle Of Life"));
   EXPECT(strstr(out, "response=\"670fd8c2df070c60b045671b8b24ff02\"") != NULL);
   EXPECT(strstr(out, "qop=") == NULL);
   EXPECT(strstr(out, "nc=") == NULL);

   /* Not digest / missing nonce / unsupported qop */
   EXPECT(!url_content_digest_auth_header(out, sizeof(out),
         "WWW-Authenticate: Basic realm=\"x\"",
         "GET", "/", "a", "b"));
   EXPECT(!url_content_digest_auth_header(out, sizeof(out),
         "Digest realm=\"x\"", "GET", "/", "a", "b"));
   EXPECT(!url_content_digest_auth_header(out, sizeof(out),
         "Digest realm=\"x\", nonce=\"n\", qop=\"auth-int\"",
         "GET", "/", "a", "b"));

   url_content_set_test_cnonce(NULL);
}

static void test_is_downloadable(void)
{
   EXPECT(url_content_is_downloadable("https://x/y.zip"));
   EXPECT(url_content_is_downloadable("http://x/y.sfc"));
   EXPECT(url_content_is_downloadable("HTTPS://x/y.zip?a=b.mp4"));
   EXPECT(!url_content_is_downloadable("https://x/live.m3u8"));
   EXPECT(!url_content_is_downloadable("https://x/clip.MP4?t=1"));
   EXPECT(!url_content_is_downloadable("udp://x"));
   EXPECT(!url_content_is_downloadable("/srv/a.sfc"));
   EXPECT(!url_content_is_downloadable(""));
   EXPECT(!url_content_is_downloadable(NULL));
}

static void test_looks_like_html(void)
{
   const char *h1 = "\n  <!DOCTYPE html>";
   const char *h2 = "\xEF\xBB\xBF<HTML lang=en>";
   const unsigned char zip[] = { 'P', 'K', 0x03, 0x04, 0x14, 0x00 };

   EXPECT(url_content_looks_like_html((const uint8_t*)h1, strlen(h1)));
   EXPECT(url_content_looks_like_html((const uint8_t*)h2, strlen(h2)));
   EXPECT(!url_content_looks_like_html(zip, sizeof(zip)));
   EXPECT(!url_content_looks_like_html((const uint8_t*)"<htm", 4));
   EXPECT(!url_content_looks_like_html(NULL, 0));
}

static bool file_exists(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0;
}

static void test_logins(void)
{
   char dir[]   = "/tmp/url_logins_test_XXXXXX";
   char path[512];
   char user[128];
   char pass[128];
   char access[128];
   char secret[128];
   char line[256];
#ifndef _WIN32
   struct stat st;
#endif
   FILE *f;
   bool saw_host_key = false;

   if (!mkdtemp(dir))
   {
      g_checks++;
      g_failures++;
      printf("FAIL %s:%d: mkdtemp\n", __FILE__, __LINE__);
      return;
   }
   snprintf(path, sizeof(path), "%s/url_logins.cfg", dir);
   url_logins_set_path(path);

   /* Missing file: every get is false, nothing crashes. */
   EXPECT(!file_exists(path));
   EXPECT(!url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get_archive_keys(access, sizeof(access),
         secret, sizeof(secret)));
   EXPECT(url_logins_forget_all());
   EXPECT(!file_exists(path));

   /* Round trip, two hosts. */
   EXPECT(url_logins_set("https://example.com", "alice", "s3cret pass"));
   EXPECT(url_logins_set("https://files.my-nas.local:8443", "testuser", "hunter2"));
   EXPECT(file_exists(path));
#ifndef _WIN32
   /* Windows has no POSIX file modes; url_logins skips them there. */
   EXPECT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
#endif

   user[0] = pass[0] = '\0';
   EXPECT(url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "alice");
   EXPECT_STR(pass, "s3cret pass");
   EXPECT(url_logins_get("https://files.my-nas.local:8443", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "testuser");
   EXPECT_STR(pass, "hunter2");

   /* Key format: the origin, scheme://host[:port], default port left
    * out, any URL of the site finds it. */
   if ((f = fopen(path, "r")))
   {
      while (fgets(line, sizeof(line), f))
         if (strncmp(line, "url_login_https://example.com_user ", 35) == 0)
            saw_host_key = true;
      fclose(f);
   }
   EXPECT(saw_host_key);
   EXPECT(url_logins_get("HTTPS://Example.com:443/some/game.sfc?x=1",
         user, sizeof(user), pass, sizeof(pass)));
   EXPECT_STR(user, "alice");
   EXPECT(url_logins_get("https://files.my-nas.local:8443/a/",
         user, sizeof(user), pass, sizeof(pass)));
   /* Another port, plain http or a bare host: not that site. */
   EXPECT(!url_logins_get("https://example.com:8443", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get("https://files.my-nas.local", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get("http://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get("example.com", user, sizeof(user),
         pass, sizeof(pass)));
   /* A login is never kept for plain http (it is never sent there). */
   EXPECT(!url_logins_set("http://example.com", "x", "y"));
   EXPECT(!url_logins_set("example.com", "x", "y"));
   EXPECT(!url_logins_set("ftp://example.com", "x", "y"));
   /* A host the file format could not hold as a key. */
   EXPECT(!url_logins_set("https://ex\"ample.com", "x", "y"));
   EXPECT(!url_logins_set("https://ex ample.com", "x", "y"));
   EXPECT(!url_logins_set("https://ex=ample.com", "x", "y"));

   /* Second set replaces the first; other host untouched. */
   EXPECT(url_logins_set("https://example.com", "carol", "new"));
   EXPECT(url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "carol");
   EXPECT_STR(pass, "new");
   EXPECT(url_logins_get("https://files.my-nas.local:8443", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "testuser");

   /* Unknown host. */
   EXPECT(!url_logins_get("https://unknown.org", user, sizeof(user),
         pass, sizeof(pass)));

   /* Values the file format cannot hold are refused. */
   EXPECT(!url_logins_set("https://example.com", "x", "has\"quote"));
   EXPECT(!url_logins_set("https://example.com", "x", "line\nbreak"));
   EXPECT(!url_logins_set(NULL, "x", "y"));
   EXPECT(!url_logins_set("", "x", "y"));
   EXPECT(url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(pass, "new");

   /* Archive keys round trip. */
   EXPECT(url_logins_set_archive_keys("AKEY123", "SKEY456"));
   access[0] = secret[0] = '\0';
   EXPECT(url_logins_get_archive_keys(access, sizeof(access),
         secret, sizeof(secret)));
   EXPECT_STR(access, "AKEY123");
   EXPECT_STR(secret, "SKEY456");
   EXPECT(url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "carol");
#ifndef _WIN32
   /* Windows has no POSIX file modes; url_logins skips them there. */
   EXPECT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
#endif

   /* Writes go through <path>.tmp + rename: no .tmp is left, a
    * stale one is replaced, and a failed write keeps the old file. */
   {
      char tmp[600];
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      EXPECT(!file_exists(tmp));
      if ((f = fopen(tmp, "wb")))
      {
         fputs("stale junk\n", f);
         fclose(f);
      }
      EXPECT(url_logins_set("https://example.com", "dave", "pw2"));
      EXPECT(!file_exists(tmp));
      EXPECT(url_logins_get("https://example.com", user, sizeof(user),
            pass, sizeof(pass)));
      EXPECT_STR(user, "dave");
#ifndef _WIN32
      EXPECT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
      /* A folder in the way of the .tmp: the write fails, the old
       * file is untouched. */
      {
         char inner[700];
         snprintf(inner, sizeof(inner), "%s/x", tmp);
         EXPECT(mkdir(tmp, 0700) == 0);
         if ((f = fopen(inner, "wb")))
            fclose(f);
         EXPECT(!url_logins_set("https://example.com", "eve", "pw3"));
         remove(inner);
      }
      EXPECT(url_logins_get("https://example.com", user, sizeof(user),
            pass, sizeof(pass)));
      EXPECT_STR(user, "dave");
      EXPECT_STR(pass, "pw2");
      rmdir(tmp);
#endif
      EXPECT(url_logins_set("https://example.com", "carol", "new"));
   }

   /* Forget all: file gone, every get false. */
   EXPECT(url_logins_forget_all());
   EXPECT(!file_exists(path));
   EXPECT(!url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get("https://files.my-nas.local:8443", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get_archive_keys(access, sizeof(access),
         secret, sizeof(secret)));

   /* No path set: everything fails safely. */
   url_logins_set_path(NULL);
   EXPECT(!url_logins_set("https://example.com", "a", "b"));
   EXPECT(!url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));

   remove(path);
   rmdir(dir);
}

static void test_form_encode(void)
{
   char out[64];

   EXPECT(url_content_form_encode(out, sizeof(out), "user@example.com"));
   EXPECT_STR(out, "user%40example.com");
   EXPECT(url_content_form_encode(out, sizeof(out), "a b&c=d+e/~_.-"));
   EXPECT_STR(out, "a+b%26c%3Dd%2Be%2F~_.-");
   EXPECT(url_content_form_encode(out, sizeof(out), "\xc3\xa9"));
   EXPECT_STR(out, "%C3%A9");
   EXPECT(url_content_form_encode(out, sizeof(out), ""));
   EXPECT_STR(out, "");
   /* Exactly fits: 3 chars + NUL in 4. */
   EXPECT(url_content_form_encode(out, 4, "abc"));
   EXPECT_STR(out, "abc");
   /* Too long: false and an empty result, never a cut value. */
   EXPECT(!url_content_form_encode(out, 4, "abcd"));
   EXPECT_STR(out, "");
   EXPECT(!url_content_form_encode(out, 5, "ab@"));
   EXPECT_STR(out, "");
}

static void test_archive_login_parse(void)
{
   char access[64];
   char secret[64];
   char err[128];
   const char *ok =
      "{\"success\": true, \"values\": {\"cookies\": {\"logged-in-sig\": \"x\","
      " \"logged-in-user\": \"user%40example.com\"}, \"email\": \"user@example.com\","
      " \"itemname\": \"@testuser\", \"s3\": {\"access\": \"AKEY123\","
      " \"secret\": \"SKEY456\"}, \"screenname\": \"testuser\"}, \"version\": 1}";
   /* s3 before success, and noise with arrays and nested objects. */
   const char *ok_reordered =
      "{\"values\": {\"list\": [{\"s3\": {\"access\": \"WRONG\"}}, \"s3\"],"
      " \"s3\": {\"secret\": \"S2\", \"access\": \"A2\"}},"
      " \"success\": true}";
   const char *bad_pw =
      "{\"success\": false, \"values\": {\"reason\": \"account_bad_password\"},"
      " \"version\": 1}";
   const char *not_found =
      "{\"success\": false, \"values\": {\"reason\": \"account_not_found\"},"
      " \"version\": 1}";
   const char *other_reason =
      "{\"success\": false, \"values\": {\"reason\": \"account_locked\"}}";
   const char *error_only =
      "{\"success\": false, \"error\": \"Missing email\"}";
   const char *no_s3 =
      "{\"success\": true, \"values\": {\"email\": \"user@example.com\"}}";
   const char *s3_top_level =
      "{\"success\": true, \"s3\": {\"access\": \"A\", \"secret\": \"S\"}}";
   const char *success_string =
      "{\"success\": \"true\", \"values\": {\"s3\": {\"access\": \"A\","
      " \"secret\": \"S\"}}}";
   const char *broken = "{\"success\": true, \"values\": {\"s3\": {\"acc";
   const char *html   = "<!DOCTYPE html><html></html>";

   EXPECT(url_content_parse_archive_login(ok, strlen(ok),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(access, "AKEY123");
   EXPECT_STR(secret, "SKEY456");
   EXPECT_STR(err, "");

   EXPECT(url_content_parse_archive_login(ok_reordered, strlen(ok_reordered),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(access, "A2");
   EXPECT_STR(secret, "S2");

   EXPECT(!url_content_parse_archive_login(bad_pw, strlen(bad_pw),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "wrong email or password");
   EXPECT_STR(access, "");
   EXPECT_STR(secret, "");

   EXPECT(!url_content_parse_archive_login(not_found, strlen(not_found),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "no account with that email");

   EXPECT(!url_content_parse_archive_login(other_reason, strlen(other_reason),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "account_locked");

   EXPECT(!url_content_parse_archive_login(error_only, strlen(error_only),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "Missing email");

   EXPECT(!url_content_parse_archive_login(no_s3, strlen(no_s3),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "no keys in the reply");

   EXPECT(!url_content_parse_archive_login(s3_top_level, strlen(s3_top_level),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "no keys in the reply");

   EXPECT(!url_content_parse_archive_login(success_string,
         strlen(success_string),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "unexpected reply");
   EXPECT_STR(access, "");

   EXPECT(!url_content_parse_archive_login(broken, strlen(broken),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "unexpected reply");
   EXPECT_STR(access, "");
   EXPECT_STR(secret, "");

   EXPECT(!url_content_parse_archive_login(html, strlen(html),
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "unexpected reply");

   EXPECT(!url_content_parse_archive_login(NULL, 0,
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "unexpected reply");

   /* Keys that do not fit are refused, never cut. */
   EXPECT(!url_content_parse_archive_login(ok, strlen(ok),
         access, 4, secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(access, "");
   EXPECT_STR(secret, "");

   /* NULL err is fine. */
   EXPECT(!url_content_parse_archive_login(bad_pw, strlen(bad_pw),
         access, sizeof(access), secret, sizeof(secret), NULL, 0));
}

static void test_cli_should_fetch(void)
{
   const char *nes = "/cores/fceumm_libretro.so";

   /* A game link with a game core: fetch. */
   EXPECT(url_content_cli_should_fetch(true, "http://h/game.nes", nes, false));
   EXPECT(url_content_cli_should_fetch(true, "https://h/game.nes", nes, true));
   EXPECT(url_content_cli_should_fetch(true, "http://h/dl?id=3", nes, false));
   /* Not a link, or a media link: never. */
   EXPECT(!url_content_cli_should_fetch(true, "/roms/game.nes", nes, false));
   EXPECT(!url_content_cli_should_fetch(true, "http://h/live.mp4", nes, false));
   EXPECT(!url_content_cli_should_fetch(true, "rtmp://h/live", nes, false));
   EXPECT(!url_content_cli_should_fetch(true, NULL, nes, false));
   /* A media core keeps every URL, even extensionless streams. */
   EXPECT(!url_content_cli_should_fetch(true, "http://h:8080/stream",
         "/cores/ffmpeg_libretro.so", false));
   EXPECT(!url_content_cli_should_fetch(true, "http://h/x.ts",
         "C:\\cores\\FFMPEG_libretro.dll", false));
   EXPECT(!url_content_cli_should_fetch(true, "http://h/stream",
         "/cores/mpv_libretro.so", false));
   /* Only the core's file name counts, not its folder. */
   EXPECT(url_content_cli_should_fetch(true, "http://h/game.nes",
         "/opt/mpv-stuff/fceumm_libretro.so", false));
   /* Later parses (menu, history, netplay reloads): never, whatever
    * the path, core or player setting. */
   EXPECT(!url_content_cli_should_fetch(false, "http://h/game.nes", nes,
         false));
   EXPECT(!url_content_cli_should_fetch(false, "https://h/dl?id=3", NULL,
         false));
   EXPECT(!url_content_cli_should_fetch(false, "http://h/game.nes", "",
         true));
   /* No core: only when the built-in player would not take it. */
   EXPECT(url_content_cli_should_fetch(true, "http://h/game.nes", NULL, false));
   EXPECT(url_content_cli_should_fetch(true, "http://h/game.nes", "", false));
   EXPECT(!url_content_cli_should_fetch(true, "http://h/stream", NULL, true));
   EXPECT(!url_content_cli_should_fetch(true, "http://h/stream", "", true));
}

static void test_clipboard_clean(void)
{
   char s[64];

   strlcpy(s, "  https://a/b.zip\r\n", sizeof(s));
   clipboard_clean_text(s);
   EXPECT_STR(s, "https://a/b.zip");

   /* Line breaks inside are dropped too (a link wrapped by a mail app). */
   strlcpy(s, "\thttps://a/\r\nlong.zip \n", sizeof(s));
   clipboard_clean_text(s);
   EXPECT_STR(s, "https://a/long.zip");

   /* Other control bytes go; inner spaces and UTF-8 stay. */
   strlcpy(s, "a\x01" "b c \xc3\xa9", sizeof(s));
   clipboard_clean_text(s);
   EXPECT_STR(s, "ab c \xc3\xa9");

   strlcpy(s, " \r\n\t ", sizeof(s));
   clipboard_clean_text(s);
   EXPECT_STR(s, "");

   s[0] = '\0';
   clipboard_clean_text(s);
   EXPECT_STR(s, "");
   clipboard_clean_text(NULL);

   strlcpy(s, "plain", sizeof(s));
   clipboard_clean_text(s);
   EXPECT_STR(s, "plain");
}

#define CAT_BASE "https://h.example/games/"

static bool cat_parse(const char *json, struct url_catalog *c)
{
   return url_catalog_parse(json, strlen(json), CAT_BASE, c);
}

static bool cat_parse_at(const char *json, const char *base,
      struct url_catalog *c)
{
   return url_catalog_parse(json, strlen(json), base, c);
}

static void test_content_resolve(void)
{
   char out[256];
   EXPECT(url_content_resolve(out, sizeof(out), "https://h/a/b.gb", "c.gb"));
   EXPECT_STR(out, "https://h/a/c.gb");
   EXPECT(url_content_resolve(out, sizeof(out), "https://h/a/", "/x"));
   EXPECT_STR(out, "https://h/x");
   EXPECT(url_content_resolve(out, sizeof(out), "https://h/a/", "//o/y"));
   EXPECT_STR(out, "https://o/y");
   EXPECT(url_content_resolve(out, sizeof(out), "https://h", "z"));
   EXPECT_STR(out, "https://h/z");
   EXPECT(!url_content_resolve(out, sizeof(out), "https://h/", "ftp://x/"));
   EXPECT(!url_content_resolve(out, sizeof(out), "https://h/", ""));
}

static void test_catalog_parse_basic(void)
{
   struct url_catalog c;
   EXPECT(cat_parse("[{\"name\":\"NES\",\"is_dir\":true,\"url\":\"./NES/\",\"size\":0},"
         "{\"name\":\"b.gb\",\"is_dir\":false,\"url\":\"./b.gb\",\"size\":10},"
         "{\"name\":\"A.gb\",\"is_dir\":false,\"url\":\"./A.gb\",\"size\":5}]", &c));
   EXPECT(c.count == 3);
   EXPECT(!c.truncated);
   EXPECT_STR(c.base, CAT_BASE);
   if (c.count == 3)
   {
      EXPECT_STR(c.entries[0].name, "NES");
      EXPECT(c.entries[0].is_dir);
      EXPECT_STR(c.entries[0].url, "https://h.example/games/NES/");
      EXPECT_STR(c.entries[1].name, "A.gb");
      EXPECT(!c.entries[1].is_dir);
      EXPECT(c.entries[1].size == 5);
      EXPECT_STR(c.entries[1].url, "https://h.example/games/A.gb");
      EXPECT_STR(c.entries[2].name, "b.gb");
      EXPECT(c.entries[2].size == 10);
   }
   url_catalog_free(&c);
   EXPECT(c.entries == NULL && c.count == 0);

   /* Real Caddy fields beyond the four are ignored; absolute same-site
    * URLs are fine. */
   EXPECT(cat_parse("[{\"name\":\"z.gb\",\"size\":7,\"url\":\"https://h.example/games/z.gb\","
         "\"mod_time\":\"2026-11-22T10:00:00Z\",\"mode\":420,\"is_dir\":false,"
         "\"is_symlink\":false,\"extra\":{\"k\":[1,{\"name\":\"q\"}]}}]", &c));
   EXPECT(c.count == 1);
   if (c.count == 1)
   {
      EXPECT_STR(c.entries[0].url, "https://h.example/games/z.gb");
      EXPECT(c.entries[0].size == 7);
   }
   url_catalog_free(&c);
}

static void test_catalog_parse_sort(void)
{
   struct url_catalog c;
   EXPECT(cat_parse("[{\"name\":\"c.gb\",\"is_dir\":false,\"url\":\"./c.gb\"},"
         "{\"name\":\"zed\",\"is_dir\":true,\"url\":\"./zed/\"},"
         "{\"name\":\"B.gb\",\"is_dir\":false,\"url\":\"./B.gb\"},"
         "{\"name\":\"Alpha\",\"is_dir\":true,\"url\":\"./Alpha/\"},"
         "{\"name\":\"a.gb\",\"is_dir\":false,\"url\":\"./a.gb\"}]", &c));
   EXPECT(c.count == 5);
   if (c.count == 5)
   {
      EXPECT_STR(c.entries[0].name, "Alpha");
      EXPECT_STR(c.entries[1].name, "zed");
      EXPECT_STR(c.entries[2].name, "a.gb");
      EXPECT_STR(c.entries[3].name, "B.gb");
      EXPECT_STR(c.entries[4].name, "c.gb");
   }
   url_catalog_free(&c);
}

static void test_catalog_parse_unsafe(void)
{
   struct url_catalog c;
   EXPECT(cat_parse("["
         "{\"name\":\"e.gb\",\"is_dir\":false,\"url\":\"https://evil.example/x.gb\"},"
         "{\"name\":\"s.gb\",\"is_dir\":false,\"url\":\"http://h.example/games/x.gb\"},"
         "{\"name\":\"p.gb\",\"is_dir\":false,\"url\":\"https://h.example:8443/games/x.gb\"},"
         "{\"name\":\"r.gb\",\"is_dir\":false,\"url\":\"//evil.example/games/x.gb\"},"
         "{\"name\":\"u.gb\",\"is_dir\":false,\"url\":\"../secret.gb\"},"
         "{\"name\":\"v.gb\",\"is_dir\":false,\"url\":\"./a/../../x\"},"
         "{\"name\":\"w.gb\",\"is_dir\":false,\"url\":\"/other/x.gb\"},"
         "{\"name\":\"x.gb\",\"is_dir\":false,\"url\":\"./%2e%2e/x.gb\"},"
         "{\"name\":\"self\",\"is_dir\":true,\"url\":\"./\"},"
         "{\"name\":\"bad\\nname.gb\",\"is_dir\":false,\"url\":\"./n.gb\"},"
         "{\"name\":\"\",\"is_dir\":false,\"url\":\"./m.gb\"},"
         "{\"name\":\"ok.gb\",\"is_dir\":false,\"url\":\"./sub/../ok.gb\"}"
         "]", &c));
   EXPECT(c.count == 1);
   if (c.count == 1)
   {
      EXPECT_STR(c.entries[0].name, "ok.gb");
      EXPECT_STR(c.entries[0].url, "https://h.example/games/ok.gb");
   }
   url_catalog_free(&c);
}

/* Caddy names folders "Name/": one trailing slash is dropped for
 * folders only; files, "/" alone and "a/b/" are still refused. */
static void test_catalog_parse_dir_slash(void)
{
   struct url_catalog c;
   EXPECT(cat_parse("["
         "{\"name\":\"Game Boy/\",\"is_dir\":true,\"url\":\"./Game%20Boy/\"},"
         "{\"name\":\"f.gb/\",\"is_dir\":false,\"url\":\"./f.gb\"},"
         "{\"name\":\"/\",\"is_dir\":true,\"url\":\"./z/\"},"
         "{\"name\":\"a/b/\",\"is_dir\":true,\"url\":\"./a/b/\"},"
         "{\"name\":\"two//\",\"is_dir\":true,\"url\":\"./two/\"}"
         "]", &c));
   EXPECT(c.count == 1);
   if (c.count == 1)
   {
      EXPECT_STR(c.entries[0].name, "Game Boy");
      EXPECT(c.entries[0].is_dir);
      EXPECT_STR(c.entries[0].url, "https://h.example/games/Game%20Boy/");
   }
   url_catalog_free(&c);
}

static void test_catalog_parse_names(void)
{
   struct url_catalog c;
   char *json;
   char *p;
   int i;

   EXPECT(cat_parse("[{\"name\":\"Pok\\u00e9mon.gbc\",\"is_dir\":false,"
         "\"url\":\"./Pok%C3%A9mon.gbc\"}]", &c));
   EXPECT(c.count == 1);
   if (c.count == 1)
   {
      EXPECT_STR(c.entries[0].name, "Pok\xc3\xa9mon.gbc");
      EXPECT_STR(c.entries[0].url, "https://h.example/games/Pok%C3%A9mon.gbc");
   }
   url_catalog_free(&c);

   /* raw UTF-8 in the JSON works the same as \u escapes */
   EXPECT(cat_parse("[{\"name\":\"Pok\xc3\xa9mon.gbc\",\"is_dir\":false,"
         "\"url\":\"./Pok%C3%A9mon.gbc\"}]", &c));
   EXPECT(c.count == 1);
   if (c.count == 1)
      EXPECT_STR(c.entries[0].name, "Pok\xc3\xa9mon.gbc");
   url_catalog_free(&c);

   /* 400 two-byte characters: cut to 254 bytes, never mid-character. */
   json = (char*)malloc(2048);
   p    = json;
   p   += sprintf(p, "[{\"name\":\"");
   for (i = 0; i < 400; i++)
      p += sprintf(p, "\xc3\xa9");
   sprintf(p, "\",\"is_dir\":false,\"url\":\"./long.gb\"},"
         "{\"name\":\"%0400d\",\"is_dir\":false,\"url\":\"./long2.gb\"}]", 0);
   EXPECT(cat_parse(json, &c));
   EXPECT(c.count == 2);
   if (c.count == 2)
   {
      size_t l0 = strlen(c.entries[0].name);
      size_t l1 = strlen(c.entries[1].name);
      /* "000..." sorts before "\xc3..." */
      EXPECT(l0 == 255);
      EXPECT(l1 == 254);
      EXPECT((unsigned char)c.entries[1].name[l1 - 2] == 0xc3);
      EXPECT((unsigned char)c.entries[1].name[l1 - 1] == 0xa9);
   }
   url_catalog_free(&c);
   free(json);
}

static void test_catalog_parse_edges(void)
{
   struct url_catalog c;
   char *json = (char*)malloc(URL_CATALOG_URL_LEN + 256);
   char *p    = json;
   int i;

   /* a url too long for an entry skips that entry only */
   p += sprintf(p, "[{\"name\":\"l.gb\",\"is_dir\":false,\"url\":\"./");
   for (i = 0; i < URL_CATALOG_URL_LEN; i++)
      *p++ = 'a';
   sprintf(p, "\"},{\"name\":\"k.gb\",\"is_dir\":false,\"url\":\"./k.gb\"}]");
   EXPECT(cat_parse(json, &c));
   EXPECT(c.count == 1);
   url_catalog_free(&c);
   free(json);

   /* base without a trailing slash: its folder is the parent */
   EXPECT(cat_parse_at("[{\"name\":\"k.gb\",\"is_dir\":false,\"url\":\"k.gb\"},"
         "{\"name\":\"j.gb\",\"is_dir\":false,\"url\":\"/top/j.gb\"}]",
         "https://h.example/top/list?x=1", &c));
   EXPECT(c.count == 2);
   if (c.count == 2)
      EXPECT_STR(c.entries[1].url, "https://h.example/top/k.gb");
   url_catalog_free(&c);

   /* host case does not matter; a bare-host base has the root folder */
   EXPECT(cat_parse_at("[{\"name\":\"k.gb\",\"is_dir\":false,"
         "\"url\":\"HTTPS://H.example/k.gb\"}]", "https://h.example", &c));
   EXPECT(c.count == 1);
   url_catalog_free(&c);
}

static void test_catalog_parse_not_listing(void)
{
   struct url_catalog c;
   EXPECT(!cat_parse("{\"a\":1}", &c));
   EXPECT(c.entries == NULL && c.count == 0);
   EXPECT(!cat_parse("[1,2]", &c));
   EXPECT(!cat_parse("[{\"name\":\"x\"}]", &c));
   EXPECT(!cat_parse("[{\"name\":\"x\",\"is_dir\":\"no\",\"url\":\"./x\"}]", &c));
   EXPECT(!cat_parse("[{\"name\":\"x\",\"is_dir\":false,\"url\":\"./x\"},3]", &c));
   EXPECT(!cat_parse("[[{\"name\":\"x\",\"is_dir\":false,\"url\":\"./x\"}]]", &c));
   EXPECT(!cat_parse("[{\"name\":\"x\",\"is_dir\":false,\"url\":\"./x\"}", &c));
   EXPECT(!cat_parse("not json", &c));
   EXPECT(!cat_parse("", &c));
   EXPECT(!url_catalog_parse(NULL, 0, CAT_BASE, &c));
   EXPECT(c.entries == NULL && c.count == 0);
}

static void test_catalog_parse_empty(void)
{
   struct url_catalog c;
   EXPECT(cat_parse("[]", &c));
   EXPECT(c.count == 0);
   EXPECT(!c.truncated);
   url_catalog_free(&c);
   EXPECT(cat_parse(" [ ] ", &c));
   url_catalog_free(&c);
}

static void test_catalog_parse_cap(void)
{
   struct url_catalog c;
   size_t cap = 5000 * 64 + 16;
   char *json = (char*)malloc(cap);
   char *p    = json;
   int i;

   *p++ = '[';
   for (i = 0; i < 5000; i++)
      p += sprintf(p, "%s{\"name\":\"g%05d.gb\",\"is_dir\":false,\"url\":\"./g%05d.gb\"}",
            i ? "," : "", i, i);
   *p++ = ']';
   *p   = '\0';
   EXPECT(cat_parse(json, &c));
   EXPECT(c.count == URL_CATALOG_MAX);
   EXPECT(c.truncated);
   if (c.count == URL_CATALOG_MAX)
      EXPECT_STR(c.entries[0].name, "g00000.gb");
   url_catalog_free(&c);
   free(json);
}

static void test_catalog_parent(void)
{
   char out[256];
   EXPECT(url_catalog_parent(out, sizeof(out), "https://h/games/NES/"));
   EXPECT_STR(out, "https://h/games/");
   EXPECT(url_catalog_parent(out, sizeof(out), "https://h/games/"));
   EXPECT_STR(out, "https://h/");
   EXPECT(url_catalog_parent(out, sizeof(out), "https://h/games/a.gb"));
   EXPECT_STR(out, "https://h/games/");
   EXPECT(!url_catalog_parent(out, sizeof(out), "https://h/"));
   EXPECT(!url_catalog_parent(out, sizeof(out), "https://h"));
   EXPECT(!url_catalog_parent(out, sizeof(out), "not a url"));
}

static void test_catalog_title(void)
{
   char out[256];
   char tiny[8];
   url_catalog_title(out, sizeof(out), "http://127.0.0.1:8080/cat/");
   EXPECT_STR(out, "127.0.0.1:8080/cat/");
   url_catalog_title(out, sizeof(out),
         "https://user:pw@Games.example/cat/Game%20Boy/?x=1#y");
   EXPECT_STR(out, "Games.example/cat/Game Boy/");
   /* Encoded control characters and bad escapes are not shown raw. */
   url_catalog_title(out, sizeof(out), "http://h/a%0Ab%zz%2/");
   EXPECT_STR(out, "h/ab%zz%2/");
   url_catalog_title(out, sizeof(out), "not a url");
   EXPECT_STR(out, "");
   url_catalog_title(tiny, sizeof(tiny), "http://hostname/long/");
   EXPECT_STR(tiny, "hostnam");
}

/* Saved Servers: each test gets its own temp file. */
static bool servers_temp(char *dir, char *path, size_t len)
{
   if (!mkdtemp(dir))
   {
      g_checks++;
      g_failures++;
      printf("FAIL %s:%d: mkdtemp\n", __FILE__, __LINE__);
      return false;
   }
   snprintf(path, len, "%s/url_servers.cfg", dir);
   url_servers_set_path(path);
   return true;
}

static void servers_cleanup(const char *dir, const char *path)
{
   char tmp[600];
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   unlink(path);
   unlink(tmp);
   snprintf(tmp, sizeof(tmp), "%s.bak", path);
   unlink(tmp);
   rmdir(dir);
   url_servers_set_path(NULL);
   rename_seam_reset(false, 0);
}

static bool file_has(const char *path, const char *text)
{
   char  buf[4096];
   size_t n;
   FILE *f = fopen(path, "rb");
   if (!f)
      return false;
   n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   return strstr(buf, text) != NULL;
}

/* Re-adding the server that is already first (the game list does it
 * on every folder open) must not touch the file at all. */
static void test_servers_add_top_no_write(void)
{
   char dir[] = "/tmp/url_servers_test_XXXXXX";
   char path[512];
   char out[256];
   struct stat a, b;
   if (!servers_temp(dir, path, sizeof(path)))
      return;
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(url_servers_add("https://b.example/"));
   EXPECT(stat(path, &a) == 0);
   rename_seam_reset(false, 0);
   EXPECT(url_servers_add("https://b.example/"));
   EXPECT(url_servers_add("https://u:p@b.example/"));
   EXPECT(g_rename_calls == 0);
   EXPECT(stat(path, &b) == 0);
   EXPECT(a.st_ino == b.st_ino);
   EXPECT(url_servers_count() == 2);
   /* Moving another one to the top still saves. */
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(g_rename_calls == 1);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://a.example/");
   url_servers_set_path(path);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://a.example/");
   servers_cleanup(dir, path);
}

/* Windows rules: no rename over an existing file. */
static void test_servers_windows_replace(void)
{
   char dir[] = "/tmp/url_servers_test_XXXXXX";
   char path[512];
   char side[600];
   char out[256];
   if (!servers_temp(dir, path, sizeof(path)))
      return;
   rename_seam_reset(true, 0);
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(url_servers_add("https://b.example/"));
   EXPECT(url_servers_count() == 2);
   snprintf(side, sizeof(side), "%s.tmp", path);
   EXPECT(!file_exists(side));
   snprintf(side, sizeof(side), "%s.bak", path);
   EXPECT(!file_exists(side));
   url_servers_set_path(path);
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://b.example/");

   /* Second rename fails (file held): the old file is put back. Calls:
    * 0 tmp->file (Windows refuses), 1 file->bak, 2 tmp->file (held),
    * 3 bak->file. */
   rename_seam_reset(true, 1u << 2);
   EXPECT(!url_servers_add("https://c.example/"));
   EXPECT(url_servers_count() == 2);
   EXPECT(file_has(path, "https://b.example/"));
   EXPECT(!file_has(path, "https://c.example/"));
   snprintf(side, sizeof(side), "%s.tmp", path);
   EXPECT(!file_exists(side));
   url_servers_set_path(path);
   EXPECT(url_servers_count() == 2);

   /* Putting it back fails too: neither rename works. Both copies stay
    * on disk and the next start uses the new one. */
   rename_seam_reset(true, (1u << 2) | (1u << 3));
   EXPECT(!url_servers_add("https://c.example/"));
   EXPECT(!file_exists(path));
   snprintf(side, sizeof(side), "%s.tmp", path);
   EXPECT(file_exists(side));
   snprintf(side, sizeof(side), "%s.bak", path);
   EXPECT(file_exists(side));
   rename_seam_reset(false, 0);
   url_servers_set_path(path);
   EXPECT(url_servers_count() == 3);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://c.example/");
   EXPECT(file_exists(path)); /* put back in place on load */

   /* Only the old copy (.bak) left: it is used. */
   EXPECT(file_exists(side));
   unlink(path);
   url_servers_set_path(path);
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://b.example/");
   EXPECT(file_exists(path));
   EXPECT(!file_exists(side));
   servers_cleanup(dir, path);
}

/* Same rules for the logins file. */
static void test_logins_windows_replace(void)
{
   char dir[]   = "/tmp/url_logins_test_XXXXXX";
   char path[512];
   char side[600];
   char user[64];
   char pass[64];
   if (!mkdtemp(dir))
   {
      EXPECT(!"mkdtemp");
      return;
   }
   snprintf(path, sizeof(path), "%s/url_logins.cfg", dir);
   url_logins_set_path(path);
   rename_seam_reset(true, 0);
   EXPECT(url_logins_set("https://a.example", "alice", "pw1"));
   EXPECT(url_logins_set("https://b.example", "testuser", "pw2"));
   EXPECT(url_logins_get("https://a.example", user, sizeof(user), pass, sizeof(pass)));
   EXPECT(url_logins_get("https://b.example", user, sizeof(user), pass, sizeof(pass)));
   EXPECT_STR(user, "testuser");

   /* Second rename held: the old file is put back. */
   rename_seam_reset(true, 1u << 2);
   EXPECT(!url_logins_set("https://c.example", "carol", "pw3"));
   EXPECT(url_logins_get("https://b.example", user, sizeof(user), pass, sizeof(pass)));
   EXPECT(!url_logins_get("https://c.example", user, sizeof(user), pass, sizeof(pass)));
   snprintf(side, sizeof(side), "%s.tmp", path);
   EXPECT(!file_exists(side));

   /* Putting it back fails too: no login is lost. */
   rename_seam_reset(true, (1u << 2) | (1u << 3));
   EXPECT(!url_logins_set("https://c.example", "carol", "pw3"));
   EXPECT(!file_exists(path));
   rename_seam_reset(false, 0);
   EXPECT(url_logins_get("https://a.example", user, sizeof(user), pass, sizeof(pass)));
   EXPECT_STR(pass, "pw1");
   EXPECT(url_logins_get("https://c.example", user, sizeof(user), pass, sizeof(pass)));
   EXPECT_STR(user, "carol");
   EXPECT(file_exists(path));
#ifdef __linux__
   {
      struct stat st;
      EXPECT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
   }
#endif

   /* Forget removes every copy. */
   snprintf(side, sizeof(side), "%s.bak", path);
   EXPECT(file_exists(side));
   EXPECT(url_logins_forget_all());
   EXPECT(!file_exists(side));
   snprintf(side, sizeof(side), "%s.tmp", path);
   EXPECT(!file_exists(side));
   EXPECT(!file_exists(path));
   rmdir(dir);
   url_logins_set_path(NULL);
}

static void test_servers_add_get(void)
{
   char dir[] = "/tmp/url_servers_test_XXXXXX";
   char path[512];
   char out[256];
   if (!servers_temp(dir, path, sizeof(path)))
      return;
   EXPECT(url_servers_count() == 0);
   EXPECT(!url_servers_get(0, out, sizeof(out)));
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(url_servers_add("https://b.example:8443/"));
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://b.example:8443/");
   EXPECT(url_servers_get(1, out, sizeof(out)));
   EXPECT_STR(out, "https://a.example/");
   EXPECT(!url_servers_get(2, out, sizeof(out)));
   /* Not a URL, or a value the line format cannot hold. */
   EXPECT(!url_servers_add(""));
   EXPECT(!url_servers_add(NULL));
   EXPECT(!url_servers_add("not a url"));
   EXPECT(!url_servers_add("https://x/\nhttps://y/"));
   EXPECT(url_servers_count() == 2);
   servers_cleanup(dir, path);
   /* No path: everything is off. */
   EXPECT(url_servers_count() == 0);
   EXPECT(!url_servers_add("https://a.example/"));
}

static void test_servers_dedupe(void)
{
   char dir[] = "/tmp/url_servers_test_XXXXXX";
   char path[512];
   char out[256];
   if (!servers_temp(dir, path, sizeof(path)))
      return;
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(url_servers_count() == 1);
   EXPECT(url_servers_add("https://b.example/"));
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://a.example/");
   EXPECT(url_servers_get(1, out, sizeof(out)));
   EXPECT_STR(out, "https://b.example/");
   /* Same server with a login in the link is still the same entry. */
   EXPECT(url_servers_add("https://u:p@b.example/"));
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://b.example/");
   servers_cleanup(dir, path);
}

static void test_servers_remove(void)
{
   char dir[] = "/tmp/url_servers_test_XXXXXX";
   char path[512];
   char out[256];
   if (!servers_temp(dir, path, sizeof(path)))
      return;
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(url_servers_add("https://b.example/"));
   EXPECT(url_servers_add("https://c.example/"));
   EXPECT(!url_servers_remove(3));
   EXPECT(url_servers_remove(1));
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://c.example/");
   EXPECT(url_servers_get(1, out, sizeof(out)));
   EXPECT_STR(out, "https://a.example/");
   /* Removal is saved. */
   url_servers_set_path(path);
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_remove(0));
   EXPECT(url_servers_remove(0));
   EXPECT(url_servers_count() == 0);
   EXPECT(!url_servers_remove(0));
   url_servers_set_path(path);
   EXPECT(url_servers_count() == 0);
   servers_cleanup(dir, path);
}

static void test_servers_persist(void)
{
   char dir[] = "/tmp/url_servers_test_XXXXXX";
   char path[512];
   char tmp[600];
   char out[256];
   if (!servers_temp(dir, path, sizeof(path)))
      return;
   EXPECT(!file_exists(path));
   EXPECT(url_servers_add("https://a.example/"));
   EXPECT(url_servers_add("http://192.168.1.5:8080/"));
   EXPECT(file_exists(path));
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   EXPECT(!file_exists(tmp));
   url_servers_set_path(NULL);
   EXPECT(url_servers_count() == 0);
   url_servers_set_path(path);
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "http://192.168.1.5:8080/");
   EXPECT(url_servers_get(1, out, sizeof(out)));
   EXPECT_STR(out, "https://a.example/");
   /* Hand-edited file: blank lines, CRLF and junk are skipped. */
   {
      FILE *f = fopen(path, "wb");
      if (f)
      {
         fputs("\r\nhttps://x.example/\r\n\nnot a url\nhttps://u:p@y.example/\n"
               "https://x.example/\n", f);
         fclose(f);
      }
   }
   url_servers_set_path(path);
   EXPECT(url_servers_count() == 2);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://x.example/");
   EXPECT(url_servers_get(1, out, sizeof(out)));
   EXPECT_STR(out, "https://y.example/");
   servers_cleanup(dir, path);
}

static void test_servers_strip_userinfo(void)
{
   char dir[] = "/tmp/url_servers_test_XXXXXX";
   char path[512];
   char out[256];
   char line[256];
   bool leaked = false;
   FILE *f;
   if (!servers_temp(dir, path, sizeof(path)))
      return;
   EXPECT(url_servers_add("https://u:p@h/"));
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://h/");
   EXPECT(url_servers_add("http://name:se@cret@h2:81/games/"));
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "http://h2:81/games/");
   if ((f = fopen(path, "rb")))
   {
      while (fgets(line, sizeof(line), f))
         if (strstr(line, "u:p") || strstr(line, "@") || strstr(line, "name"))
            leaked = true;
      fclose(f);
   }
   else
      leaked = true;
   EXPECT(!leaked);
   servers_cleanup(dir, path);
}

static void test_servers_cap(void)
{
   char dir[] = "/tmp/url_servers_test_XXXXXX";
   char path[512];
   char url[64];
   char out[256];
   int i;
   if (!servers_temp(dir, path, sizeof(path)))
      return;
   for (i = 0; i < URL_SERVERS_MAX + 1; i++)
   {
      snprintf(url, sizeof(url), "https://s%d.example/", i);
      EXPECT(url_servers_add(url));
   }
   EXPECT(url_servers_count() == URL_SERVERS_MAX);
   EXPECT(url_servers_get(0, out, sizeof(out)));
   EXPECT_STR(out, "https://s64.example/");
   EXPECT(url_servers_get(URL_SERVERS_MAX - 1, out, sizeof(out)));
   EXPECT_STR(out, "https://s1.example/");
   url_servers_set_path(path);
   EXPECT(url_servers_count() == URL_SERVERS_MAX);
   servers_cleanup(dir, path);
}

static void test_servers_root(void)
{
   char out[256];
   url_servers_root(out, sizeof(out), "https://h:8443/games/NES/");
   EXPECT_STR(out, "https://h:8443/");
   url_servers_root(out, sizeof(out), "https://u:p@Games.Example/a/b.gb");
   EXPECT_STR(out, "https://games.example/");
   url_servers_root(out, sizeof(out), "http://h");
   EXPECT_STR(out, "http://h/");
   url_servers_root(out, sizeof(out), "http://h?x=1");
   EXPECT_STR(out, "http://h/");
   url_servers_root(out, sizeof(out), "http://[::1]:8080/x/");
   EXPECT_STR(out, "http://[::1]:8080/");
   url_servers_root(out, sizeof(out), "not a url");
   EXPECT_STR(out, "");
   url_servers_root(out, sizeof(out), "https:///x");
   EXPECT_STR(out, "");
   url_servers_root(out, 8, "https://long.example/");
   EXPECT_STR(out, "");
}

/* ---------- fingerprints (#sha256=) ---------- */

#define FP_HEX "79a68194a5a1dc354264d70a556ff0a6acf1478d589a98cbb22bbb81fe55b5e5"

static void test_split_fingerprint(void)
{
   static const uint8_t want[32] = {
      0x79, 0xa6, 0x81, 0x94, 0xa5, 0xa1, 0xdc, 0x35,
      0x42, 0x64, 0xd7, 0x0a, 0x55, 0x6f, 0xf0, 0xa6,
      0xac, 0xf1, 0x47, 0x8d, 0x58, 0x9a, 0x98, 0xcb,
      0xb2, 0x2b, 0xbb, 0x81, 0xfe, 0x55, 0xb5, 0xe5 };
   char out[512];
   char upper[sizeof(FP_HEX)];
   uint8_t fp[32];
   bool has = true;
   size_t i;

   /* Valid: stripped, decoded. */
   memset(fp, 0, sizeof(fp));
   EXPECT(url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "https://h/a/game.nes#sha256=" FP_HEX));
   EXPECT_STR(out, "https://h/a/game.nes");
   EXPECT(has);
   EXPECT(!memcmp(fp, want, 32));

   /* Upper-case hex and key are the same fingerprint. */
   for (i = 0; FP_HEX[i]; i++)
      upper[i] = (FP_HEX[i] >= 'a' && FP_HEX[i] <= 'f')
         ? (char)(FP_HEX[i] - 'a' + 'A') : FP_HEX[i];
   upper[i] = '\0';
   snprintf(out, sizeof(out), "http://h/x?id=1#SHA256=%s", upper);
   memset(fp, 0, sizeof(fp));
   EXPECT(url_content_split_fingerprint(out, sizeof(out), fp, &has, out));
   EXPECT_STR(out, "http://h/x?id=1");
   EXPECT(has);
   EXPECT(!memcmp(fp, want, 32));

   /* No fragment: copied as is. */
   has = true;
   EXPECT(url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/game.gb"));
   EXPECT_STR(out, "http://h/game.gb");
   EXPECT(!has);

   /* Other fragments: stripped and ignored. */
   has = true;
   EXPECT(url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/game.gb#page=2"));
   EXPECT_STR(out, "http://h/game.gb");
   EXPECT(!has);
   has = true;
   EXPECT(url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/game.gb#"));
   EXPECT_STR(out, "http://h/game.gb");
   EXPECT(!has);
   has = true;
   EXPECT(url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/game.gb#md5=" FP_HEX));
   EXPECT_STR(out, "http://h/game.gb");
   EXPECT(!has);

   /* A broken fingerprint is refused, never silently dropped: wrong
    * length (63, 65), not hex, extra text after it. */
   EXPECT(!url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/g.gb#sha256=79a68194a5a1dc354264d70a556ff0a6acf1478d589a98cbb22bbb81fe55b5e"));
   EXPECT(!has);
   EXPECT(!url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/g.gb#sha256=" FP_HEX "0"));
   EXPECT(!url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/g.gb#sha256=g9a68194a5a1dc354264d70a556ff0a6acf1478d589a98cbb22bbb81fe55b5e5"));
   EXPECT(!url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/g.gb#sha256=" FP_HEX "&x=1"));
   EXPECT(!url_content_split_fingerprint(out, sizeof(out), fp, &has,
         "http://h/g.gb#sha256="));

   /* Does not fit: false, empty out. */
   EXPECT(!url_content_split_fingerprint(out, 8, fp, &has,
         "http://h/game.gb"));
   EXPECT_STR(out, "");

   /* Saved Servers keep no fragment. */
   url_servers_root(out, sizeof(out), "https://h:8443/cat/#sha256=" FP_HEX);
   EXPECT_STR(out, "https://h:8443/");
   url_servers_root(out, sizeof(out), "https://h#x");
   EXPECT_STR(out, "https://h/");
}

static void test_sha256_file(void)
{
   static const uint8_t abc[32] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
   char path[] = "/tmp/url_sha256.XXXXXX";
   uint8_t d[32];
   int fd = mkstemp(path);
   FILE *f;
   EXPECT(fd >= 0);
   if (fd < 0)
      return;
   f = fdopen(fd, "wb");
   fwrite("abc", 1, 3, f);
   fclose(f);
   memset(d, 0, sizeof(d));
   EXPECT(sha256_calculate(path, d) == 0);
   EXPECT(!memcmp(d, abc, 32));
   remove(path);
   EXPECT(sha256_calculate(path, d) != 0);
}

/* ---------- url_safety ---------- */

static const char *g_fixtures = NULL; /* made by make_archives.py */
static bool g_have_7z = false;

/* A realistic first-bytes buffer for each console, padded to 4096. */
static size_t make_head(uint8_t *b, size_t cap, const char *kind)
{
   size_t n = 4096;
   size_t i;
   for (i = 0; i < n && i < cap; i++)
      b[i] = (uint8_t)((i * 31 + 7) & 0xFF);
   if (!strcmp(kind, "gb"))
   {
      static const uint8_t logo[4] = {0xCE, 0xED, 0x66, 0x66};
      b[0x100] = 0x00; b[0x101] = 0xC3; b[0x102] = 0x50; b[0x103] = 0x01;
      memcpy(b + 0x104, logo, 4);
   }
   else if (!strcmp(kind, "gb-mz"))
   {
      /* A ROM whose first bytes happen to be "MZ", no PE header. */
      b[0] = 'M'; b[1] = 'Z';
      b[0x3C] = 0x10; b[0x3D] = 0x00; b[0x3E] = 0x00; b[0x3F] = 0x00;
      b[0x10] = 0x00; b[0x11] = 0x11; b[0x12] = 0x22; b[0x13] = 0x33;
   }
   else if (!strcmp(kind, "nes"))
      memcpy(b, "NES\x1a\x02\x01\x01\x00", 8);
   else if (!strcmp(kind, "smc"))
      memset(b, 0, 512);           /* copier header */
   else if (!strcmp(kind, "md"))
      memcpy(b + 0x100, "SEGA MEGA DRIVE ", 16);
   else if (!strcmp(kind, "iso"))
   {
      memset(b, 0, 4096);          /* system area is zeros */
   }
   else if (!strcmp(kind, "cue"))
   {
      const char *t = "FILE \"Game (Track 1).bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n";
      n = strlen(t);
      memcpy(b, t, n);
   }
   else if (!strcmp(kind, "chd"))
      memcpy(b, "MComprHD\x00\x00\x00\x7c\x00\x00\x00\x05", 16);
   else if (!strcmp(kind, "pbp"))
      memcpy(b, "\x00PBP\x00\x00\x01\x00", 8);
   else if (!strcmp(kind, "zip"))
      memcpy(b, "PK\x03\x04\x14\x00\x00\x00", 8);
   else if (!strcmp(kind, "7z"))
      memcpy(b, "7z\xbc\xaf\x27\x1c\x00\x04", 8);
   else if (!strcmp(kind, "bin"))
      memcpy(b, "\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00", 12);
   return n;
}

static void test_safety_blocks_programs(void)
{
   static const char *exts[] = {
      "exe", "com", "scr", "pif", "msi", "msp", "dll", "sys", "cpl",
      "bat", "cmd", "ps1", "psm1", "vbs", "vbe", "js", "jse", "wsf",
      "wsh", "hta", "jar", "lnk", "url", "reg", "inf", "scf", "app",
      "dmg", "pkg", "sh", "bash", "command", "desktop", "so", "dylib",
      "appimage", "apk", "deb", "rpm", NULL };
   uint8_t b[8192];
   size_t n;
   int i;

   /* PE: "MZ" ... "PE\0\0" at e_lfanew. */
   n = make_head(b, sizeof(b), "plain");
   b[0] = 'M'; b[1] = 'Z';
   b[0x3C] = 0x80; b[0x3D] = 0; b[0x3E] = 0; b[0x3F] = 0;
   memcpy(b + 0x80, "PE\0\0", 4);
   EXPECT(url_safety_check_file("game.gb", b, n) == URL_SAFE_PROGRAM);
   EXPECT(url_safety_check_file("game.zip", b, n) == URL_SAFE_PROGRAM);

   /* ELF. */
   n = make_head(b, sizeof(b), "plain");
   memcpy(b, "\x7f" "ELF\x02\x01\x01", 7);
   EXPECT(url_safety_check_file("sneaky.gb", b, n) == URL_SAFE_PROGRAM);

   /* Mach-O 64 (both byte orders) and 32, and fat. */
   n = make_head(b, sizeof(b), "plain");
   b[0] = 0xfe; b[1] = 0xed; b[2] = 0xfa; b[3] = 0xcf;
   EXPECT(url_safety_check_file("game.nes", b, n) == URL_SAFE_PROGRAM);
   b[0] = 0xcf; b[1] = 0xfa; b[2] = 0xed; b[3] = 0xfe;
   EXPECT(url_safety_check_file("game.nes", b, n) == URL_SAFE_PROGRAM);
   b[0] = 0xce; b[1] = 0xfa; b[2] = 0xed; b[3] = 0xfe;
   EXPECT(url_safety_check_file("game.nes", b, n) == URL_SAFE_PROGRAM);
   b[0] = 0xca; b[1] = 0xfe; b[2] = 0xba; b[3] = 0xbe;
   EXPECT(url_safety_check_file("game.sfc", b, n) == URL_SAFE_PROGRAM);

   /* Script. */
   n = make_head(b, sizeof(b), "plain");
   memcpy(b, "#!/bin/sh\n", 10);
   EXPECT(url_safety_check_file("game.md", b, n) == URL_SAFE_PROGRAM);

   /* Windows shortcut. */
   n = make_head(b, sizeof(b), "plain");
   memcpy(b, "\x4c\x00\x00\x00\x01\x14\x02\x00", 8);
   EXPECT(url_safety_check_file("game.iso", b, n) == URL_SAFE_PROGRAM);

   /* OLE / MSI. */
   n = make_head(b, sizeof(b), "plain");
   memcpy(b, "\xd0\xcf\x11\xe0", 4);
   EXPECT(url_safety_check_file("game.bin", b, n) == URL_SAFE_PROGRAM);

   /* Every listed extension, even with harmless bytes and any case. */
   n = make_head(b, sizeof(b), "gb");
   for (i = 0; exts[i]; i++)
   {
      char name[64];
      snprintf(name, sizeof(name), "0123456789abcdef_file.%s", exts[i]);
      EXPECT(url_safety_check_file(name, b, n) == URL_SAFE_PROGRAM);
      snprintf(name, sizeof(name), "FILE.%s", exts[i]);
      {
         char *p;
         for (p = name; *p; p++)
            if (*p >= 'a' && *p <= 'z')
               *p = (char)(*p - 'a' + 'A');
      }
      EXPECT(url_safety_check_file(name, b, n) == URL_SAFE_PROGRAM);
   }
   /* Windows ignores trailing dots and spaces. */
   EXPECT(url_safety_check_file("game.exe.", b, n) == URL_SAFE_PROGRAM);
   EXPECT(url_safety_check_file("game.exe ", b, n) == URL_SAFE_PROGRAM);

   /* A bare DOS MZ (no PE) is a program only as .exe/.com/.bat. */
   n = make_head(b, sizeof(b), "gb-mz");
   EXPECT(url_safety_check_file("game.exe", b, n) == URL_SAFE_PROGRAM);
   EXPECT(url_safety_check_file("game.com", b, n) == URL_SAFE_PROGRAM);
   EXPECT(url_safety_check_file("game.bat", b, n) == URL_SAFE_PROGRAM);

   /* A short file is the whole file. */
   EXPECT(url_safety_check_file("x.gb", (const uint8_t*)"\x7f" "ELF", 4)
         == URL_SAFE_PROGRAM);
}

static void test_safety_allows_games(void)
{
   static const char *names[][2] = {
      {"game.gb", "gb"}, {"game.gbc", "gb"}, {"game.nes", "nes"},
      {"game.sfc", "plain"}, {"game.smc", "smc"}, {"game.md", "md"},
      {"game.bin", "bin"}, {"game.cue", "cue"}, {"game.iso", "iso"},
      {"game.chd", "chd"}, {"game.pbp", "pbp"}, {"game.zip", "zip"},
      {"game.7z", "7z"}, {"GAME.GB", "gb"}, {"game.gb", "gb-mz"},
      {"0123456789abcdef_Game (World).sfc", "plain"},
      {"game", "plain"}, {NULL, NULL} };
   uint8_t b[8192];
   int i;
   for (i = 0; names[i][0]; i++)
   {
      size_t n = make_head(b, sizeof(b), names[i][1]);
      enum url_safety_verdict v = url_safety_check_file(names[i][0], b, n);
      if (v != URL_SAFE_OK)
         printf("  (blocked %s / %s: %d)\n", names[i][0], names[i][1], (int)v);
      EXPECT(v == URL_SAFE_OK);
   }
   /* Tiny files and an empty head are fine. */
   EXPECT(url_safety_check_file("x.gb", (const uint8_t*)"M", 1) == URL_SAFE_OK);
   EXPECT(url_safety_check_file("x.gb", (const uint8_t*)"MZ", 2) == URL_SAFE_OK);
   EXPECT(url_safety_check_file("x.gb", NULL, 0) == URL_SAFE_OK);
   /* "MZ" whose e_lfanew points past the head: not provably PE. */
   {
      size_t n = make_head(b, sizeof(b), "plain");
      b[0] = 'M'; b[1] = 'Z';
      b[0x3C] = 0x00; b[0x3D] = 0x00; b[0x3E] = 0x10; b[0x3F] = 0x00;
      EXPECT(url_safety_check_file("x.gb", b, n) == URL_SAFE_OK);
   }
   /* A name with no extension or a dot-only part. */
   EXPECT(url_safety_check_file("exe", b, 16) == URL_SAFE_OK);
   EXPECT(url_safety_check_file("game.exe.gb", b, 16) == URL_SAFE_OK);
}

static enum url_safety_verdict fixture(const char *name)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s", g_fixtures, name);
   return url_safety_check_archive(path);
}

#define EXPECT_FIX(name, verdict) do { \
   enum url_safety_verdict v_ = fixture(name); \
   if (v_ != (verdict)) \
      printf("  (%s gave %d)\n", name, (int)v_); \
   EXPECT(v_ == (verdict)); \
} while (0)

static void test_safety_archive_zip(void)
{
   EXPECT_FIX("ok.zip", URL_SAFE_OK);
   EXPECT_FIX("ok_stored.zip", URL_SAFE_OK);
   EXPECT_FIX("ok_zip64.zip", URL_SAFE_OK);
   EXPECT_FIX("dosgame.zip", URL_SAFE_OK);
   EXPECT_FIX("many_ok.zip", URL_SAFE_OK);
   EXPECT_FIX("tiny_ratio.zip", URL_SAFE_OK);
   EXPECT_FIX("exactly2g.zip", URL_SAFE_OK);
   /* Ruling R2: DOS-world names are judged by their bytes. */
   EXPECT_FIX("dos_world.zip", URL_SAFE_OK);
   EXPECT_FIX("app_member.zip", URL_SAFE_OK);  /* harmless bytes */
   EXPECT_FIX("dos_world_pe.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("vbs_member.zip", URL_SAFE_ARCHIVE_TRAP);

   EXPECT_FIX("slip_dotdot.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("slip_abs.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("slip_drive.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("slip_bslash.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("pe_member.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("pe_named_bin.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("pe_stored.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("pe_far.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("elf_member.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("lnk_member.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("script_member.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("bomb.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("many.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("toobig.zip", URL_SAFE_ARCHIVE_TOO_BIG);
   EXPECT_FIX("broken.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("missing.zip", URL_SAFE_UNREADABLE);

   /* Review C1/C2: read exactly what RetroArch's reader reads, refuse
    * where the two could disagree. */
   EXPECT_FIX("two_eocd.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("csize_lie.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("csize_honest.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("encrypted_flag.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("no_local_sig.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("eocd_not_at_end.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("eocd_in_comment.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("zip64_marker.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("usize_ffff.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("dir_with_pe.zip", URL_SAFE_ARCHIVE_TRAP);
   /* Re-review I1: fewer bytes in the file than the declared size. */
   EXPECT_FIX("stored_past_eof.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("stored_past_eof_bin.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("deflate_short.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("deflate_short_bin.zip", URL_SAFE_UNREADABLE);
   EXPECT_FIX("deflate_short_far.zip", URL_SAFE_UNREADABLE);
   /* Ruling R6 + minors: .cmd always, no ':' in member names. */
   EXPECT_FIX("cmd_member.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("stream_name.zip", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("colon_name.zip", URL_SAFE_ARCHIVE_TRAP);
}

static void test_safety_archive_7z(void)
{
   if (!g_have_7z)
   {
      printf("SKIP: .7z archive tests (no 7z tool made the fixtures)\n");
      return;
   }
   EXPECT_FIX("ok.7z", URL_SAFE_OK);
   EXPECT_FIX("dosgame.7z", URL_SAFE_OK);
   EXPECT_FIX("pe.7z", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("pe_named_bin.7z", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("elf.7z", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("lnk.7z", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("bomb.7z", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("many.7z", URL_SAFE_ARCHIVE_TRAP);
   EXPECT_FIX("broken.7z", URL_SAFE_UNREADABLE);
}

static void test_safety_archive_other(void)
{
   char path[1024];
   /* Not an archive by name: nothing to check. */
   snprintf(path, sizeof(path), "%s/ok.zip", g_fixtures);
   EXPECT(url_safety_check_archive("/nonexistent/game.gb") == URL_SAFE_OK);
   /* Upper-case extension is still a zip. */
   {
      char up[1024];
      snprintf(up, sizeof(up), "%s/SLIP.ZIP", g_fixtures);
      {
         char src[1024];
         FILE *in, *out;
         char buf[4096];
         size_t got;
         snprintf(src, sizeof(src), "%s/slip_dotdot.zip", g_fixtures);
         in  = fopen(src, "rb");
         out = fopen(up, "wb");
         while (in && out && (got = fread(buf, 1, sizeof(buf), in)) > 0)
            fwrite(buf, 1, got, out);
         if (in) fclose(in);
         if (out) fclose(out);
      }
      EXPECT(url_safety_check_archive(up) == URL_SAFE_ARCHIVE_TRAP);
      remove(up);
   }
   EXPECT(url_safety_check_archive(NULL) == URL_SAFE_UNREADABLE);
}

static void test_origin(void)
{
   char out[300];
   EXPECT(url_content_origin(out, sizeof(out), "https://Example.COM/a/b"));
   EXPECT_STR(out, "https://example.com");
   EXPECT(url_content_origin(out, sizeof(out), "HTTPS://example.com:443"));
   EXPECT_STR(out, "https://example.com");
   EXPECT(url_content_origin(out, sizeof(out), "http://example.com:80/x"));
   EXPECT_STR(out, "http://example.com");
   EXPECT(url_content_origin(out, sizeof(out), "http://example.com:443/x"));
   EXPECT_STR(out, "http://example.com:443");
   EXPECT(url_content_origin(out, sizeof(out), "https://example.com:8443?x=1"));
   EXPECT_STR(out, "https://example.com:8443");
   EXPECT(url_content_origin(out, sizeof(out), "https://example.com:08443/"));
   EXPECT_STR(out, "https://example.com:8443");
   EXPECT(url_content_origin(out, sizeof(out), "https://example.com:/x"));
   EXPECT_STR(out, "https://example.com");
   EXPECT(url_content_origin(out, sizeof(out), "https://u:p@nas.local:8443/x#y"));
   EXPECT_STR(out, "https://nas.local:8443");
   EXPECT(url_content_origin(out, sizeof(out), "https://[::1]:8443/x"));
   EXPECT_STR(out, "https://[::1]:8443");
   EXPECT(url_content_origin(out, sizeof(out), "https://[::1]/x"));
   EXPECT_STR(out, "https://[::1]");
   /* Not an http(s) site, or no host, or a broken port. */
   EXPECT(!url_content_origin(out, sizeof(out), "ftp://example.com/"));
   EXPECT_STR(out, "");
   EXPECT(!url_content_origin(out, sizeof(out), "example.com"));
   EXPECT(!url_content_origin(out, sizeof(out), "https:///x"));
   EXPECT(!url_content_origin(out, sizeof(out), "https://example.com:80a/"));
   EXPECT(!url_content_origin(out, sizeof(out), "https://example.com:99999/"));
   EXPECT(!url_content_origin(out, sizeof(out), NULL));
   EXPECT(!url_content_origin(out, 10, "https://example.com"));
}

/* Logins saved by older builds: one entry per bare host
 * (url_login_<host with . : [ as _>_user/_pass). They count as the
 * https site on the default port only, and are moved to the new key
 * when that site's login is saved again (or migrated after use). */
static void test_logins_legacy(void)
{
   char dir[]   = "/tmp/url_logins_legacy_XXXXXX";
   char path[512];
   char user[128];
   char pass[128];
   char access[128];
   char secret[128];
   char text[2048];
   size_t n;
   FILE *f;

   if (!mkdtemp(dir))
   {
      EXPECT(!"mkdtemp");
      return;
   }
   snprintf(path, sizeof(path), "%s/url_logins.cfg", dir);
   if ((f = fopen(path, "wb")))
   {
      fputs("url_login_example_com_user = \"alice\"\n"
            "url_login_example_com_pass = \"pw1\"\n"
            "url_login_nas_local_user = \"testuser\"\n"
            "url_login_nas_local_pass = \"pw2\"\n"
            "url_login_127_0_0_1_user = \"carol\"\n"
            "url_login_127_0_0_1_pass = \"pw3\"\n"
            "url_login_archive_access = \"AK\"\n"
            "url_login_archive_secret = \"SK\"\n", f);
      fclose(f);
   }
   url_logins_set_path(path);

   EXPECT(url_logins_get("https://example.com/x.sfc", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "alice");
   EXPECT_STR(pass, "pw1");
   EXPECT(url_logins_get("https://127.0.0.1", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "carol");
   /* Not for another port or for plain http. */
   EXPECT(!url_logins_get("https://example.com:8443", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get("http://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get("https://127.0.0.1:8443", user, sizeof(user),
         pass, sizeof(pass)));

   /* Saving that site again: the new key replaces the old one. */
   EXPECT(url_logins_set("https://example.com", "alice", "new"));
   EXPECT(url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(pass, "new");
   /* Migrating a site whose old login worked. */
   EXPECT(url_logins_migrate("https://nas.local"));
   EXPECT(url_logins_get("https://nas.local", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "testuser");
   EXPECT_STR(pass, "pw2");
   /* Nothing to migrate: fine, nothing changes. */
   EXPECT(url_logins_migrate("https://unknown.example"));
   EXPECT(!url_logins_migrate("http://nas.local"));

   text[0] = '\0';
   if ((f = fopen(path, "rb")))
   {
      n = fread(text, 1, sizeof(text) - 1, f);
      text[n] = '\0';
      fclose(f);
   }
   EXPECT(strstr(text, "url_login_example_com_") == NULL);
   EXPECT(strstr(text, "url_login_nas_local_") == NULL);
   EXPECT(strstr(text, "url_login_https://example.com_pass") != NULL);
   EXPECT(strstr(text, "url_login_https://nas.local_user") != NULL);
   /* Other old entries and the archive.org keys are left as they are. */
   EXPECT(strstr(text, "url_login_127_0_0_1_user") != NULL);
   EXPECT(url_logins_get("https://127.0.0.1", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "carol");
   EXPECT(url_logins_get_archive_keys(access, sizeof(access),
         secret, sizeof(secret)));
   EXPECT_STR(access, "AK");
   EXPECT_STR(secret, "SK");

   EXPECT(url_logins_forget_all());
   url_logins_set_path(NULL);
   rmdir(dir);
}

/* Cloud Sync never uploads the logins (and their side files) or the
 * saved servers. */
/* Only well-formed base64 is ever decoded (libretro's unbase64 trusts
 * its input). */
static void test_logins_base64_ok(void)
{
   EXPECT(url_logins_base64_ok("AAAA"));
   EXPECT(url_logins_base64_ok("bm90IHNlYWxlZA=="));
   EXPECT(url_logins_base64_ok("YWJj"));
   EXPECT(url_logins_base64_ok("YWI="));
   EXPECT(url_logins_base64_ok("a+b/"));
   EXPECT(!url_logins_base64_ok("AAAA="));
   EXPECT(!url_logins_base64_ok("x=="));
   EXPECT(!url_logins_base64_ok("x="));
   EXPECT(!url_logins_base64_ok("="));
   EXPECT(!url_logins_base64_ok("===="));
   EXPECT(!url_logins_base64_ok("A==="));
   EXPECT(!url_logins_base64_ok("AA=A"));
   EXPECT(!url_logins_base64_ok("AAA"));
   EXPECT(!url_logins_base64_ok("%%%%"));
   EXPECT(!url_logins_base64_ok("AA AA=="));
   EXPECT(!url_logins_base64_ok("AAAA\n"));
   EXPECT(!url_logins_base64_ok(""));
   EXPECT(!url_logins_base64_ok(NULL));
   /* What passes is safe to decode (run under ASan by hand:
    * make CFLAGS+=-fsanitize=address test). */
   {
      static const char *in[] = { "AAAA", "YWI=", "YWJjZA==", "AAAA=",
         "x==", "x=", "====", "A===", NULL };
      int i;
      for (i = 0; in[i]; i++)
         if (url_logins_base64_ok(in[i]))
         {
            int n = 0;
            unsigned char *b = unbase64(in[i], (int)strlen(in[i]), &n);
            EXPECT(b != NULL && n >= 1 && n <= 6);
            free(b);
         }
   }
}

static void test_logins_private_files(void)
{
   EXPECT(url_logins_is_private_file("config/url_logins.cfg"));
   EXPECT(url_logins_is_private_file("config/url_logins.cfg.tmp"));
   EXPECT(url_logins_is_private_file("config/url_logins.cfg.bak"));
   EXPECT(url_logins_is_private_file("config/url_servers.cfg"));
   EXPECT(url_logins_is_private_file("config/url_servers.cfg.tmp"));
   EXPECT(url_logins_is_private_file("config/url_servers.cfg.bak"));
   EXPECT(url_logins_is_private_file("config/sub/URL_Logins.cfg"));
   EXPECT(url_logins_is_private_file("url_logins.cfg"));
   EXPECT(!url_logins_is_private_file("config/retroarch-core-options.cfg"));
   EXPECT(!url_logins_is_private_file("config/my_url_logins.cfg"));
   EXPECT(!url_logins_is_private_file("saves/url_logins.cfg.srm"));
   EXPECT(!url_logins_is_private_file(""));
   EXPECT(!url_logins_is_private_file(NULL));
}

static void test_safety_zone_text(void)
{
   char t[512];
   url_safety_zone_text(t, sizeof(t), "https://user:pw@example.com:8443/a/b.gb?x=1#sha256=ab");
   EXPECT_STR(t, "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://example.com:8443/a/b.gb?x=1\r\n");
   url_safety_zone_text(t, sizeof(t), "http://example.com/x@y.gb");
   EXPECT_STR(t, "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=http://example.com/x@y.gb\r\n");
   url_safety_zone_text(t, sizeof(t), "http://a@b@example.com");
   EXPECT_STR(t, "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=http://example.com\r\n");
   url_safety_zone_text(t, sizeof(t), "http://example.com/a\r\nZoneId=0");
   EXPECT_STR(t, "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=http://example.com/aZoneId=0\r\n");
   /* Marking is a no-op off Windows and never crashes. */
   url_safety_mark_downloaded("/nonexistent/x.gb", "http://example.com/x.gb");
   url_safety_mark_downloaded(NULL, NULL);
}

int main(int argc, char **argv)
{
   test_fix_link();
   test_cache_name();
   test_disposition();
   test_host();
   test_origin();
   test_wipe();
   test_basic_auth();
   test_digest_auth();
   test_archive_auth();
   test_is_downloadable();
   test_looks_like_html();
   test_logins();
   test_form_encode();
   test_archive_login_parse();
   test_cli_should_fetch();
   test_clipboard_clean();
   test_content_resolve();
   test_catalog_parse_basic();
   test_catalog_parse_sort();
   test_catalog_parse_unsafe();
   test_catalog_parse_dir_slash();
   test_catalog_parse_names();
   test_catalog_parse_edges();
   test_catalog_parse_not_listing();
   test_catalog_parse_empty();
   test_catalog_parse_cap();
   test_catalog_parent();
   test_catalog_title();
   test_servers_add_get();
   test_servers_dedupe();
   test_servers_remove();
   test_servers_persist();
   test_servers_strip_userinfo();
   test_servers_cap();
   test_servers_root();
   test_servers_add_top_no_write();
   test_servers_windows_replace();
   test_logins_windows_replace();
   test_logins_legacy();
   test_logins_private_files();
   test_logins_base64_ok();
   test_safety_blocks_programs();
   test_safety_allows_games();
   test_safety_zone_text();
   test_split_fingerprint();
   test_sha256_file();
   if (argc >= 2)
   {
      char probe[1024];
      FILE *f;
      g_fixtures = argv[1];
      snprintf(probe, sizeof(probe), "%s/ok.7z", g_fixtures);
      if ((f = fopen(probe, "rb")))
      {
         g_have_7z = true;
         fclose(f);
      }
      test_safety_archive_zip();
      test_safety_archive_7z();
      test_safety_archive_other();
   }
   else
      printf("SKIP: archive tests (no fixture folder given)\n");

   if (g_failures)
   {
      printf("url_content: %d of %d checks FAILED\n", g_failures, g_checks);
      return 1;
   }
   printf("url_content: all %d checks passed\n", g_checks);
   return 0;
}
