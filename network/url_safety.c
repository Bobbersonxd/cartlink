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

#ifdef HAVE_NETWORKING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <encodings/utf.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#ifdef HAVE_7ZIP
#include <7zip/7z.h>
#include <7zip/7zCrc.h>
#include <7zip/7zFile.h>
#endif

#if defined(_WIN32) && !defined(__WINRT__) && !defined(_XBOX)
#define US_WINDOWS_MARK
#include <windows.h>
#endif

#include "url_safety.h"

/* Program file types, lower case, without the dot (spec list). */
static const char *const us_program_exts[] = {
   "exe", "com", "scr", "pif", "msi", "msp", "dll", "sys", "cpl", "bat",
   "cmd", "ps1", "psm1", "vbs", "vbe", "js", "jse", "wsf", "wsh", "hta",
   "jar", "lnk", "url", "reg", "inf", "scf", "app", "dmg", "pkg", "sh",
   "bash", "command", "desktop", "so", "dylib", "appimage", "apk", "deb",
   "rpm", NULL
};

#define US_EXT_LEN        16
#define US_ZIP_CD_MAX     ((uint64_t)16 * 1024 * 1024) /* central directory */
#define US_ZIP_SEEK_MAX   ((uint64_t)64 * 1024 * 1024) /* inflated to find a PE header */
#define US_7Z_SNIFF_ALL   ((uint64_t)64 * 1024 * 1024) /* solid blocks always unpacked */
#define US_7Z_DECODE_MAX  ((uint64_t)512 * 1024 * 1024) /* only for .exe/.com members */

/* What the first bytes say. */
enum us_kind
{
   US_DATA = 0,
   US_PROGRAM,   /* PE, ELF, Mach-O, script, shortcut, OLE/MSI */
   US_DOS,       /* MZ with no PE header */
   US_MZ_FAR     /* MZ whose PE offset lies past the bytes we have */
};

/* ---------- names ---------- */

/* Lower-case extension of name[0..n) (trailing dots and spaces dropped,
 * as Windows does). Empty if none or longer than US_EXT_LEN - 1. */
static void us_ext(const char *name, size_t n, char *ext)
{
   size_t end = n;
   size_t i;
   ext[0] = '\0';
   while (end > 0 && (name[end - 1] == '.' || name[end - 1] == ' '))
      end--;
   for (i = end; i > 0; i--)
   {
      if (name[i - 1] == '.')
      {
         size_t len = end - i;
         size_t k;
         if (!len || len >= US_EXT_LEN || i == 1)
            return;           /* ".exe" alone is a hidden name, no ext */
         for (k = 0; k < len; k++)
         {
            char c = name[i + k];
            ext[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
         }
         ext[len] = '\0';
         return;
      }
   }
}

static bool us_ext_in(const char *ext, const char *const *list)
{
   size_t i;
   if (!ext[0])
      return false;
   for (i = 0; list[i]; i++)
      if (string_is_equal(ext, list[i]))
         return true;
   return false;
}

static bool us_ext_is_program(const char *ext)
{
   return us_ext_in(ext, us_program_exts);
}

/* DOS programs: fine inside an archive (Ruling R1). */
static bool us_ext_is_dos(const char *ext)
{
   return     string_is_equal(ext, "exe")
           || string_is_equal(ext, "com")
           || string_is_equal(ext, "bat");
}

/* DOS programs that start with an MZ header to look at. */
static bool us_ext_is_dos_binary(const char *ext)
{
   return string_is_equal(ext, "exe") || string_is_equal(ext, "com");
}

/* ---------- bytes ---------- */

static uint32_t us_le32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
      | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t us_le16(const uint8_t *p)
{
   return (uint16_t)(p[0] | (p[1] << 8));
}

static uint64_t us_le64(const uint8_t *p)
{
   return (uint64_t)us_le32(p) | ((uint64_t)us_le32(p + 4) << 32);
}

/* Classifies the first n bytes of a file. For US_MZ_FAR, *pe_off is
 * where the "PE\0\0" would have to be. */
static enum us_kind us_sniff(const uint8_t *h, size_t n, uint32_t *pe_off)
{
   if (!h)
      return US_DATA;
   if (n >= 4)
   {
      uint32_t be = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16)
         | ((uint32_t)h[2] << 8) | (uint32_t)h[3];
      if (     be == 0x7F454C46u   /* ELF */
            || be == 0xFEEDFACEu || be == 0xFEEDFACFu  /* Mach-O BE */
            || be == 0xCEFAEDFEu || be == 0xCFFAEDFEu  /* Mach-O LE */
            || be == 0xCAFEBABEu || be == 0xCAFEBABFu  /* fat (32/64) */
            || be == 0xD0CF11E0u)  /* OLE: .msi, old Office macros */
         return US_PROGRAM;
   }
   if (n >= 8 && !memcmp(h, "\x4c\x00\x00\x00\x01\x14\x02\x00", 8))
      return US_PROGRAM;           /* Windows shortcut (.lnk) */
   if (n >= 3 && h[0] == '#' && h[1] == '!')
   {
      /* "#!/..." or "#! /...": a script for some interpreter. */
      size_t i = 2;
      while (i < n && h[i] == ' ')
         i++;
      if (i < n && h[i] == '/')
         return US_PROGRAM;
   }
   if (n >= 2 && h[0] == 'M' && h[1] == 'Z')
   {
      uint32_t off;
      if (n < 0x40)
         return US_DOS;
      off = us_le32(h + 0x3C);
      if (off <= n - 4)
         return memcmp(h + off, "PE\0\0", 4) == 0 ? US_PROGRAM : US_DOS;
      if (pe_off)
         *pe_off = off;
      return US_MZ_FAR;
   }
   return US_DATA;
}

