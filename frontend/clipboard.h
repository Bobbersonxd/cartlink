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

#ifndef __FRONTEND_CLIPBOARD_H
#define __FRONTEND_CLIPBOARD_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Reads the system clipboard as UTF-8 text into 'out' (at most
 * len - 1 bytes, always terminated) and cleans it with
 * clipboard_clean_text().
 *
 * Win32: CF_UNICODETEXT. Linux/BSD: wl-paste (Wayland), else
 * xclip, else xsel, each given at most one second.
 *
 * Returns false (out = "") when no clipboard tool or no text is
 * available, or on other platforms. */
bool frontend_clipboard_get_text(char *out, size_t len);

/* Drops line breaks and other control bytes, then trims
 * surrounding spaces and tabs, in place. */
void clipboard_clean_text(char *s);

RETRO_END_DECLS

#endif
