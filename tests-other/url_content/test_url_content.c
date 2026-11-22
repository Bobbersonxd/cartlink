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
#include <string.h>

#include "../../network/url_content.h"

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

static void test_basic_auth(void)
{
   char out[256];
   url_content_basic_auth_header(out, sizeof(out), "bob", "pw");
   EXPECT_STR(out, "Authorization: Basic Ym9iOnB3\r\n");
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

int main(void)
{
   test_fix_link();
   test_cache_name();
   test_disposition();
   test_host();
   test_basic_auth();
   test_digest_auth();
   test_is_downloadable();
   test_looks_like_html();

   if (g_failures)
   {
      printf("url_content: %d of %d checks FAILED\n", g_failures, g_checks);
      return 1;
   }
   printf("url_content: all %d checks passed\n", g_checks);
   return 0;
}
