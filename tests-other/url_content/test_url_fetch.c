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
 * Usage: test_url_fetch <port> [<https port> <test CA pem>]
 *        test_url_fetch online   (certificate checks on real sites) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <utime.h>

#include <net/net_http.h>
#include <net/net_socket_ssl.h>
#include <retro_timers.h>

#include "../../network/url_fetch.h"
#include "../../network/url_catalog.h"
#include "../../network/url_archive_org.h"
#include "../../network/url_logins.h"

static int g_checks   = 0;
static int g_failures = 0;
static int g_port     = 0;
static int g_sport    = 0;      /* https port, 0 = no https tests */
static const char *g_ca = NULL; /* the test CA that signed localhost */
static char g_root[256];
static char g_logins[300];

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
         ch, ch_len, err, sizeof(err), NULL, NULL, NULL);
}

/* GETs path + "?n=<counter>" (a new URL each time, so the cache never
 * answers) and copies the body into buf. False on any failure. */
static int get_text(const char *path, char *buf, size_t len)
{
   static int n = 0;
   char u[256];
   char cache[512];
   char out[1024];
   FILE *f;
   size_t got;

   snprintf(u, sizeof(u), "%s?n=%d", path, ++n);
   snprintf(cache, sizeof(cache), "%s/text", g_root);
   if (fetch(u, cache, NULL, NULL, out, sizeof(out), NULL, 0)
         != URL_FETCH_OK)
      return 0;
   if (!(f = fopen(out, "rb")))
      return 0;
   got = fread(buf, 1, len - 1, f);
   fclose(f);
   buf[got] = '\0';
   remove(out);
   return 1;
}

/* Authorization schemes serve.py saw on /basic/ and /digest/ since the
 * last call, e.g. "none,Basic"; "empty" if none. */
static const char *auth_log(void)
{
   static char buf[256];
   if (!get_text("/authlog", buf, sizeof(buf)))
      snprintf(buf, sizeof(buf), "(error)");
   return buf;
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
         NULL, 0, err, sizeof(err), prog_cb, NULL, NULL);
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
   /* A fresh .part may be another download of the same link going on
    * right now: it is left alone. */
   r = fetch("/game.sfc", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(count_files(cache, NULL, 0, &parts) == 2);
   EXPECT(parts == 1);
   /* Untouched for two hours: a leftover, removed (on a cache hit too). */
   {
      struct utimbuf old;
      old.actime = old.modtime = time(NULL) - 2 * 3600;
      EXPECT(utime(part, &old) == 0);
   }
   r = fetch("/game.sfc", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_CACHED);
   EXPECT(count_files(cache, NULL, 0, &parts) == 1);
   EXPECT(parts == 0);
}

/* Two downloads of one link at the same time (two RetroArch
 * instances): both finish, one file, no .part left. */
