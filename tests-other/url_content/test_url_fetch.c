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

/* Tests for network/url_fetch.c against serve.py on 127.0.0.1.
 * Usage: test_url_fetch <port> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

#include "../../network/url_fetch.h"
#include "../../network/url_logins.h"

static int g_checks   = 0;
static int g_failures = 0;
static int g_port     = 0;
static char g_root[256];
static char g_logins[300];

#define EXPECT(x) do { \
   g_checks++; \
   if (!(x)) { \
      g_failures++; \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
   } \
} while (0)

static unsigned char g_game[8192];

static void make_game(void)
{
   int i;
   for (i = 0; i < 8192; i++)
      g_game[i] = (unsigned char)((i * 7 + 3) & 0xFF);
}

static void url(char *out, size_t len, const char *path)
{
   snprintf(out, len, "http://127.0.0.1:%d%s", g_port, path);
}

static void new_cache(char *out, size_t len, const char *name)
{
   snprintf(out, len, "%s/%s", g_root, name);
}

/* Number of entries in dir; first name copied into first. */
static int count_files(const char *dir, char *first, size_t flen,
      int *parts)
{
   int n = 0;
   struct dirent *e;
   DIR *d = opendir(dir);
   if (parts)
      *parts = 0;
   if (first && flen)
      first[0] = '\0';
   if (!d)
      return 0;
   while ((e = readdir(d)))
   {
      size_t l;
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
         continue;
      if (first && n == 0)
         snprintf(first, flen, "%s", e->d_name);
      l = strlen(e->d_name);
      if (parts && l > 5 && !strcmp(e->d_name + l - 5, ".part"))
         (*parts)++;
      n++;
   }
   closedir(d);
   return n;
}

static long file_size(const char *path)
{
   struct stat st;
   if (stat(path, &st) != 0)
      return -1;
   return (long)st.st_size;
}

static int file_is_game(const char *path)
{
   unsigned char buf[8193];
   size_t n;
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   n = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   return n == sizeof(g_game) && !memcmp(buf, g_game, n);
}

static int ends_with(const char *s, const char *suffix)
{
   size_t ls = strlen(s);
   size_t lx = strlen(suffix);
   return ls >= lx && !strcmp(s + ls - lx, suffix);
}

static enum url_fetch_result fetch(const char *path, const char *cache,
      const char *user, const char *pass, char *out, size_t out_len,
      char *ch, size_t ch_len)
{
   char u[256];
   char err[256];
   url(u, sizeof(u), path);
   return url_fetch_to_cache(u, cache, user, pass, out, out_len,
         ch, ch_len, err, sizeof(err), NULL, NULL);
}

static size_t g_prog_calls = 0;
static size_t g_prog_last  = 0;

static void prog_cb(size_t done, size_t total, void *ud)
{
   g_prog_calls++;
   g_prog_last = done;
   (void)total;
   (void)ud;
}

static void test_ok_and_cached(void)
{
   char cache[300], out[512], out2[512], first[256], u[256], err[256];
   char base[256];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "ok");

   url(u, sizeof(u), "/game.sfc");
   r = url_fetch_to_cache(u, cache, NULL, NULL, out, sizeof(out),
         NULL, 0, err, sizeof(err), prog_cb, NULL);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
   EXPECT(ends_with(out, "_game.sfc"));
   EXPECT(g_prog_calls > 0);
   EXPECT(count_files(cache, first, sizeof(first), NULL) == 1);
   snprintf(base, sizeof(base), "%s", first);

   r = fetch("/game.sfc", cache, NULL, NULL, out2, sizeof(out2), NULL, 0);
   EXPECT(r == URL_FETCH_CACHED);
   EXPECT(!strcmp(out, out2));
   EXPECT(count_files(cache, NULL, 0, NULL) == 1);

   /* Same URL in a fresh cache: same name, identical bytes. */
   new_cache(cache, sizeof(cache), "ok2");
   r = fetch("/game.sfc", cache, NULL, NULL, out2, sizeof(out2), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out2));
   EXPECT(count_files(cache, first, sizeof(first), NULL) == 1);
   EXPECT(!strcmp(first, base));
}

static void test_leftover_part_removed(void)
{
   char cache[300], out[512], first[256], part[600];
   int parts = 0;
   FILE *f;
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "leftover");

   /* Learn the hash prefix, then fake a stale .part next to nothing. */
   r = fetch("/game.sfc", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   count_files(cache, first, sizeof(first), NULL);
   snprintf(part, sizeof(part), "%s/%.16s_stale.bin.part", cache, first);
   remove(out);
   f = fopen(part, "wb");
   if (f)
   {
      fputs("junk", f);
      fclose(f);
   }
   r = fetch("/game.sfc", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(count_files(cache, NULL, 0, &parts) == 1);
   EXPECT(parts == 0);
}

static void test_nodisp(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "nodisp");
   r = fetch("/nodisp?id=1", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(ends_with(out, "_Named.gba"));
   EXPECT(file_is_game(out));
}

static void test_page(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "page");
   r = fetch("/page", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_NOT_A_FILE);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
}

