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

/* Windows-only check of network/url_logins.c: values sealed with DPAPI
 * (current user), the "# cartlink-logins v2 dpapi" marker, an older
 * plain file read and sealed on the next save. Built with MinGW and
 * run under Wine by "make win-test".
 * Usage: test_logins_win.exe <empty folder> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../network/url_logins.h"

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

static char g_text[8192];

/* The whole file into g_text ("" if missing). */
static const char *slurp(const char *path)
{
   size_t n = 0;
   FILE *f  = fopen(path, "rb");
   g_text[0] = '\0';
   if (f)
   {
      n = fread(g_text, 1, sizeof(g_text) - 1, f);
      fclose(f);
   }
   g_text[n] = '\0';
   return g_text;
}

static void put(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   fputs(text, f);
   fclose(f);
}

int main(int argc, char **argv)
{
   char path[1024];
   char user[128];
   char pass[128];
   char a[128];
   char s[128];
   const char *t;

   if (argc < 2)
   {
      printf("usage: %s <empty folder>\n", argv[0]);
      return 2;
   }
   snprintf(path, sizeof(path), "%s\\url_logins.cfg", argv[1]);
   remove(path);
   url_logins_set_path(path);

   /* New file: sealed values behind the marker; the plain text of the
    * login is nowhere in it. */
   EXPECT(url_logins_set("https://example.com", "alice", "s3cret pass"));
   EXPECT(url_logins_set_archive_keys("AKEY123", "SKEY456"));
   t = slurp(path);
   EXPECT(strncmp(t, "# cartlink-logins v2 dpapi\n", 27) == 0);
   EXPECT(strstr(t, "url_login_https://example.com_user = \"") != NULL);
   EXPECT(strstr(t, "alice") == NULL);
   EXPECT(strstr(t, "s3cret") == NULL);
   EXPECT(strstr(t, "AKEY123") == NULL);
   EXPECT(strstr(t, "SKEY456") == NULL);
   EXPECT(url_logins_get("https://example.com/x.sfc", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "alice");
   EXPECT_STR(pass, "s3cret pass");
   EXPECT(url_logins_get_archive_keys(a, sizeof(a), s, sizeof(s)));
   EXPECT_STR(a, "AKEY123");
   EXPECT_STR(s, "SKEY456");
   EXPECT(!url_logins_get("https://example.com:8443", user, sizeof(user),
         pass, sizeof(pass)));

   /* Replacing one keeps the others readable. */
   EXPECT(url_logins_set("https://example.com", "carol", "new"));
   EXPECT(url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "carol");
   EXPECT_STR(pass, "new");
   EXPECT(url_logins_get_archive_keys(a, sizeof(a), s, sizeof(s)));
   EXPECT_STR(a, "AKEY123");

   /* A value that is not a sealed blob (edited, or from another user):
    * no login, nothing crashes. */
   put(path, "# cartlink-logins v2 dpapi\n"
         "url_login_https://example.com_user = \"bm90IHNlYWxlZA==\"\n"
         "url_login_https://example.com_pass = \"%%%\"\n");
   EXPECT(!url_logins_get("https://example.com", user, sizeof(user),
         pass, sizeof(pass)));

   /* An older build's plain file (no marker, host-only keys): read as
    * is, then sealed as a whole on the next save. */
   put(path, "url_login_nas_local_user = \"bob\"\n"
         "url_login_nas_local_pass = \"hunter2\"\n"
         "url_login_archive_access = \"OLDA\"\n"
         "url_login_archive_secret = \"OLDS\"\n");
   EXPECT(url_logins_get("https://nas.local", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(user, "bob");
   EXPECT_STR(pass, "hunter2");
   EXPECT(url_logins_set("https://other.example", "dave", "pw4"));
   t = slurp(path);
   EXPECT(strncmp(t, "# cartlink-logins v2 dpapi\n", 27) == 0);
   EXPECT(strstr(t, "hunter2") == NULL);
   EXPECT(strstr(t, "OLDS") == NULL);
   EXPECT(strstr(t, "pw4") == NULL);
   EXPECT(url_logins_get("https://nas.local", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(pass, "hunter2");
   EXPECT(url_logins_get("https://other.example", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(pass, "pw4");
   EXPECT(url_logins_get_archive_keys(a, sizeof(a), s, sizeof(s)));
   EXPECT_STR(s, "OLDS");
   /* Migrating the old entry keeps it sealed. */
   EXPECT(url_logins_migrate("https://nas.local"));
   t = slurp(path);
   EXPECT(strstr(t, "url_login_https://nas.local_user") != NULL);
   EXPECT(strstr(t, "url_login_nas_local_") == NULL);
   EXPECT(strstr(t, "hunter2") == NULL);
   EXPECT(url_logins_get("https://nas.local", user, sizeof(user),
         pass, sizeof(pass)));
   EXPECT_STR(pass, "hunter2");

   EXPECT(url_logins_forget_all());
   EXPECT(slurp(path)[0] == '\0');
   url_logins_set_path(NULL);

   if (g_failures)
   {
      printf("url_logins (Windows): %d of %d checks FAILED\n",
            g_failures, g_checks);
      return 1;
   }
   printf("url_logins (Windows): all %d checks passed\n", g_checks);
   return 0;
}