enum url_safety_verdict url_safety_check_file(const char *name,
      const uint8_t *head, size_t head_len)
{
   char ext[US_EXT_LEN];
   ext[0] = '\0';
   if (name)
   {
      /* Only the last path part counts. */
      const char *base = name;
      const char *p;
      for (p = name; *p; p++)
         if (*p == '/' || *p == '\\')
            base = p + 1;
      us_ext(base, strlen(base), ext);
   }
   if (us_ext_is_program(ext))
      return URL_SAFE_PROGRAM;
   switch (us_sniff(head, head_len, NULL))
   {
      case US_PROGRAM:
         return URL_SAFE_PROGRAM;
      case US_DOS:
      case US_MZ_FAR:
         /* A bare DOS program only by name (also on the list above):
          * ROMs may start with "MZ" by chance. */
         if (us_ext_is_dos(ext))
            return URL_SAFE_PROGRAM;
         break;
      default:
         break;
   }
   return URL_SAFE_OK;
}

/* ---------- archive members ---------- */

struct us_member
{
   char ext[US_EXT_LEN];
   bool is_dir;
   bool dos_binary;   /* .exe/.com: its MZ header must be checked */
};

/* Checks one member name (any bytes, length n). TRAP for unsafe
 * paths and program types other than DOS ones. */
