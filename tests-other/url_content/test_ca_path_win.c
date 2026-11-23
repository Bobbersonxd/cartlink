/*  Copyright (C) 2026 Cartlink contributors
 *
 *  Cartlink is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. See COPYING.
 */

/* Windows-only check that the bundled cacert.pem is the one next to the
 * program, also when the program's folder has a long non-ASCII name
 * (more UTF-8 bytes than UTF-16 units). A cut path would point into
 * the parent folder, where "make win-test" plants a different
 * cacert.pem. Built with MinGW and run under Wine by "make win-test",
 * from <data>/<DIRNAME>/cartlink.exe (a name shorter than the
 * 20 extra UTF-8 bytes, so a cut path really leaves the folder). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <boolean.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <net/net_socket_ssl.h>
#include "../../deps/mbedtls/mbedtls/x509_crt.h"

/* 20 Cyrillic letters: 40 UTF-8 bytes, 20 UTF-16 units. */
#define DIRNAME "\xd0\x90\xd0\xbb\xd0\xb5\xd0\xba\xd1\x81\xd0\xb0\xd0\xbd\xd0\xb4\xd1\x80" \
                "\xd0\x9f\xd0\xb5\xd1\x82\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb8\xd1\x87" \
                "\xd0\xa1\xd0\xb8\xd0\xb4"

static int g_checks   = 0;
static int g_failures = 0;

#define EXPECT(x) do { \
   g_checks++; \
   if (!(x)) { \
      g_failures++; \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
   } \
} while (0)

static bool ends_with(const char *s, const char *tail)
{
   size_t a = strlen(s), b = strlen(tail);
   return a >= b && !strcmp(s + a - b, tail);
}

int main(void)
{
   char path[4096];
   char dir[4096];
   void *buf   = NULL;
   int64_t len = 0;
   bool ok;

   ok = ssl_socket_test_default_ca_bundle_path(path, sizeof(path));
   EXPECT(ok);
   EXPECT(ends_with(path, "\\" DIRNAME "\\cacert.pem"));

   /* The file there is the real one, not the parent's planted one. */
   EXPECT(filestream_read_file(path, &buf, &len) == 1);
   EXPECT(buf && strstr((const char*)buf, "CARTLINK-TEST-REAL-BUNDLE"));
   EXPECT(!buf || !strstr((const char*)buf, "PLANTED"));
   if (buf)
   {
      mbedtls_x509_crt crt;
      mbedtls_x509_crt_init(&crt);
      EXPECT(mbedtls_x509_crt_parse(&crt, (const unsigned char*)buf,
               (size_t)len + 1) == 0);
      EXPECT(crt.version != 0);
      mbedtls_x509_crt_free(&crt);
      free(buf);
   }

   /* The upstream helper agrees (it used to cut the UTF-8 path at the
    * UTF-16 length). */
   fill_pathname_application_dir(dir, sizeof(dir));
   EXPECT(ends_with(dir, "\\" DIRNAME "\\"));

   if (g_failures)
   {
      printf("ca path (Windows): %d of %d checks FAILED (path: %s)\n",
            g_failures, g_checks, path);
      return 1;
   }
   printf("ca path (Windows): all %d checks passed\n", g_checks);
   return 0;
}
