#!/usr/bin/env python3
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Writes THIRD-PARTY-NOTICES.txt for a finished build: the licence of
# every bundled third-party component that was compiled into it.
#
# Which components are in the binary is read from the build's object
# files (obj-*/**/*.o), so the list follows the configure options.
# A compiled deps/<name> missing from COMPONENTS stops the script, and
# so does any other compiled file whose header has a copyright or
# licence that is not GPL, RetroArch's or Cartlink's and that no
# OUTSIDE entry covers: add it (with its licence) before releasing.
#
# Usage: tools/third_party_notices.py <built source dir> <out file> windows|linux

import glob
import os
import re
import sys

SRC, OUT, PLATFORM = sys.argv[1], sys.argv[2], sys.argv[3]
if PLATFORM not in ("windows", "linux"):
    sys.exit("platform must be windows or linux")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().replace("\r\n", "\n")


def src(rel):
    return os.path.join(SRC, rel)


def leading_comments(path):
    """The comment blocks at the top of a source file, markers removed."""
    text = read(path)
    blocks, pos = [], 0
    while True:
        # Include guards (#ifndef/#define) may come before the notice.
        g = re.match(r"\s*#(?:ifndef|define)[^\n]*\n", text[pos:])
        if g:
            pos += g.end()
            continue
        m = re.match(r"\s*(/\*.*?\*/|(?://[^\n]*\n)+)", text[pos:], re.S)
        if not m:
            break
        raw = m.group(1)
        if raw.startswith("/*"):
            lines = raw[2:-2].split("\n")
            lines = [re.sub(r"^\s?\*\s?", "", l) if l.lstrip().startswith("*")
                     else l for l in lines]
        else:
            lines = [re.sub(r"^\s*// ?", "", l) for l in raw.rstrip("\n").split("\n")]
        blocks.append("\n".join(l.rstrip() for l in lines).strip("\n"))
        pos += m.end()
    return blocks


def licence_header(path):
    """Leading comment blocks that hold a copyright or licence line."""
    keep = [b for b in leading_comments(path)
            if re.search(r"copyright|licen[cs]e|public domain", b, re.I)]
    if not keep:
        sys.exit("no licence header in " + path)
    return "\n\n".join(keep)


def tail_licence(path, marker):
    """The licence text at the end of a single-header library."""
    text = read(path)
    i = text.rfind(marker)
    if i < 0:
        sys.exit("no licence text in " + path)
    return text[i:].rstrip().rstrip("*/").rstrip()


def compiled_sources(dep):
    """Sources of the objects built from deps/<dep>/."""
    out = []
    for obj in glob.glob(src("obj-*/**/deps/%s/**/*.o" % dep), recursive=True):
        rel = obj[obj.index("deps/"):-2]
        for ext in (".c", ".cpp", ".cc"):
            if os.path.exists(src(rel + ext)):
                out.append(src(rel + ext))
                break
    return sorted(out)


def distinct_headers(dep):
    """Every distinct licence header among the compiled files of dep."""
    seen = []
    for f in compiled_sources(dep):
        h = licence_header(f)
        if h not in seen:
            seen.append(h)
    return "\n\n-----\n\n".join(seen)