static enum url_safety_verdict us_check_member_name(const char *name,
      size_t n, struct us_member *m)
{
   size_t start = 0;
   size_t i;

   memset(m, 0, sizeof(*m));
   if (!n)
      return URL_SAFE_ARCHIVE_TRAP;
   if (name[0] == '/' || name[0] == '\\')
      return URL_SAFE_ARCHIVE_TRAP;            /* absolute */
   if (n >= 2 && name[1] == ':'
         && ((name[0] >= 'A' && name[0] <= 'Z')
          || (name[0] >= 'a' && name[0] <= 'z')))
      return URL_SAFE_ARCHIVE_TRAP;            /* drive letter */

   m->is_dir = (name[n - 1] == '/' || name[n - 1] == '\\');

   for (i = 0; i <= n; i++)
   {
      if (i < n && name[i] == '\0')
         return URL_SAFE_ARCHIVE_TRAP;         /* hidden cut in the name */
      if (i == n || name[i] == '/' || name[i] == '\\')
      {
         size_t len  = i - start;
         size_t dots = 0;
         size_t k;
         bool only_dots = len > 0;
         for (k = start; k < i; k++)
         {
            if (name[k] == '.')
               dots++;
            else if (name[k] != ' ')
               only_dots = false;
         }
         /* "..", and "..." / ".. " which Windows reads as "..". */
         if (only_dots && dots >= 2)
            return URL_SAFE_ARCHIVE_TRAP;
         if (len)
         {
            char ext[US_EXT_LEN];
            us_ext(name + start, len, ext);
            if (i == n && !m->is_dir)
            {
               strlcpy(m->ext, ext, sizeof(m->ext));
               if (us_ext_is_program(ext) && !us_ext_is_dos(ext))
                  return URL_SAFE_ARCHIVE_TRAP;
               m->dos_binary = us_ext_is_dos_binary(ext);
            }
            else if (string_is_equal(ext, "app"))
               return URL_SAFE_ARCHIVE_TRAP;   /* macOS app bundle */
         }
         start = i + 1;
      }
   }
   return URL_SAFE_OK;
}

static bool us_is_bomb(uint64_t packed, uint64_t unpacked)
{
   if (unpacked < URL_SAFETY_RATIO_FLOOR)
      return false;
   return unpacked / URL_SAFETY_MAX_RATIO > packed
      || (unpacked / URL_SAFETY_MAX_RATIO == packed
          && unpacked % URL_SAFETY_MAX_RATIO);
}

/* Verdict for a member's bytes. data[0..n) are its first bytes
 * (n == size when the whole member is there); readable false when
 * none could be read. read_at(ctx, off, buf4) fetches 4 bytes at off
 * for a far PE offset (may be NULL). */
typedef bool (*us_read_at_t)(void *ctx, uint64_t off, uint8_t *out4);

static enum url_safety_verdict us_check_member_bytes(
      const struct us_member *m, bool readable,
      const uint8_t *data, size_t n, uint64_t size,
      us_read_at_t read_at, void *ctx)
{
   uint32_t pe_off = 0;
   if (!readable)
      /* A DOS .exe/.com must be shown to be DOS, not Windows. */
      return m->dos_binary ? URL_SAFE_ARCHIVE_TRAP : URL_SAFE_OK;
   switch (us_sniff(data, n, &pe_off))
   {
      case US_PROGRAM:
         return URL_SAFE_ARCHIVE_TRAP;
      case US_MZ_FAR:
         {
            uint8_t sig[4];
            if ((uint64_t)pe_off + 4 > size)
               return URL_SAFE_OK;     /* past the end: plain DOS */
            if (!read_at || !read_at(ctx, pe_off, sig))
               return m->dos_binary ? URL_SAFE_ARCHIVE_TRAP : URL_SAFE_OK;
            if (!memcmp(sig, "PE\0\0", 4))
               return URL_SAFE_ARCHIVE_TRAP;
         }
         return URL_SAFE_OK;
      default:
         break;
   }
   return URL_SAFE_OK;
}

/* ---------- zip ---------- */

struct us_zip_entry
{
   uint64_t csize;
   uint64_t usize;
   uint64_t offset;
   uint16_t method;
   uint16_t flags;
   const char *name;
   size_t name_len;
};

/* Parses the central directory entry at p (avail bytes left). Returns
 * its total length, 0 if malformed. */
static size_t us_zip_entry(const uint8_t *p, size_t avail,
      struct us_zip_entry *e)
{
   size_t nlen, xlen, clen, total;
   const uint8_t *x;
   if (avail < 46 || us_le32(p) != 0x02014B50u)
      return 0;
   nlen  = us_le16(p + 28);
   xlen  = us_le16(p + 30);
   clen  = us_le16(p + 32);
   total = 46 + nlen + xlen + clen;
   if (total > avail)
      return 0;
   e->flags    = us_le16(p + 8);
   e->method   = us_le16(p + 10);
   e->csize    = us_le32(p + 20);
   e->usize    = us_le32(p + 24);
   e->offset   = us_le32(p + 42);
   e->name     = (const char*)p + 46;
   e->name_len = nlen;

