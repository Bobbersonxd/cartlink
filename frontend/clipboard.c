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

/* Clipboard text for the text input dialog (Ctrl+V / Shift+Insert). */

#include <stdlib.h>
#include <string.h>

#include "clipboard.h"

#if defined(_WIN32) && !defined(_XBOX) && !defined(__WINRT__)
#define CLIPBOARD_WIN32
#include <windows.h>
#elif (defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
      || defined(__NetBSD__) || defined(__DragonFly__)) \
      && !defined(ANDROID) && !defined(__ANDROID__) \
      && !defined(__EMSCRIPTEN__) && !defined(__SWITCH__) \
      && !defined(HAVE_LIBNX)
#define CLIPBOARD_UNIX
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

extern char **environ;

/* Each clipboard tool gets at most this long, start to exit. */
#define CLIPBOARD_TIMEOUT_MS 1000
#endif

void clipboard_clean_text(char *s)
{
   char *src;
   char *dst;
   char *start;
   size_t n;

   if (!s)
      return;

   /* Drop line breaks, tabs and other control bytes. */
   for (src = dst = s; *src; src++)
   {
      unsigned char c = (unsigned char)*src;
      if (c < 0x20 || c == 0x7f)
         continue;
      *dst++ = *src;
   }
   *dst = '\0';

   /* Trim surrounding spaces. */
   start = s;
   while (*start == ' ')
      start++;
   n = strlen(start);
   while (n > 0 && start[n - 1] == ' ')
      n--;
   if (start != s)
      memmove(s, start, n);
   s[n] = '\0';
}

#if defined(CLIPBOARD_WIN32) || defined(CLIPBOARD_UNIX)
/* Copies up to len - 1 bytes of src into out, never cutting a
 * UTF-8 sequence in half. */
static void clipboard_copy_utf8(char *out, size_t len,
      const char *src, size_t src_len)
{
   size_t n = src_len;
   if (n > len - 1)
   {
      n = len - 1;
      while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
         n--;
   }
   memcpy(out, src, n);
   out[n] = '\0';
}
#endif

