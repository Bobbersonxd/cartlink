/*  Cartlink - check_link: runs one link through Cartlink's own
 *  fingerprint parser (url_content_split_fingerprint).
 *  Copyright (C) 2026 Cartlink contributors
 *
 *  This program is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Usage: check_link LINK
 * Prints the link without its fragment on line 1 and the fingerprint as
 * 64 lower-case hex digits on line 2. Exits 1 if the parser refuses the
 * link, 2 if it has no fingerprint. Used by
 * tools/cartlink-server/test_server.sh to prove that cartlink-link's
 * output is accepted by Cartlink. */

#include <stdio.h>
#include <string.h>
#include <boolean.h>

#include "../../network/url_content.h"

int main(int argc, char **argv)
{
   char    url[4096];
   uint8_t fp[32];
   bool    has_fp = false;
   int     i;

   if (argc != 2)
   {
      fprintf(stderr, "usage: check_link LINK\n");
      return 64;
   }
   if (!url_content_split_fingerprint(url, sizeof(url), fp, &has_fp, argv[1]))
   {
      fprintf(stderr, "check_link: refused\n");
      return 1;
   }
   if (!has_fp)
   {
      fprintf(stderr, "check_link: no fingerprint\n");
      return 2;
   }
   printf("%s\n", url);
   for (i = 0; i < 32; i++)
      printf("%02x", fp[i]);
   printf("\n");
   return 0;
}