   /* ZIP64: the 0xFFFFFFFF fields come from extra field 0x0001, in
    * the order usize, csize, offset. */
   x = p + 46 + nlen;
   while (xlen >= 4)
   {
      size_t id  = us_le16(x);
      size_t len = us_le16(x + 2);
      if (len + 4 > xlen)
         return 0;
      if (id == 0x0001)
      {
         const uint8_t *v = x + 4;
         size_t left      = len;
         if (e->usize == 0xFFFFFFFFu)
         {
            if (left < 8)
               return 0;
            e->usize = us_le64(v);
            v += 8; left -= 8;
         }
         if (e->csize == 0xFFFFFFFFu)
         {
            if (left < 8)
               return 0;
            e->csize = us_le64(v);
            v += 8; left -= 8;
         }
         if (e->offset == 0xFFFFFFFFu)
         {
            if (left < 8)
               return 0;
            e->offset = us_le64(v);
         }
      }
      x    += 4 + len;
      xlen -= 4 + len;
   }
   return total;
}

struct us_zip_reader
{
   RFILE *f;
   uint64_t file_size;
   uint64_t data_off;   /* start of the member's (compressed) bytes */
   const struct us_zip_entry *e;
};

static bool us_read_exact(RFILE *f, uint64_t off, void *buf, size_t len)
{
   if (filestream_seek(f, (int64_t)off, RETRO_VFS_SEEK_POSITION_START) < 0)
      return false;
   return filestream_read(f, buf, (int64_t)len) == (int64_t)len;
}

/* Finds where the member's data starts (after its local header). */
static bool us_zip_locate(struct us_zip_reader *r)
{
   uint8_t lh[30];
   if (     r->e->offset + 30 > r->file_size
         || !us_read_exact(r->f, r->e->offset, lh, sizeof(lh))
         || us_le32(lh) != 0x04034B50u)
      return false;
   r->data_off = r->e->offset + 30 + us_le16(lh + 26) + us_le16(lh + 28);
   return r->data_off <= r->file_size;
}

/* Up to want bytes of the member starting at uncompressed offset skip.
 * Returns how many were read; (size_t)-1 if the member can't be read
 * (encrypted, unknown method, broken data). Never inflates more than
 * skip + want bytes. */
static size_t us_zip_read(struct us_zip_reader *r, uint64_t skip,
      uint8_t *out, size_t want)
{
   const struct us_zip_entry *e = r->e;
   uint64_t avail;

   if (e->flags & 1)
      return (size_t)-1;                      /* encrypted */
   avail = r->file_size - r->data_off;
   if (avail > e->csize)
      avail = e->csize;

   if (e->method == 0)
   {
      size_t n;
      if (skip >= avail)
         return 0;
      n = (avail - skip < want) ? (size_t)(avail - skip) : want;
      return us_read_exact(r->f, r->data_off + skip, out, n) ? n : (size_t)-1;
   }
#ifdef HAVE_ZLIB
   if (e->method == 8)
   {
      uint8_t in[16384];
      uint8_t scratch[16384];
      z_stream zs;
      uint64_t in_left  = avail;
      uint64_t produced = 0;
      size_t got        = 0;
      int zret          = Z_OK;

      memset(&zs, 0, sizeof(zs));
      if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
         return (size_t)-1;
      if (filestream_seek(r->f, (int64_t)r->data_off,
               RETRO_VFS_SEEK_POSITION_START) < 0)
      {
         inflateEnd(&zs);
         return (size_t)-1;
      }
      while (got < want && zret != Z_STREAM_END)
      {
         if (!zs.avail_in)
         {
            int64_t rd;
            size_t chunk = in_left < sizeof(in) ? (size_t)in_left : sizeof(in);
            if (!chunk)
               break;
            rd = filestream_read(r->f, in, (int64_t)chunk);
            if (rd <= 0)
               break;
            in_left    -= (uint64_t)rd;
            zs.next_in  = in;
            zs.avail_in = (uInt)rd;
         }
         if (produced < skip)
         {
            uint64_t left = skip - produced;
            zs.next_out  = scratch;
            zs.avail_out = (uInt)(left < sizeof(scratch) ? left : sizeof(scratch));
         }
         else
         {
            zs.next_out  = out + got;
            zs.avail_out = (uInt)(want - got);
         }
         {
            uInt before = zs.avail_out;
            zret        = inflate(&zs, Z_NO_FLUSH);
            if (zret != Z_OK && zret != Z_STREAM_END)
            {
               if (zret == Z_BUF_ERROR && before != zs.avail_out)
                  zret = Z_OK;
               else
               {
                  inflateEnd(&zs);
                  return got ? got : (size_t)-1;
               }
            }
            if (produced < skip)
               produced += before - zs.avail_out;
            else
               got      += before - zs.avail_out;
         }
      }
      inflateEnd(&zs);
      return got;
   }
#endif
   return (size_t)-1;
}