static void test_basic(void)
{
   char cache[300], out[512], ch[256];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "basic");
   r = fetch("/basic/game.sfc", cache, NULL, NULL, out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_NEED_LOGIN);
   EXPECT(strstr(ch, "Basic") != NULL);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);

   r = fetch("/basic/game.sfc", cache, "bob", "nope", out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_NEED_LOGIN);

   r = fetch("/basic/game.sfc", cache, "bob", "pw", out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
}

static void test_saved_login(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "saved");
   EXPECT(url_logins_set("127.0.0.1", "bob", "pw"));
   r = fetch("/basic/game.sfc", cache, NULL, NULL, out, sizeof(out),
         NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
   EXPECT(url_logins_forget_all());
}

static void test_digest(void)
{
   char cache[300], out[512], ch[256];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "digest");
   r = fetch("/digest/game.sfc", cache, NULL, NULL, out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_NEED_LOGIN);
   EXPECT(strstr(ch, "Digest") != NULL);

   r = fetch("/digest/game.sfc", cache, "bob", "wrong", out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_NEED_LOGIN);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);

   r = fetch("/digest/game.sfc", cache, "bob", "pw", out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
}

static void test_redirect(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "redirect");
   r = fetch("/redirect", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
}

static void test_drop(void)
{
   char cache[300], out[512];
   int parts = 0;
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "drop");
   r = fetch("/drop", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT(count_files(cache, NULL, 0, &parts) == 0);
   EXPECT(parts == 0);
}

static void test_missing(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "missing");
   r = fetch("/nothing-here.bin", cache, NULL, NULL, out, sizeof(out),
         NULL, 0);
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
}

static void test_big(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "big");
   r = fetch("/big", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK || r == URL_FETCH_TOO_BIG);
   if (r == URL_FETCH_OK)
   {
      EXPECT(file_size(out) == 300L * 1024L * 1024L);
      remove(out);
   }
}

/* 1 if the file holds exactly text. */
static int file_is_text(const char *path, const char *text)
{
   char buf[512];
   size_t n;
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   return n == strlen(text) && !memcmp(buf, text, n);
}

static void test_redirect_auth(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;

   /* Same host: the echo sees the login. */
   new_cache(cache, sizeof(cache), "echo");
   r = fetch("/echoauth", cache, "bob", "pw", out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_text(out, "auth:Basic Ym9iOnB3"));

   /* 127.0.0.1 -> 127.0.0.2: no Authorization is sent. */
   new_cache(cache, sizeof(cache), "xhost");
   r = fetch("/xhost", cache, "bob", "pw", out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_text(out, "none"));

   /* Saved logins do not cross hosts either. */
   new_cache(cache, sizeof(cache), "xhost-saved");
   EXPECT(url_logins_set("127.0.0.1", "bob", "pw"));
   r = fetch("/xhost", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_text(out, "none"));
   EXPECT(url_logins_forget_all());

   /* Same-host redirect keeps the login. */
   new_cache(cache, sizeof(cache), "samehost");
   r = fetch("/samehost", cache, "bob", "pw", out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
}

static void test_redirect_limits(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;

   new_cache(cache, sizeof(cache), "loop");
   r = fetch("/loop", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);

   new_cache(cache, sizeof(cache), "chain10");
   r = fetch("/chain/10", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));

   new_cache(cache, sizeof(cache), "chain11");
   r = fetch("/chain/11", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_FAILED);

   new_cache(cache, sizeof(cache), "rel");
   r = fetch("/rel/a", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
   EXPECT(ends_with(out, "_a"));
}

static void test_empty(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "empty");
   r = fetch("/empty", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
}

static void test_huge(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   time_t t0 = time(NULL);
   new_cache(cache, sizeof(cache), "huge");
   r = fetch("/huge", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_TOO_BIG);
   EXPECT(time(NULL) - t0 < 5);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
}

static void rm_tree(const char *dir)
{
   char cmd[600];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
   if (system(cmd) != 0)
      printf("warning: could not remove %s\n", dir);
}

int main(int argc, char **argv)
{
   char tmpl[] = "/tmp/url_fetch_test.XXXXXX";
   if (argc < 2 || (g_port = atoi(argv[1])) <= 0)
   {
      printf("usage: %s <port>\n", argv[0]);
      return 2;
   }
   if (!mkdtemp(tmpl))
   {
      printf("mkdtemp failed\n");
      return 2;
   }
   snprintf(g_root, sizeof(g_root), "%s", tmpl);
   snprintf(g_logins, sizeof(g_logins), "%s/logins.cfg", g_root);
   url_logins_set_path(g_logins);
   make_game();

   test_ok_and_cached();
   test_leftover_part_removed();
   test_nodisp();
   test_page();
   test_basic();
   test_saved_login();
   test_digest();
   test_redirect();
   test_drop();
   test_missing();
   test_big();
   test_redirect_auth();
   test_redirect_limits();
   test_empty();
   test_huge();

   rm_tree(g_root);

   if (g_failures)
   {
      printf("url_fetch: %d of %d checks FAILED\n", g_failures, g_checks);
      return 1;
   }
   printf("url_fetch: all %d checks passed\n", g_checks);
   return 0;
}
