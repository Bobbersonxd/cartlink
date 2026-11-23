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
#include "../../frontend/clipboard.h"

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
         "Content-Disposition: attachment; filename=\"Zelda.zip\""));
   EXPECT_STR(out, "Zelda.zip");

   EXPECT(url_content_name_from_disposition(out, sizeof(out),
         "Content-Disposition: attachment; filename*=UTF-8''Zelda%20DX.zip"));
   EXPECT_STR(out, "Zelda DX.zip");

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
   url_content_basic_auth_header(out, sizeof(out), "bob", "pw");
   EXPECT_STR(out, "Authorization: Basic Ym9iOnB3\r\n");
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
   EXPECT(!url_content_is_downloadable("/home/a.sfc"));
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
   EXPECT(!url_logins_get("example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get_archive_keys(access, sizeof(access),
         secret, sizeof(secret)));
   EXPECT(url_logins_forget_all());
   EXPECT(!file_exists(path));

   /* Round trip, two hosts. */
   EXPECT(url_logins_set("example.com", "alice", "s3cret pass"));
   EXPECT(url_logins_set("files.my-nas.local", "bob", "hunter2"));
   EXPECT(file_exists(path));
#ifndef _WIN32
   /* Windows has no POSIX file modes; url_logins skips them there. */
   EXPECT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
#endif

   user[0] = pass[0] = '\0';
   EXPECT(url_logins_get("example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "alice");
   EXPECT_STR(pass, "s3cret pass");
   EXPECT(url_logins_get("files.my-nas.local", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "bob");
   EXPECT_STR(pass, "hunter2");

   /* Key format: dots become '_'. */
   if ((f = fopen(path, "r")))
   {
      while (fgets(line, sizeof(line), f))
         if (strncmp(line, "url_login_example_com_user", 26) == 0)
            saw_host_key = true;
      fclose(f);
   }
   EXPECT(saw_host_key);

   /* Second set replaces the first; other host untouched. */
   EXPECT(url_logins_set("example.com", "carol", "new"));
   EXPECT(url_logins_get("example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "carol");
   EXPECT_STR(pass, "new");
   EXPECT(url_logins_get("files.my-nas.local", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "bob");

   /* Unknown host. */
   EXPECT(!url_logins_get("unknown.org", user, sizeof(user),
         pass, sizeof(pass)));

   /* Values the file format cannot hold are refused. */
   EXPECT(!url_logins_set("example.com", "x", "has\"quote"));
   EXPECT(!url_logins_set("example.com", "x", "line\nbreak"));
   EXPECT(!url_logins_set(NULL, "x", "y"));
   EXPECT(!url_logins_set("", "x", "y"));
   EXPECT(url_logins_get("example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(pass, "new");

   /* Archive keys round trip. */
   EXPECT(url_logins_set_archive_keys("AKEY123", "SKEY456"));
   access[0] = secret[0] = '\0';
   EXPECT(url_logins_get_archive_keys(access, sizeof(access),
         secret, sizeof(secret)));
   EXPECT_STR(access, "AKEY123");
   EXPECT_STR(secret, "SKEY456");
   EXPECT(url_logins_get("example.com", user, sizeof(user),
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
      EXPECT(url_logins_set("example.com", "dave", "pw2"));
      EXPECT(!file_exists(tmp));
      EXPECT(url_logins_get("example.com", user, sizeof(user),
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
         EXPECT(!url_logins_set("example.com", "eve", "pw3"));
         remove(inner);
      }
      EXPECT(url_logins_get("example.com", user, sizeof(user),
            pass, sizeof(pass)));
      EXPECT_STR(user, "dave");
      EXPECT_STR(pass, "pw2");
      rmdir(tmp);
#endif
      EXPECT(url_logins_set("example.com", "carol", "new"));
   }

   /* Forget all: file gone, every get false. */
   EXPECT(url_logins_forget_all());
   EXPECT(!file_exists(path));
   EXPECT(!url_logins_get("example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get("files.my-nas.local", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT(!url_logins_get_archive_keys(access, sizeof(access),
         secret, sizeof(secret)));

   /* No path set: everything fails safely. */
   url_logins_set_path(NULL);
   EXPECT(!url_logins_set("example.com", "a", "b"));
   EXPECT(!url_logins_get("example.com", user, sizeof(user),
         pass, sizeof(pass)));

   remove(path);
   rmdir(dir);
}

static void test_form_encode(void)
{
   char out[64];

   EXPECT(url_content_form_encode(out, sizeof(out), "bob@example.com"));
   EXPECT_STR(out, "bob%40example.com");
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
      " \"logged-in-user\": \"bob%40example.com\"}, \"email\": \"bob@example.com\","
      " \"itemname\": \"@bob\", \"s3\": {\"access\": \"AKEY123\","
      " \"secret\": \"SKEY456\"}, \"screenname\": \"bob\"}, \"version\": 1}";
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
      "{\"success\": true, \"values\": {\"email\": \"bob@example.com\"}}";
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

int main(void)
{
   test_fix_link();
   test_cache_name();
   test_disposition();
   test_host();
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
   test_catalog_parse_names();
   test_catalog_parse_edges();
   test_catalog_parse_not_listing();
   test_catalog_parse_empty();
   test_catalog_parse_cap();
   test_catalog_parent();

   if (g_failures)
   {
      printf("url_content: %d of %d checks FAILED\n", g_failures, g_checks);
      return 1;
   }
   printf("url_content: all %d checks passed\n", g_checks);
   return 0;
}