static bool us_zip_read_at(void *ctx, uint64_t off, uint8_t *out4)
{
   struct us_zip_reader *r = (struct us_zip_reader*)ctx;
   if (off > US_ZIP_SEEK_MAX)
      return false;
   return us_zip_read(r, off, out4, 4) == 4;
}

static enum url_safety_verdict us_check_zip(const char *path)
{
   uint8_t tail[65535 + 22];
   uint8_t *cd     = NULL;
   RFILE *f        = NULL;
   int64_t size;
   size_t tail_len, i, pos;
   uint64_t entries, cd_size, cd_off, total = 0, n;
   uint64_t eocd_at;
   enum url_safety_verdict v = URL_SAFE_UNREADABLE;
   uint8_t *head   = NULL;

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return URL_SAFE_UNREADABLE;
   size = filestream_get_size(f);
   if (size < 22)
      goto end;

   /* End of central directory: the last "PK\5\6" whose comment fits. */
   tail_len = (uint64_t)size < sizeof(tail) ? (size_t)size : sizeof(tail);
   if (!us_read_exact(f, (uint64_t)size - tail_len, tail, tail_len))
      goto end;
   for (i = tail_len - 22 + 1; i-- > 0; )
   {
      if (     us_le32(tail + i) == 0x06054B50u
            && i + 22 + us_le16(tail + i + 20) <= tail_len)
         break;
      if (i == 0)
         goto end;
   }
   eocd_at = (uint64_t)size - tail_len + i;
   entries = us_le16(tail + i + 10);
   cd_size = us_le32(tail + i + 12);
   cd_off  = us_le32(tail + i + 16);

   if (entries == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu)
   {
      /* ZIP64 locator just before, pointing at the ZIP64 record. */
      uint8_t loc[20];
      uint8_t rec[56];
      uint64_t rec_at;
      if (     eocd_at < 20
            || !us_read_exact(f, eocd_at - 20, loc, sizeof(loc))
            || us_le32(loc) != 0x07064B50u)
         goto end;
      rec_at = us_le64(loc + 8);
      if (     rec_at + sizeof(rec) > (uint64_t)size
            || !us_read_exact(f, rec_at, rec, sizeof(rec))
            || us_le32(rec) != 0x06064B50u)
         goto end;
      entries = us_le64(rec + 32);
      cd_size = us_le64(rec + 40);
      cd_off  = us_le64(rec + 48);
   }

   if (entries > URL_SAFETY_MAX_MEMBERS)
   {
      v = URL_SAFE_ARCHIVE_TRAP;
      goto end;
   }
   if (     cd_size > US_ZIP_CD_MAX
         || cd_off > (uint64_t)size
         || cd_size > (uint64_t)size - cd_off
         || !(cd = (uint8_t*)malloc(cd_size ? (size_t)cd_size : 1))
         || !us_read_exact(f, cd_off, cd, (size_t)cd_size))
      goto end;