# name in deps/ -> (title, function giving the licence text)
COMPONENTS = {
    "mbedtls": ("Mbed TLS 2.28.10 (Apache-2.0)",
                lambda: read(src("deps/mbedtls/LICENSE"))),
    "glslang": ("glslang (Khronos Group; BSD-3-Clause and others, per file)",
                lambda: distinct_headers("glslang")),
    "SPIRV-Cross": ("SPIRV-Cross (Apache-2.0)",
                    lambda: read(src("deps/SPIRV-Cross/LICENSE"))),
    "libz": ("zlib 1.2.8 (zlib licence)",
             lambda: licence_header(src("libretro-common/include/compat/zlib/zlib.h"))),
    "7zip": ("LZMA SDK / 7-Zip decoder by Igor Pavlov (public domain)",
             lambda: licence_header(src("deps/7zip/7z.h"))),
    "rcheevos": ("rcheevos (MIT)",
                 lambda: read(src("deps/rcheevos/LICENSE"))),
    "ibxm": ("IBXM module player (BSD-3-Clause)",
             lambda: read(src("deps/ibxm/licence.txt"))),
    "libFLAC": ("libFLAC (Xiph.Org, BSD-3-Clause)",
                lambda: read(src("deps/libFLAC/COPYING.Xiph"))),
    "zstd": ("Zstandard, including xxHash (BSD-3-Clause)",
             lambda: read(src("deps/zstd/LICENSE"))),
    "switchres": ("Switchres (GPL-2.0-or-later)",
                  lambda: licence_header(src("deps/switchres/switchres.cpp"))),
    "yxml": ("yxml (MIT)",
             lambda: licence_header(src("deps/yxml/yxml.c"))),
    "discord-rpc": ("Discord RPC (MIT)",
                    lambda: read(src("deps/discord-rpc/LICENSE"))),
    "xdelta3": ("xdelta3 (Apache-2.0)",
                lambda: read(src("deps/xdelta3/LICENSE"))),
}


# Third-party code compiled from outside deps/: source path prefix ->
# (title, licence text).
OUTSIDE = [
    ("libretro-common/formats/libchdr/",
     ("libchdr, MAME CHD reader by Aaron Giles (BSD-3-Clause)",
      lambda: distinct_headers_in("libretro-common/formats/libchdr/"))),
    ("gfx/common/wayland/",
     ("Wayland protocol code, generated from wayland-protocols (MIT, per protocol)",
      lambda: distinct_headers_in("gfx/common/wayland/"))),
    ("cores/libretro-video-processor/video_processor_v4l2",
     ("Video4Linux2 video processor core by Jared McNeill (BSD-2-Clause)",
      lambda: licence_header(src("cores/libretro-video-processor/video_processor_v4l2.c")))),
    ("libretro-common/encodings/encoding_base64",
     ("Base64 decoder by William Sherif (zlib licence)",
      lambda: licence_header(src("libretro-common/encodings/encoding_base64.c")))),
    ("libretro-common/utils/md5",
     ("MD5 by Alexander Peslyak (public domain)",
      lambda: licence_header(src("libretro-common/utils/md5.c")))),
]

# A header naming one of these is GPL code, RetroArch's own, or the
# libretro-common MIT notice that has its own section below.
OWN = re.compile(r"GNU General Public License|GNU GPL|\bGPL(v[23])?\b|"
                 r"RetroArch team|Cartlink contributors", re.I)
FOREIGN = re.compile(r"copyright|licen[cs]e|public domain", re.I)


def objects():
    """(relative source path without extension, source file or None)."""
    out = []
    for obj in glob.glob(src("obj-*/**/*.o"), recursive=True):
        rel = re.sub(r"^obj-[^/]+/[^/]+/", "", os.path.relpath(obj, SRC))[:-2]
        found = None
        for ext in (".c", ".cpp", ".cc", ".m", ".S", ".s"):
            if os.path.exists(src(rel + ext)):
                found = src(rel + ext)
                break
        out.append((rel, found))
    return sorted(out)


def distinct_headers_in(prefix):
    seen = []
    for rel, f in objects():
        if rel.startswith(prefix) and f:
            h = licence_header(f)
            if h not in seen:
                seen.append(h)
    return "\n\n-----\n\n".join(seen)


def outside_sections():
    """Sections for third-party code outside deps/; fails on unknown notices."""
    used, unknown = [], []
    for rel, f in objects():
        if rel.startswith("deps/"):
            continue
        if f is None:
            # Generated at build time with no source next to it: Qt moc
            # output and the Windows resource file.
            if re.search(r"(^|/)moc_[^/]+$", rel) or rel.startswith("media/"):
                continue
            unknown.append(rel + " (no source file found)")
            continue
        entry = next((e for pre, e in OUTSIDE if rel.startswith(pre)), None)
        if entry:
            if entry not in used:
                used.append(entry)
            continue
        with open(f, encoding="utf-8", errors="replace") as fh:
            head = fh.read(4000)
        if FOREIGN.search(head) and not OWN.search(head):
            unknown.append(rel)
    if unknown:
        sys.exit("compiled files with a third-party notice not covered by OUTSIDE "
                 "(add their licence):\n  " + "\n  ".join(unknown))
    return [(t, fn()) for t, fn in used]


