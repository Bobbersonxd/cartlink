/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 Cartlink contributors
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

/* Differential test: url_safety_check_archive against RetroArch's own
 * archive reader (libretro-common/file/archive_file*.c). For every
 * archive the check lets through, every member RetroArch lists is
 * extracted the way a core load would, and its bytes must not be a
 * program (PE, ELF, Mach-O, script, shortcut, OLE). A disagreement
 * means the check read different data than RetroArch extracts.
 * Built with -Wl,--wrap=malloc: every fresh buffer is filled with a
 * pattern that reads as a PE header ("PE\0\0" at 0x40, e_lfanew 0x40
 * at 0x3C), so bytes RetroArch hands out without reading them from
 * the file (a member shorter than its declared size) look like a
 * program instead of harmless zeros.
 * Usage: test_archive_agree <archive>...   (exit 1 on a disagreement) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <file/archive_file.h>
#include <lists/string_list.h>

#include "../../network/url_safety.h"

void *__real_malloc(size_t len);
void *__wrap_malloc(size_t len);
void *__wrap_malloc(size_t len)
{
   static const unsigned char unit[8] = {'P', 'E', 0, 0, 0x40, 0, 0, 0};
   unsigned char *p = (unsigned char*)__real_malloc(len);
   size_t i;
   if (p)
      for (i = 0; i < len; i++)
         p[i] = unit[i & 7];
   return p;
}

int main(int argc, char **argv)
{
   int i;
   int checked = 0, bad = 0, members = 0;
   for (i = 1; i < argc; i++)
   {
      struct string_list *list;
      size_t k;
      if (url_safety_check_archive(argv[i]) != URL_SAFE_OK)
         continue;            /* refused: nothing gets extracted */
      checked++;
      if (!(list = file_archive_get_file_list(argv[i], NULL)))
         continue;            /* RetroArch can't open it either */
      for (k = 0; k < list->size; k++)
      {
         char inner[4096];
         void *buf   = NULL;
         int64_t len = 0;
         snprintf(inner, sizeof(inner), "%s#%s", argv[i],
               list->elems[k].data);
         if (file_archive_compressed_read(inner, &buf, NULL, &len) && buf)
         {
            members++;
            /* A neutral name: only the bytes are judged here (names
             * are the check's own rules, not a reader difference). */
            if (url_safety_check_file("member.dat", (const uint8_t*)buf,
                     len > 0 ? (size_t)len : 0) != URL_SAFE_OK)
            {
               printf("FAIL %s: allowed, but RetroArch extracts a program as \"%s\"\n",
                     argv[i], list->elems[k].data);
               bad++;
            }
         }
         free(buf);
      }
      string_list_free(list);
   }
   if (bad)
   {
      printf("archive_agree: %d disagreement(s)\n", bad);
      return 1;
   }
   printf("archive_agree: %d allowed archives, %d members extracted by RetroArch, all agree\n",
         checked, members);
   return 0;
}