   /* Pass 1, headers only: names, count, sizes, ratios. */
   for (pos = 0, n = 0; pos < cd_size; n++)
   {
      struct us_zip_entry e;
      struct us_member m;
      size_t len = us_zip_entry(cd + pos, (size_t)cd_size - pos, &e);
      if (!len)
         goto end;
      if (n >= URL_SAFETY_MAX_MEMBERS)
      {
         v = URL_SAFE_ARCHIVE_TRAP;
         goto end;
      }
      if (us_check_member_name(e.name, e.name_len, &m) != URL_SAFE_OK)
      {
         v = URL_SAFE_ARCHIVE_TRAP;
         goto end;
      }
      if (us_is_bomb(e.csize, e.usize))
      {
         v = URL_SAFE_ARCHIVE_TRAP;
         goto end;
      }
      total += e.usize;
      if (total < e.usize)               /* wrapped */
         total = (uint64_t)-1;
      pos   += len;
   }
   if (n != entries && entries != 0xFFFF)
      goto end;
   if (total > URL_SAFETY_MAX_TOTAL)
   {
      v = URL_SAFE_ARCHIVE_TOO_BIG;
      goto end;
   }

   /* Pass 2: the first bytes of every member. */
   if (!(head = (uint8_t*)malloc(URL_SAFETY_HEAD_LEN)))
      goto end;
   for (pos = 0; pos < cd_size; )
   {
      struct us_zip_entry e;
      struct us_member m;
      struct us_zip_reader r;
      size_t got   = (size_t)-1;
      size_t len   = us_zip_entry(cd + pos, (size_t)cd_size - pos, &e);
      pos         += len;
      us_check_member_name(e.name, e.name_len, &m);
      if (m.is_dir || !e.usize)
         continue;
      r.f         = f;
      r.file_size = (uint64_t)size;
      r.e         = &e;
      if (us_zip_locate(&r))
         got = us_zip_read(&r, 0, head, URL_SAFETY_HEAD_LEN);
      if (us_check_member_bytes(&m, got != (size_t)-1, head,
               got == (size_t)-1 ? 0 : got, e.usize,
               us_zip_read_at, &r) != URL_SAFE_OK)
      {
         v = URL_SAFE_ARCHIVE_TRAP;
         goto end;
      }
   }
   v = URL_SAFE_OK;

end:
   free(head);
   free(cd);
   filestream_close(f);
   return v;
}

/* ---------- 7z ---------- */

#ifdef HAVE_7ZIP
static void *us_7z_alloc(ISzAllocPtr p, size_t len)
{
   /* The SDK sizes buffers from the archive's own claims: never more
    * than one solid block we are willing to unpack. */
   if (!len || (uint64_t)len > US_7Z_DECODE_MAX + 1024 * 1024)
      return NULL;
   return malloc(len);
}

static void us_7z_free(ISzAllocPtr p, void *address)
{
   free(address);
}

/* ASCII copy of member i's name; anything else becomes '_' (enough
 * for the path and extension rules). Returns its length. */
static size_t us_7z_name(const CSzArEx *db, uint32_t i, char **buf,
      size_t *cap)
{
   size_t len = SzArEx_GetFileNameUtf16(db, i, NULL); /* incl. NUL */
   uint16_t *w;
   size_t k;
   if (!len)
      return 0;
   if (!(w = (uint16_t*)malloc(len * sizeof(*w))))
      return 0;
   if (len > *cap)
   {
      char *tmp = (char*)realloc(*buf, len);
      if (!tmp)
      {
         free(w);
         return 0;
      }
      *buf = tmp;
      *cap = len;
   }
   SzArEx_GetFileNameUtf16(db, i, w);
   for (k = 0; k + 1 < len; k++)
      (*buf)[k] = (w[k] && w[k] < 0x80) ? (char)w[k]
         : (w[k] ? '_' : '\0');
   (*buf)[len - 1] = '\0';
   free(w);
   return len - 1;
}