#if defined(CLIPBOARD_UNIX)
static long clipboard_now_ms(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

/* Runs argv (searched on PATH) with stdin/stderr on /dev/null and
 * reads its stdout into out. Gives up and kills the child after
 * CLIPBOARD_TIMEOUT_MS. True when the tool ran, exited cleanly
 * (or filled the buffer) and gave some text. */
static bool clipboard_run(char *const argv[], char *out, size_t len)
{
   posix_spawn_file_actions_t actions;
   posix_spawnattr_t attr;
   int fds[2];
   pid_t pid;
   char *buf;
   size_t got      = 0;
   size_t cap      = len; /* one byte more than fits, to see a cut */
   bool eof        = false;
   bool full       = false;
   bool exited     = false;
   bool reaped_ok  = false;
   int status      = 0;
   long deadline;

   if (pipe(fds) != 0)
      return false;
   fcntl(fds[0], F_SETFD, FD_CLOEXEC);
   fcntl(fds[1], F_SETFD, FD_CLOEXEC);

   if (posix_spawn_file_actions_init(&actions) != 0)
   {
      close(fds[0]);
      close(fds[1]);
      return false;
   }
   posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
   posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
   posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);

   /* Own process group, so a hung tool and anything it started
    * can be killed together. */
   if (posix_spawnattr_init(&attr) != 0)
   {
      posix_spawn_file_actions_destroy(&actions);
      close(fds[0]);
      close(fds[1]);
      return false;
   }
   posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
   posix_spawnattr_setpgroup(&attr, 0);

   if (posix_spawnp(&pid, argv[0], &actions, &attr, argv, environ) != 0)
   {
      posix_spawnattr_destroy(&attr);
      posix_spawn_file_actions_destroy(&actions);
      close(fds[0]);
      close(fds[1]);
      return false;
   }
   posix_spawnattr_destroy(&attr);
   posix_spawn_file_actions_destroy(&actions);
   close(fds[1]);
   fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

   buf      = (char*)malloc(cap);
   deadline = clipboard_now_ms() + CLIPBOARD_TIMEOUT_MS;

   while (buf && !eof && !full)
   {
      struct pollfd pfd;
      ssize_t n;
      long left = deadline - clipboard_now_ms();
      if (left <= 0)
         break;
      pfd.fd      = fds[0];
      pfd.events  = POLLIN;
      pfd.revents = 0;
      if (poll(&pfd, 1, (int)left) < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (!pfd.revents)
         continue;
      n = read(fds[0], buf + got, cap - got);
      if (n > 0)
      {
         got += (size_t)n;
         if (got >= cap)
            full = true;
      }
      else if (n == 0)
         eof = true;
      else if (errno != EINTR && errno != EAGAIN)
         break;
   }
   close(fds[0]);

   /* Give the tool what is left of its second to exit. */
   for (;;)
   {
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid)
      {
         exited    = true;
         reaped_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
         break;
      }
      if (r < 0 && errno == ECHILD)
      {
         /* SIGCHLD ignored elsewhere: already reaped, status lost. */
         exited    = true;
         reaped_ok = eof;
         break;
      }
      if (r < 0 && errno != EINTR)
         break;
      if (full || clipboard_now_ms() >= deadline)
         break;
      {
         struct timespec ts;
         ts.tv_sec  = 0;
         ts.tv_nsec = 5000000L;
         nanosleep(&ts, NULL);
      }
   }
   if (!exited)
   {
      kill(-pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
   }

   if (!buf)
      return false;
   if ((!(eof && reaped_ok) && !full) || got == 0)
   {
      free(buf);
      return false;
   }
   clipboard_copy_utf8(out, len, buf, got);
   free(buf);
   return true;
}
#endif

bool frontend_clipboard_get_text(char *out, size_t len)
{
   bool ok = false;

   if (!out || len == 0)
      return false;
   out[0] = '\0';

#if defined(CLIPBOARD_WIN32)
   if (     IsClipboardFormatAvailable(CF_UNICODETEXT)
         && OpenClipboard(NULL))
   {
      HANDLE data = GetClipboardData(CF_UNICODETEXT);
      if (data)
      {
         const WCHAR *wide = (const WCHAR*)GlobalLock(data);
         if (wide)
         {
            int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                  NULL, 0, NULL, NULL);
            if (need > 1)
            {
               char *utf8 = (char*)malloc((size_t)need);
               if (utf8)
               {
                  if (WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                           utf8, need, NULL, NULL) == need)
                  {
                     clipboard_copy_utf8(out, len, utf8,
                           (size_t)(need - 1));
                     ok = true;
                  }
                  free(utf8);
               }
            }
            GlobalUnlock(data);
         }
      }
      CloseClipboard();
   }
#elif defined(CLIPBOARD_UNIX)
   {
      const char *wayland = getenv("WAYLAND_DISPLAY");
      const char *x11     = getenv("DISPLAY");
      if (!ok && wayland && *wayland)
      {
         char *argv[5];
         argv[0] = (char*)"wl-paste";
         argv[1] = (char*)"-n";
         argv[2] = (char*)"-t";
         argv[3] = (char*)"text";
         argv[4] = NULL;
         ok      = clipboard_run(argv, out, len);
      }
      if (!ok && x11 && *x11)
      {
         char *argv[5];
         argv[0] = (char*)"xclip";
         argv[1] = (char*)"-o";
         argv[2] = (char*)"-selection";
         argv[3] = (char*)"clipboard";
         argv[4] = NULL;
         ok      = clipboard_run(argv, out, len);
      }
      if (!ok && x11 && *x11)
      {
         char *argv[3];
         argv[0] = (char*)"xsel";
         argv[1] = (char*)"-ob";
         argv[2] = NULL;
         ok      = clipboard_run(argv, out, len);
      }
   }
#endif

   if (!ok)
   {
      out[0] = '\0';
      return false;
   }
   clipboard_clean_text(out);
   return true;
}
