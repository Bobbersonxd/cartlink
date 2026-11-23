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

/* "Load from URL" safety checks: a downloaded file that is a program
 * (not a game) is refused, and so is a .zip/.7z whose members are
 * programs, use unsafe paths or unpack to too much. Ruling: plain DOS
 * executables (MZ without a PE header) and .com/.bat files are fine
 * as members of an archive (DOS cores run them), never as a bare
 * download; inside archives other names (.pif, .sys, .dll, ...) are
 * judged by their bytes too (Ruling R2). Nothing here extracts an archive. */

#ifndef __RARCH_URL_SAFETY_H
#define __RARCH_URL_SAFETY_H

#ifdef HAVE_NETWORKING

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define URL_SAFETY_MAX_TOTAL    ((uint64_t)2048 * 1024 * 1024) /* 2 GiB */
#define URL_SAFETY_MAX_MEMBERS  10000
#define URL_SAFETY_MAX_RATIO    1000
/* The ratio rule only looks at members at least this big: smaller
 * ones cannot blow up (and blank padding compresses very well). */
#define URL_SAFETY_RATIO_FLOOR  ((uint64_t)1024 * 1024)
/* How many first bytes of a file url_safety_check_file wants. */
#define URL_SAFETY_HEAD_LEN     65536

enum url_safety_verdict
{
   URL_SAFE_OK = 0,
   URL_SAFE_PROGRAM,          /* a program or script, not a game */
   URL_SAFE_ARCHIVE_TRAP,     /* archive member is a program, unsafe path or bomb */
   URL_SAFE_ARCHIVE_TOO_BIG,  /* archive unpacks to more than 2 GiB */
   URL_SAFE_UNREADABLE        /* could not be checked (refused) */
};

/* name = final cache file name, head = first bytes (>= 4096 or whole
 * file). PROGRAM for a listed program extension, or for PE, ELF,
 * Mach-O, script (#!), Windows shortcut or OLE/MSI bytes whatever the
 * name. A bare DOS MZ header (no PE) is a program only when the name
 * ends in .exe/.com/.bat (which the extension list blocks anyway). */
enum url_safety_verdict url_safety_check_file(const char *name,
      const uint8_t *head, size_t head_len);

/* Opens a .zip/.7z on disk (by its extension; anything else is OK)
 * and checks every member from the archive's headers before
 * RetroArch extracts it: unsafe paths (absolute, "..", drive letters,
 * backslash traversal), members whose bytes are PE, ELF, Mach-O,
 * script, shortcut or OLE (DOS MZ and Win16 NE are fine), members with
 * a script/shortcut/installer name (ps1, vbs, js, hta, lnk, url, reg,
 * msi, jar, desktop, sh, ...: Ruling R2), more than 10,000 members, a
 * member over 1000:1 compression, more than 2 GiB in total. */
enum url_safety_verdict url_safety_check_archive(const char *path);

/* Windows: writes the Zone.Identifier stream (ZoneId=3 + HostUrl,
 * without any user:password@ part or #fragment) so Windows treats the
 * file as downloaded from the internet, unless the file already has
 * one. No-op elsewhere. */
void url_safety_mark_downloaded(const char *path, const char *url);

/* The Zone.Identifier text url_safety_mark_downloaded writes
 * (exposed for tests; works on every platform). */
void url_safety_zone_text(char *out, size_t len, const char *url);

RETRO_END_DECLS

#endif /* HAVE_NETWORKING */

#endif