static enum url_safety_verdict us_check_7z(const char *path)
{
   CFileInStream archive;
   CLookToRead2 look;
   CSzArEx db;
   ISzAlloc alloc;
   uint8_t *folder_flags = NULL; /* 1: has .exe/.com, 2: unpack it */
   char *name            = NULL;
   size_t name_cap       = 0;
   Byte *out             = NULL;
   size_t out_size       = 0;
   uint32_t block        = 0xFFFFFFFF;
   uint64_t total        = 0;
   uint32_t i;
   bool opened           = false;
   enum url_safety_verdict v = URL_SAFE_UNREADABLE;

   alloc.Alloc  = us_7z_alloc;
   alloc.Free   = us_7z_free;
   memset(&look, 0, sizeof(look));
   memset(&archive, 0, sizeof(archive));

#if defined(_WIN32) && defined(USE_WINDOWS_FILE) && !defined(LEGACY_WIN32)
   {
      wchar_t *path_w = utf8_to_utf16_string_alloc(path);
      WRes bad        = path_w ? InFile_OpenW(&archive.file, path_w) : 1;
      free(path_w);
      if (bad)
         return URL_SAFE_UNREADABLE;
   }
#else
   if (InFile_Open(&archive.file, path))
      return URL_SAFE_UNREADABLE;
#endif
   look.bufSize = 1 << 14;
   if (!(look.buf = (Byte*)malloc(look.bufSize)))
   {
      File_Close(&archive.file);
      return URL_SAFE_UNREADABLE;
   }
   FileInStream_CreateVTable(&archive);
   LookToRead2_CreateVTable(&look, false);
   look.realStream = &archive.vt;
   LookToRead2_Init(&look);
   CrcGenerateTable();
   SzArEx_Init(&db);
   if (SzArEx_Open(&db, &look.vt, &alloc, &alloc) != SZ_OK)
      goto end;
   opened = true;

   if (db.NumFiles > URL_SAFETY_MAX_MEMBERS)
   {
      v = URL_SAFE_ARCHIVE_TRAP;
      goto end;
   }
   if (!(folder_flags = (uint8_t*)calloc(db.db.NumFolders + 1, 1)))
      goto end;

   /* Pass 1, headers only. */
   for (i = 0; i < db.NumFiles; i++)
   {
      struct us_member m;
      size_t len = us_7z_name(&db, i, &name, &name_cap);
      uint64_t size;
      if (!len)
         goto end;
      if (us_check_member_name(name, len, &m) != URL_SAFE_OK)
      {
         v = URL_SAFE_ARCHIVE_TRAP;
         goto end;
      }
      if (SzArEx_IsDir(&db, i))
         continue;
      size   = SzArEx_GetFileSize(&db, i);
      total += size;
      if (total < size)
         total = (uint64_t)-1;
      if (m.dos_binary && db.FileToFolder[i] != (uint32_t)-1)
         folder_flags[db.FileToFolder[i]] |= 1;
   }
   for (i = 0; i < db.db.NumFolders; i++)
   {
      uint64_t unpack = SzAr_GetFolderUnpackSize(&db.db, i);
      uint64_t packed = db.db.PackPositions[db.db.FoStartPackStreamIndex[i + 1]]
                      - db.db.PackPositions[db.db.FoStartPackStreamIndex[i]];
      if (us_is_bomb(packed, unpack))
      {
         v = URL_SAFE_ARCHIVE_TRAP;
         goto end;
      }
      if (     unpack <= US_7Z_SNIFF_ALL
            || ((folder_flags[i] & 1) && unpack <= US_7Z_DECODE_MAX))
         folder_flags[i] |= 2;
      else if (folder_flags[i] & 1)
      {
         /* A DOS .exe/.com we cannot unpack to look at. */
         v = URL_SAFE_ARCHIVE_TRAP;
         goto end;
      }
   }
   if (total > URL_SAFETY_MAX_TOTAL)
   {
      v = URL_SAFE_ARCHIVE_TOO_BIG;
      goto end;
   }