def config_has(name):
    try:
        return re.search(r"^%s = 1$" % name, read(src("config.mk")), re.M) is not None
    except OSError:
        return False


sections = []
built = sorted({re.sub(r".*/deps/([^/]+)/.*", r"\1", o) for o in
                glob.glob(src("obj-*/**/deps/*/**/*.o"), recursive=True)})
if not built:
    sys.exit("no object files under %s/obj-*: build first" % SRC)
unknown = [d for d in built if d not in COMPONENTS]
if unknown:
    sys.exit("compiled but not in COMPONENTS (add its licence): " + ", ".join(unknown))
for dep in built:
    title, text = COMPONENTS[dep]
    sections.append((title, text()))

sections += outside_sections()

if glob.glob(src("obj-*/**/libretro-common/**/*.o"), recursive=True):
    sections.append(("libretro-common (The RetroArch team; MIT, per file)",
                     "Each libretro-common file carries its own MIT notice like this\n"
                     "one (from libretro-common/file/file_path.c):\n\n" +
                     licence_header(src("libretro-common/file/file_path.c"))))

# Single-header libraries, compiled into RetroArch's own files.
if config_has("HAVE_STB_IMAGE") or config_has("HAVE_STB_FONT") or config_has("HAVE_STB_VORBIS"):
    sections.append(("stb_image, stb_truetype, stb_rect_pack, stb_vorbis by Sean Barrett (public domain)",
                     "These files are in the public domain; no notice is required.\n"
                     "From stb_image.h:\n\n" +
                     tail_licence(src("deps/stb/stb_image.h"), "This software is in the public domain")
                     .split("\n\n")[0]))
if config_has("HAVE_DR_MP3") or config_has("HAVE_BUILTINFLAC"):
    sections.append(("dr_mp3, dr_flac by David Reid (public domain / Unlicense, or MIT-0)",
                     tail_licence(src("deps/dr/dr_mp3.h"),
                                  "This is free and unencumbered software")))

if PLATFORM == "windows":
    lic = "/usr/share/licenses/mingw-w64-crt/COPYING.MinGW-w64-runtime.txt"
    pth = "/usr/x86_64-w64-mingw32/include/pthread.h"
    for p in (lic, pth):
        if not os.path.exists(p):
            sys.exit("missing MinGW-w64 licence source: " + p)
    sections.append(("MinGW-w64 runtime (statically linked)", read(lic)))
    sections.append(("winpthreads, MinGW-w64 POSIX threads (MIT; statically linked)",
                     leading_comments(pth)[0]))
    sections.append(("libgcc and libstdc++ (statically linked)",
                     "GNU GPL v3 with the GCC Runtime Library Exception 3.1, which\n"
                     "allows them to be linked into programs under any licence.\n"
                     "https://www.gnu.org/licenses/gcc-exception-3.1.html"))

rule = "=" * 72
head = ("Cartlink: third-party notices\n\n"
        "Cartlink (an unofficial fork of RetroArch) is under the GNU GPL v3\n"
        "or later: see COPYING. This program also contains the following\n"
        "third-party code, under the licences below. All of them can be\n"
        "used in a GPLv3 program. Individual source files carry their own\n"
        "notices; the complete source is listed in SOURCE.txt.\n\n"
        "Contents:\n" + "".join("  - %s\n" % t for t, _ in sections))
body = "".join("\n%s\n%s\n%s\n\n%s\n" % (rule, t, rule, x.strip("\n"))
               for t, x in sections)
with open(OUT, "w", encoding="utf-8") as f:
    f.write(head + body)
print("%s: %d components" % (OUT, len(sections)))