static void test_concurrent_same_url(void)
{
   char cache[300], out[512];
   int parts = 0, status = 0;
   pid_t pid;
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "concurrent");
   mkdir(cache, 0700);
   fflush(stdout);
   if ((pid = fork()) == 0)
   {
      char o[512];
      r = fetch("/slowsmall", cache, NULL, NULL, o, sizeof(o), NULL, 0);
      _exit(r == URL_FETCH_OK ? 0 : 10 + (int)r);
   }
   EXPECT(pid > 0);
   {
      struct timespec ts = {0, 300000000};
      nanosleep(&ts, NULL);   /* the first one is half-way by now */
   }
   r = fetch("/slowsmall", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   if (pid > 0)
   {
      waitpid(pid, &status, 0);
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
         printf("  (first download: status %d)\n", status);
      EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   EXPECT(file_size(out) == 2L * 1024L * 1024L);
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
   auth_log();
   r = fetch("/basic/game.sfc", cache, NULL, NULL, out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_NEED_LOGIN);
   EXPECT(strstr(ch, "Basic") != NULL);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
   EXPECT_STR(auth_log(), "none");

   /* Plain http: the login waits for the server's 401, then answers
    * it once. */
   r = fetch("/basic/game.sfc", cache, "bob", "nope", out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_NEED_LOGIN);
   EXPECT_STR(auth_log(), "none,Basic");

   r = fetch("/basic/game.sfc", cache, "bob", "pw", out, sizeof(out),
         ch, sizeof(ch));
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
   EXPECT_STR(auth_log(), "none,Basic");
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

/* Over plain http a Digest server never sees Basic (the password). */
static void test_digest_no_basic_first(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "digest-first");
   auth_log();
   r = fetch("/digest/game.sfc", cache, "bob", "pw", out, sizeof(out),
         NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT_STR(auth_log(), "none,Digest");

   new_cache(cache, sizeof(cache), "digest-first-saved");
   EXPECT(url_logins_set("127.0.0.1", "bob", "pw"));
   r = fetch("/digest/game.sfc", cache, NULL, NULL, out, sizeof(out),
         NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT_STR(auth_log(), "none,Digest");
   EXPECT(url_logins_forget_all());
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

/* Peak resident memory in KiB (VmHWM), after resetting it to now when
 * reset is set. -1 if /proc is not there. */
static long peak_rss_kib(int reset)
{
   char line[256];
   long kib = -1;
   FILE *f;
   if (reset && (f = fopen("/proc/self/clear_refs", "w")))
   {
      fputs("5", f);
      fclose(f);
   }
   if (!(f = fopen("/proc/self/status", "r")))
      return -1;
   while (fgets(line, sizeof(line), f))
      if (!strncmp(line, "VmHWM:", 6))
         kib = atol(line + 6);
   fclose(f);
   return kib;
}

/* 300 MiB goes straight to the file: memory hardly grows. */
static void test_big(void)
{
   char cache[300], out[512];
   long before, after;
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "big");
   before = peak_rss_kib(1);
   r = fetch("/big", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   after  = peak_rss_kib(0);
   EXPECT(r == URL_FETCH_OK);
   if (r == URL_FETCH_OK)
   {
      EXPECT(file_size(out) == 300L * 1024L * 1024L);
      remove(out);
   }
   if (before > 0 && after > 0)
   {
      printf("  /big: peak memory grew by %ld KiB\n", after - before);
      EXPECT(after - before < 64L * 1024L);
   }
   else
      printf("SKIP: memory check (no /proc/self/status)\n");
}

static void test_chunked(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "chunked");
   r = fetch("/chunked", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));

   new_cache(cache, sizeof(cache), "chunkedbig");
   r = fetch("/chunkedbig", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_size(out) == 20L * 1024L * 1024L);
   if (r == URL_FETCH_OK)
   {
      unsigned char buf[4096];
      size_t n, i;
      long at = 0;
      int same = 1;
      FILE *f = fopen(out, "rb");
      while (f && (n = fread(buf, 1, sizeof(buf), f)) > 0)
      {
         for (i = 0; i < n; i++, at++)
            if (buf[i] != (unsigned char)(((at % 256) * 7 + 3) & 0xFF))
               same = 0;
      }
      if (f)
         fclose(f);
      EXPECT(same);
      remove(out);
   }
}

static void copy_file(const char *from, const char *to)
{
   char buf[4096];
   size_t n;
   FILE *in  = fopen(from, "rb");
   FILE *o   = fopen(to, "wb");
   while (in && o && (n = fread(buf, 1, sizeof(buf), in)) > 0)
      fwrite(buf, 1, n, o);
   if (in)
      fclose(in);
   if (o)
      fclose(o);
}

static void test_safety_fetch(void)
{
   char cache[300], out[512], out2[512], err[256], u[256], slip[512];
   int parts = 0;
   FILE *f;
   enum url_fetch_result r;

   /* A program by name and bytes. */
   new_cache(cache, sizeof(cache), "prog");
   url(u, sizeof(u), "/prog.exe");
   r = url_fetch_to_cache(u, cache, NULL, NULL, out, sizeof(out),
         NULL, 0, err, sizeof(err), NULL, NULL, NULL);
   EXPECT(r == URL_FETCH_BLOCKED_PROGRAM);
   EXPECT_STR(err, "Blocked: this link gives a program, not a game.");
   EXPECT(out[0] == '\0');
   EXPECT(count_files(cache, NULL, 0, &parts) == 0);

   /* A game name with program bytes. */
   new_cache(cache, sizeof(cache), "sneaky");
   r = fetch("/sneaky.gb", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_BLOCKED_PROGRAM);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);

   /* Game bytes, but the server names it .exe. */
   new_cache(cache, sizeof(cache), "namedprog");
   r = fetch("/named-prog", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_BLOCKED_PROGRAM);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);

   /* A DOS game zip (.exe + .bat inside) is fine. */
   new_cache(cache, sizeof(cache), "dosgame");
   r = fetch("/dosgame.zip", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(ends_with(out, "_dosgame.zip"));
   EXPECT(count_files(cache, NULL, 0, NULL) == 1);

   /* Zip-slip: refused, nothing left behind. */
   new_cache(cache, sizeof(cache), "slip");
   url(u, sizeof(u), "/slip.zip");
   r = url_fetch_to_cache(u, cache, NULL, NULL, out2, sizeof(out2),
         NULL, 0, err, sizeof(err), NULL, NULL, NULL);
   EXPECT(r == URL_FETCH_BLOCKED_ARCHIVE);
   EXPECT_STR(err, "Blocked: this archive contains a program or an unsafe file path.");
   EXPECT(count_files(cache, NULL, 0, &parts) == 0);

   /* Cache hits are checked too: a cached zip turned into a trap. */
   new_cache(cache, sizeof(cache), "slipsrc");
   r = fetch("/slip.dat", cache, NULL, NULL, slip, sizeof(slip), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   new_cache(cache, sizeof(cache), "dosgame");
   copy_file(slip, out);
   r = fetch("/dosgame.zip", cache, NULL, NULL, out2, sizeof(out2), NULL, 0);
   EXPECT(r == URL_FETCH_BLOCKED_ARCHIVE);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);

   /* ... and a cached game turned into a program. */
   new_cache(cache, sizeof(cache), "tamper");
   r = fetch("/game.sfc", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   if ((f = fopen(out, "wb")))
   {
      fwrite("\x7f" "ELF\x02\x01\x01\x00", 1, 8, f);
      fclose(f);
   }
   r = fetch("/game.sfc", cache, NULL, NULL, out2, sizeof(out2), NULL, 0);
   EXPECT(r == URL_FETCH_BLOCKED_PROGRAM);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
   /* Afterwards the real file downloads again. */
   r = fetch("/game.sfc", cache, NULL, NULL, out2, sizeof(out2), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out2));
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

   /* Plain http, no 401: the login is never sent. */
   new_cache(cache, sizeof(cache), "echo");
   r = fetch("/echoauth", cache, "bob", "pw", out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_text(out, "none"));

   /* Same host asking for it: the echo sees the login. */
   new_cache(cache, sizeof(cache), "basicecho");
   r = fetch("/basicecho", cache, "bob", "pw", out, sizeof(out), NULL, 0);
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

/* archive.org keys never go over plain http. */
static void test_archive_keys_http(void)
{
   char cache[300], out[512];
   enum url_fetch_result r;
   url_fetch_set_test_archive_host("127.0.0.1");
   EXPECT(url_logins_set_archive_keys("ACCESSKEY", "SECRETKEY"));
   new_cache(cache, sizeof(cache), "archive-http");
   r = fetch("/echoauth", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_text(out, "none"));
   url_fetch_set_test_archive_host(NULL);
   EXPECT(url_logins_forget_all());
}

static int is_dir(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void make_dir(const char *path)
{
#ifdef _WIN32
   mkdir(path);
#else
   mkdir(path, 0700);
#endif
}

static void touch(const char *path)
{
   FILE *f = fopen(path, "wb");
   if (f)
   {
      fputs("x", f);
      fclose(f);
   }
}

static void test_clear_cache(void)
{
   char cache[300], out[512], p[1024], keep_dir[700], keep_file[700];
   char other[300];
   unsigned removed = 99;
   enum url_fetch_result r;

   new_cache(cache, sizeof(cache), "url_cache");
   r = fetch("/game.sfc", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   r = fetch("/nodisp?id=1", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   snprintf(p, sizeof(p), "%s/0123456789abcdef_old.bin.part", cache);
   touch(p);
   /* Not url_fetch's: kept. */
   snprintf(keep_file, sizeof(keep_file), "%s/notes.txt", cache);
   touch(keep_file);
   snprintf(keep_dir, sizeof(keep_dir), "%s/0123456789abcdef_dir", cache);
   make_dir(keep_dir);
   snprintf(p, sizeof(p), "%s/inner.bin", keep_dir);
   touch(p);

   EXPECT(count_files(cache, NULL, 0, NULL) == 5);
   EXPECT(url_fetch_clear_cache(cache, &removed));
   EXPECT(removed == 3);
   EXPECT(count_files(cache, NULL, 0, NULL) == 2);
   EXPECT(file_size(keep_file) == 1);
   EXPECT(is_dir(keep_dir) && file_size(p) == 1);

   /* The link downloads again afterwards. */
   r = fetch("/game.sfc", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);

   /* Trailing slash is fine; a folder not named url_cache is refused
    * and left alone; a missing url_cache is already clear. */
   snprintf(p, sizeof(p), "%s/", cache);
   EXPECT(url_fetch_clear_cache(p, &removed));
   EXPECT(removed == 1);
   new_cache(other, sizeof(other), "ok");
   EXPECT(count_files(other, NULL, 0, NULL) == 1);
   EXPECT(!url_fetch_clear_cache(other, &removed));
   EXPECT(count_files(other, NULL, 0, NULL) == 1);
   EXPECT(!url_fetch_clear_cache("", NULL));
   EXPECT(!url_fetch_clear_cache(NULL, NULL));
   snprintf(p, sizeof(p), "%s/nothing/url_cache", g_root);
   EXPECT(url_fetch_clear_cache(p, NULL));
}

/* ---------- game lists (Caddy browse JSON) ---------- */

static enum url_fetch_result fetch_cat(const char *full_url,
      const char *cache, const char *user, const char *pass,
      char *out, size_t out_len, char *err, size_t err_len,
      struct url_catalog *cat)
{
   return url_fetch_to_cache(full_url, cache, user, pass, out, out_len,
         NULL, 0, err, err_len, NULL, NULL, cat);
}

/* 1 if the file holds exactly the bytes "GBDUMMY". */
static int file_is_gbdummy(const char *path)
{
   char buf[64];
   size_t n;
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   n = fread(buf, 1, sizeof(buf), f);
   fclose(f);
   return n == 7 && !memcmp(buf, "GBDUMMY", 7);
}

static void test_fetch_catalog_root(void)
{
   char cache[300], out[512], err[256], u[256];
   struct url_catalog cat;
   enum url_fetch_result r;
   memset(&cat, 0, sizeof(cat));
   new_cache(cache, sizeof(cache), "cat-root");
   url(u, sizeof(u), "/cat/");
   r = fetch_cat(u, cache, NULL, NULL, out, sizeof(out), err, sizeof(err),
         &cat);
   EXPECT(r == URL_FETCH_CATALOG);
   EXPECT(cat.count == 2);
   if (cat.count == 2)
   {
      EXPECT_STR(cat.entries[0].name, "Game Boy");
      EXPECT(cat.entries[0].is_dir);
      EXPECT(ends_with(cat.entries[0].url, "/cat/Game%20Boy/"));
      EXPECT_STR(cat.entries[1].name, "x.gb");
      EXPECT(!cat.entries[1].is_dir);
   }
   EXPECT_STR(out, "");
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
   url_catalog_free(&cat);

   /* A list is never answered from the cache: same URL again is a
    * fresh list, and the cache stays empty. */
   memset(&cat, 0, sizeof(cat));
   r = fetch_cat(u, cache, NULL, NULL, out, sizeof(out), err, sizeof(err),
         &cat);
   EXPECT(r == URL_FETCH_CATALOG);
   EXPECT(cat.count == 2);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
   url_catalog_free(&cat);
}

static void test_fetch_catalog_file_from_listing(void)
{
   char cache[300], out[512], err[256], u[256];
   struct url_catalog cat;
   enum url_fetch_result r;
   memset(&cat, 0, sizeof(cat));
   new_cache(cache, sizeof(cache), "cat-file");
   url(u, sizeof(u), "/cat/Game%20Boy/");
   r = fetch_cat(u, cache, NULL, NULL, out, sizeof(out), err, sizeof(err),
         &cat);
   EXPECT(r == URL_FETCH_CATALOG);
   EXPECT(cat.count == 1);
   if (cat.count == 1)
   {
      EXPECT_STR(cat.entries[0].name, "Space & Time #2.gb");
      r = fetch_cat(cat.entries[0].url, cache, NULL, NULL,
            out, sizeof(out), err, sizeof(err), NULL);
      EXPECT(r == URL_FETCH_OK);
      EXPECT(file_is_gbdummy(out));
   }
   url_catalog_free(&cat);
}

static void test_fetch_catalog_empty(void)
{
   char cache[300], out[512], err[256], u[256];
   struct url_catalog cat;
   enum url_fetch_result r;
   memset(&cat, 0, sizeof(cat));
   new_cache(cache, sizeof(cache), "cat-empty");
   url(u, sizeof(u), "/cat/empty/");
   r = fetch_cat(u, cache, NULL, NULL, out, sizeof(out), err, sizeof(err),
         &cat);
   EXPECT(r == URL_FETCH_CATALOG);
   EXPECT(cat.count == 0);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
   url_catalog_free(&cat);
}

static void test_fetch_catalog_login(void)
{
   char cache[300], out[512], err[256], u[256];
   struct url_catalog cat;
   enum url_fetch_result r;
   memset(&cat, 0, sizeof(cat));
   new_cache(cache, sizeof(cache), "cat-login");

   url(u, sizeof(u), "/basic/cat/");
   r = fetch_cat(u, cache, NULL, NULL, out, sizeof(out), err, sizeof(err),
         &cat);
   EXPECT(r == URL_FETCH_NEED_LOGIN);
   url_catalog_free(&cat);

   EXPECT(url_logins_set("127.0.0.1", "bob", "pw"));
   memset(&cat, 0, sizeof(cat));
   r = fetch_cat(u, cache, NULL, NULL, out, sizeof(out), err, sizeof(err),
         &cat);
   EXPECT(r == URL_FETCH_CATALOG);
   EXPECT(cat.count == 2);
   if (cat.count == 2)
   {
      EXPECT_STR(cat.entries[1].name, "x.gb");
      r = fetch_cat(cat.entries[1].url, cache, NULL, NULL,
            out, sizeof(out), err, sizeof(err), NULL);
      EXPECT(r == URL_FETCH_OK);
      EXPECT(file_is_gbdummy(out));
   }
   url_catalog_free(&cat);
   EXPECT(url_logins_forget_all());
}

static void test_fetch_catalog_null_sink(void)
{
   char cache[300], out[512], err[256], u[256];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "cat-null");
   url(u, sizeof(u), "/cat/");
   r = fetch_cat(u, cache, NULL, NULL, out, sizeof(out), err, sizeof(err),
         NULL);
   EXPECT(r == URL_FETCH_CATALOG);
   EXPECT_STR(err, "This link is a game list. Open it from the menu: Load Content > Load from URL.");
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
}

/* JSON that is not a listing is a file, as before. */
static void test_fetch_plain_json_is_file(void)
{
   char cache[300], out[512], out2[512];
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "plain-json");
   r = fetch("/plain.json", cache, NULL, NULL, out, sizeof(out), NULL, 0);
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_text(out, "{\"hi\":1}"));
   EXPECT(count_files(cache, NULL, 0, NULL) == 1);
   r = fetch("/plain.json", cache, NULL, NULL, out2, sizeof(out2), NULL, 0);
   EXPECT(r == URL_FETCH_CACHED);
   EXPECT(!strcmp(out, out2));
}

static void test_fetch_accept_header(void)
{
   char buf[256];
   EXPECT(get_text("/acceptlog", buf, sizeof(buf)));
   EXPECT_STR(buf, "accept:*/*, application/json;q=0.9");
}

/* ---------- https (needs the port and test CA from the Makefile) ---------- */

static enum url_fetch_result fetch_s(const char *host, const char *path,
      const char *cache, const char *user, const char *pass,
      char *out, size_t out_len, char *err, size_t err_len)
{
   char u[256];
   snprintf(u, sizeof(u), "https://%s:%d%s", host, g_sport, path);
   return url_fetch_to_cache(u, cache, user, pass, out, out_len,
         NULL, 0, err, err_len, NULL, NULL, NULL);
}

/* Runs with the system CAs (the test CA untrusted): sending Accept must
 * not switch certificate checks on, so anonymous https still works,
 * for a file and for a list. */
static void test_fetch_no_verify_change(void)
{
   char cache[300], out[512], err[256];
   struct url_catalog cat;
   enum url_fetch_result r;
   new_cache(cache, sizeof(cache), "https-noverify");
   r = fetch_s("localhost", "/plain.json", cache, NULL, NULL,
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_text(out, "{\"hi\":1}"));

   memset(&cat, 0, sizeof(cat));
   {
      char u[256];
      snprintf(u, sizeof(u), "https://localhost:%d/cat/", g_sport);
      r = fetch_cat(u, cache, NULL, NULL, out, sizeof(out),
            err, sizeof(err), &cat);
   }
   EXPECT(r == URL_FETCH_CATALOG);
   EXPECT(cat.count == 2);
   url_catalog_free(&cat);
}

static void test_https(void)
{
   char cache[300], out[512], err[256];
   char a[64], s[64], u[256];
   enum url_fetch_result r;
   const char *bad = "This site's security certificate is not valid, so the login was not sent.";
   const char *noca = "Can't check this site's security certificate, so the login was not sent.";

   /* System CAs (the test CA is not one of them): anonymous downloads
    * work as in upstream RetroArch (no check), a login is refused
    * before the server sees anything. */
   ssl_socket_set_ca_bundle_override(NULL);
   new_cache(cache, sizeof(cache), "https-anon");
   r = fetch_s("localhost", "/game.sfc", cache, NULL, NULL,
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));

   test_fetch_no_verify_change();

   auth_log();
   new_cache(cache, sizeof(cache), "https-untrusted");
   r = fetch_s("localhost", "/basic/game.sfc", cache, "bob", "pw",
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT_STR(err, bad);
   EXPECT(count_files(cache, NULL, 0, NULL) == 0);
   EXPECT_STR(auth_log(), "empty");

   /* Saved login: same. */
   EXPECT(url_logins_set("localhost", "bob", "pw"));
   r = fetch_s("localhost", "/basic/game.sfc", cache, NULL, NULL,
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT_STR(err, bad);
   EXPECT_STR(auth_log(), "empty");
   EXPECT(url_logins_forget_all());

   /* No CA certificates at all: a clear refusal; anonymous still ok. */
   ssl_socket_set_ca_bundle_override("/nonexistent/ca.pem");
   new_cache(cache, sizeof(cache), "https-noca");
   r = fetch_s("localhost", "/basic/game.sfc", cache, "bob", "pw",
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT_STR(err, noca);
   EXPECT_STR(auth_log(), "empty");
   r = fetch_s("localhost", "/game.sfc", cache, NULL, NULL,
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_OK);

   /* Trusting the test CA: the login goes through (Basic right away
    * over https), but only for the name on the certificate. */
   ssl_socket_set_ca_bundle_override(g_ca);
   new_cache(cache, sizeof(cache), "https-trusted");
   r = fetch_s("localhost", "/basic/game.sfc", cache, "bob", "pw",
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_game(out));
   EXPECT_STR(auth_log(), "Basic");

   new_cache(cache, sizeof(cache), "https-wrongname");
   r = fetch_s("127.0.0.1", "/basic/game.sfc", cache, "bob", "pw",
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT_STR(err, bad);
   EXPECT_STR(auth_log(), "empty");

   /* Digest over https: Basic first, then the Digest answer. */
   new_cache(cache, sizeof(cache), "https-digest");
   r = fetch_s("localhost", "/digest/game.sfc", cache, "bob", "pw",
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_OK);
   EXPECT_STR(auth_log(), "Basic,Digest");

   /* archive.org keys: sent over (verified) https. */
   url_fetch_set_test_archive_host("localhost");
   EXPECT(url_logins_set_archive_keys("ACCESSKEY", "SECRETKEY"));
   new_cache(cache, sizeof(cache), "https-low");
   r = fetch_s("localhost", "/echoauth", cache, NULL, NULL,
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_OK);
   EXPECT(file_is_text(out, "auth:LOW ACCESSKEY:SECRETKEY"));
   /* ... and not to a server whose certificate fails. */
   ssl_socket_set_ca_bundle_override(NULL);
   new_cache(cache, sizeof(cache), "https-low-bad");
   r = fetch_s("localhost", "/echoauth", cache, NULL, NULL,
         out, sizeof(out), err, sizeof(err));
   EXPECT(r == URL_FETCH_FAILED);
   EXPECT_STR(err, bad);
   url_fetch_set_test_archive_host(NULL);
   EXPECT(url_logins_forget_all());

   /* archive.org sign-in over https: the password is only POSTed to a
    * server with a valid certificate. */
   {
      int before = -1;
      char cnt[32];
      if (get_text("/xauthn/count", cnt, sizeof(cnt)))
         before = atoi(cnt);
      snprintf(u, sizeof(u), "https://localhost:%d/xauthn/?op=login",
            g_sport);
      url_archive_org_set_test_url(u);
      EXPECT(!url_archive_org_login("bob+ia@example.com", "p&ss w=rd",
            a, sizeof(a), s, sizeof(s), err, sizeof(err)));
      EXPECT_STR(err, "archive.org's security certificate is not valid, so the password was not sent");
      EXPECT(get_text("/xauthn/count", cnt, sizeof(cnt))
            && atoi(cnt) == before);

      ssl_socket_set_ca_bundle_override(g_ca);
      EXPECT(url_archive_org_login("bob+ia@example.com", "p&ss w=rd",
            a, sizeof(a), s, sizeof(s), err, sizeof(err)));
      EXPECT_STR(a, "ACCESSKEY");
      url_archive_org_set_test_url(NULL);
      url_logins_forget_all();
   }
   ssl_socket_set_ca_bundle_override(NULL);
}

/* ---------- online: real sites, system CAs ---------- */

/* GET with verify_tls; returns the status (-1 on failure) and the
 * TLS error. */
static int online_get(const char *u, int *tls_error)
{
   struct http_connection_t *conn;
   struct http_t *http;
   int status;
   int spins = 0;

   *tls_error = -1;
   if (!(conn = net_http_connection_new(u, "GET", NULL)))
      return -1;
   if (!net_http_connection_iterate(conn) || !net_http_connection_done(conn))
   {
      net_http_connection_free(conn);
      return -1;
   }
   net_http_connection_set_user_agent(conn, "RetroArch");
   net_http_connection_set_follow_redirects(conn, false);
   net_http_connection_set_verify_tls(conn, true);
   http = net_http_new(conn);
   net_http_connection_free(conn);
   if (!http)
      return -1;
   while (!net_http_update(http, NULL, NULL) && spins++ < 30000)
      retro_sleep(1);
   status     = net_http_error(http) ? -1 : net_http_status(http);
   *tls_error = net_http_tls_error(http);
   net_http_delete(http);
   return status;
}

static int run_online(void)
{
   int tls, status;

   status = online_get("https://archive.org/robots.txt", &tls);
   printf("archive.org: status %d, tls_error %d\n", status, tls);
   EXPECT(tls == NET_HTTP_TLS_OK);
   EXPECT(status >= 200 && status < 400);

   status = online_get("https://self-signed.badssl.com/", &tls);
   printf("self-signed.badssl.com: status %d, tls_error %d\n", status, tls);
   EXPECT(tls == NET_HTTP_TLS_BAD_CERT);

   status = online_get("https://wrong.host.badssl.com/", &tls);
   printf("wrong.host.badssl.com: status %d, tls_error %d\n", status, tls);
   EXPECT(tls == NET_HTTP_TLS_BAD_CERT);

   status = online_get("https://expired.badssl.com/", &tls);
   printf("expired.badssl.com: status %d, tls_error %d\n", status, tls);
   EXPECT(tls == NET_HTTP_TLS_BAD_CERT);

   if (g_failures)
   {
      printf("tls online: %d of %d checks FAILED\n", g_failures, g_checks);
      return 1;
   }
   printf("tls online: all %d checks passed\n", g_checks);
   return 0;
}

static void rm_tree(const char *dir)
{
   char cmd[600];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
   if (system(cmd) != 0)
      printf("warning: could not remove %s\n", dir);
}

/* Number of POSTs serve.py's fake xauthn endpoint has received. Each
 * call uses a new URL so the fetch cache never answers it. */
static int login_posts(void)
{
   static int n = 0;
   char u[256];
   char cache[512];
   char out[1024];
   char buf[32];
   FILE *f;
   size_t got;

   snprintf(u, sizeof(u), "/xauthn/count?n=%d", ++n);
   new_cache(cache, sizeof(cache), "count");
   if (fetch(u, cache, NULL, NULL, out, sizeof(out), NULL, 0)
         != URL_FETCH_OK)
      return -1;
   if (!(f = fopen(out, "rb")))
      return -1;
   got = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[got] = '\0';
   return atoi(buf);
}

static void test_archive_login(void)
{
   int posts_before;
   char u[256];
   char access[64];
   char secret[64];
   char err[128];
   char a2[64];
   char s2[64];

   url(u, sizeof(u), "/xauthn/?op=login");
   url_archive_org_set_test_url(u);

   /* Wrong password: refused, nothing saved. */
   EXPECT(!url_archive_org_login("bob+ia@example.com", "nope",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "wrong email or password");
   EXPECT(!url_logins_get_archive_keys(a2, sizeof(a2), s2, sizeof(s2)));

   EXPECT(!url_archive_org_login("nobody@example.com", "x",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "no account with that email");

   /* An error status with archive.org's JSON reason: the reason wins. */
   EXPECT(!url_archive_org_login("bob401@example.com", "nope",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "wrong email or password");
   EXPECT(!url_archive_org_login("html401@example.com", "nope",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "server answered 401");

   /* A web page instead of JSON. */
   EXPECT(!url_archive_org_login("page@example.com", "x",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "unexpected reply");

   /* Empty input is refused before any request. */
   EXPECT(!url_archive_org_login("", "x",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT(!url_archive_org_login("a@b.c", "",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));

   /* Right login (form-encoding needed for '+', '&', ' ', '='):
    * keys come back and are saved. */
   EXPECT(url_archive_org_login("bob+ia@example.com", "p&ss w=rd",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(access, "ACCESSKEY");
   EXPECT_STR(secret, "SECRETKEY");
   EXPECT(url_logins_get_archive_keys(a2, sizeof(a2), s2, sizeof(s2)));
   EXPECT_STR(a2, "ACCESSKEY");
   EXPECT_STR(s2, "SECRETKEY");

   /* A redirect is not followed (the password is never re-sent):
    * the 302 is the answer, and the login endpoint it points at sees
    * no new POST. */
   posts_before = login_posts();
   EXPECT(posts_before > 0);
   url(u, sizeof(u), "/xauthn/redirect?op=login");
   url_archive_org_set_test_url(u);
   EXPECT(!url_archive_org_login("bob+ia@example.com", "p&ss w=rd",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT_STR(err, "server answered 302");
   EXPECT_STR(access, "");
   EXPECT(login_posts() == posts_before);

   /* Nothing listening: a clean failure. */
   url_archive_org_set_test_url("http://127.0.0.1:1/xauthn/?op=login");
   EXPECT(!url_archive_org_login("bob+ia@example.com", "p&ss w=rd",
         access, sizeof(access), secret, sizeof(secret), err, sizeof(err)));
   EXPECT(err[0] != '\0');

   url_archive_org_set_test_url(NULL);
   url_logins_forget_all();
}

int main(int argc, char **argv)
{
   char tmpl[] = "/tmp/url_fetch_test.XXXXXX";
   if (argc >= 2 && !strcmp(argv[1], "online"))
      return run_online();
   if (argc < 2 || (g_port = atoi(argv[1])) <= 0)
   {
      printf("usage: %s <port> [<https port> <ca.pem>] | online\n",
            argv[0]);
      return 2;
   }
   if (argc >= 4)
   {
      g_sport = atoi(argv[2]);
      g_ca    = argv[3];
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
   test_concurrent_same_url();
   test_nodisp();
   test_page();
   test_basic();
   test_saved_login();
   test_digest();
   test_digest_no_basic_first();
   test_redirect();
   test_drop();
   test_missing();
   test_big();
   test_chunked();
   test_safety_fetch();
   test_redirect_auth();
   test_redirect_limits();
   test_empty();
   test_huge();
   test_archive_login();
   test_archive_keys_http();
   test_clear_cache();
   test_fetch_catalog_root();
   test_fetch_catalog_file_from_listing();
   test_fetch_catalog_empty();
   test_fetch_catalog_login();
   test_fetch_catalog_null_sink();
   test_fetch_plain_json_is_file();
   test_fetch_accept_header();
   if (g_sport > 0)
      test_https();
   else
      printf("SKIP: https tests (no https port given)\n");

   rm_tree(g_root);

   if (g_failures)
   {
      printf("url_fetch: %d of %d checks FAILED\n", g_failures, g_checks);
      return 1;
   }
   printf("url_fetch: all %d checks passed\n", g_checks);
   return 0;
}