   /* Pass 2: whole members of the solid blocks we unpack. */
   for (i = 0; i < db.NumFiles; i++)
   {
      struct us_member m;
      uint32_t folder = db.FileToFolder[i];
      size_t offset   = 0;
      size_t got      = 0;
      size_t len;
      bool ok;
      if (SzArEx_IsDir(&db, i) || folder == (uint32_t)-1
            || !(folder_flags[folder] & 2))
         continue;
      len = us_7z_name(&db, i, &name, &name_cap);
      us_check_member_name(name, len, &m);
      ok  = SzArEx_Extract(&db, &look.vt, i, &block, &out, &out_size,
            &offset, &got, &alloc, &alloc) == SZ_OK;
      if (us_check_member_bytes(&m, ok, ok ? out + offset : NULL,
               ok ? got : 0, ok ? (uint64_t)got : 0, NULL, NULL)
            != URL_SAFE_OK)
      {
         v = URL_SAFE_ARCHIVE_TRAP;
         goto end;
      }
   }
   v = URL_SAFE_OK;

end:
   if (out)
      IAlloc_Free(&alloc, out);
   free(name);
   free(folder_flags);
   if (opened)
      SzArEx_Free(&db, &alloc);
   File_Close(&archive.file);
   free(look.buf);
   return v;
}
#endif

enum url_safety_verdict url_safety_check_archive(const char *path)
{
   char ext[US_EXT_LEN];
   const char *base;
   const char *p;
   if (string_is_empty(path))
      return URL_SAFE_UNREADABLE;
   for (base = p = path; *p; p++)
      if (*p == '/' || *p == '\\')
         base = p + 1;
   us_ext(base, strlen(base), ext);
   if (string_is_equal(ext, "zip"))
      return us_check_zip(path);
   if (string_is_equal(ext, "7z"))
   {
#ifdef HAVE_7ZIP
      return us_check_7z(path);
#else
      return URL_SAFE_UNREADABLE;  /* cannot look inside: refuse */
#endif
   }
   return URL_SAFE_OK;
}

/* ---------- Windows mark of the web ---------- */

void url_safety_zone_text(char *out, size_t len, const char *url)
{
   char clean[4096];
   size_t o = 0;
   const char *p;
   const char *auth;
   const char *auth_end;
   const char *at = NULL;

   if (!out || !len)
      return;
   out[0] = '\0';
   if (!url)
      url = "";

   /* Drop user:password@ from the authority. */
   auth = strstr(url, "://");
   auth = auth ? auth + 3 : url;
   for (auth_end = auth; *auth_end && *auth_end != '/' && *auth_end != '?'
         && *auth_end != '#'; auth_end++)
      if (*auth_end == '@')
         at = auth_end;

   for (p = url; *p && *p != '#' && o + 1 < sizeof(clean); p++)
   {
      if (at && p >= auth && p <= at)
         continue;
      if ((unsigned char)*p < 0x20 || *p == 0x7F)
         continue;           /* no line breaks inside the INI value */
      clean[o++] = *p;
   }
   clean[o] = '\0';
   snprintf(out, len, "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=%s\r\n", clean);
}

void url_safety_mark_downloaded(const char *path, const char *url)
{
#ifdef US_WINDOWS_MARK
   char text[4200];
   char ads[4200];
   wchar_t *ads_w;
   HANDLE h;
   if (string_is_empty(path))
      return;
   url_safety_zone_text(text, sizeof(text), url);
   snprintf(ads, sizeof(ads), "%s:Zone.Identifier", path);
   if (!(ads_w = utf8_to_utf16_string_alloc(ads)))
      return;
   h = CreateFileW(ads_w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
         FILE_ATTRIBUTE_NORMAL, NULL);
   free(ads_w);
   if (h != INVALID_HANDLE_VALUE)
   {
      DWORD wrote = 0;
      WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL);
      CloseHandle(h);
   }
#else
   (void)path;
   (void)url;
#endif
}

#endif /* HAVE_NETWORKING */
