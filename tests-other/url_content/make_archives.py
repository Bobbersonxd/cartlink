#!/usr/bin/env python3
# Builds the .zip/.7z fixtures for the url_safety tests into <dir>.
# Only dummy bytes: no real game or program files. Sizes that would be
# too big to write (bombs, > 2 GiB) are claimed in hand-made zip
# headers instead. The .7z files need the 7z/7za tool; without it they
# are left out and the tests skip them.
# Usage: make_archives.py <dir>
#        make_archives.py --run <test binary>  (temp folder, run, remove)

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


def dos_exe():
    # Plain DOS MZ: e_lfanew (0x3C) points past the end, so no PE.
    b = bytearray(512)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 0x00120034)
    return bytes(b)


def dos_exe_far():
    # DOS MZ, 100 KiB, e_lfanew points inside the file but past the
    # first 64 KiB, at bytes that are not "PE\0\0".
    b = bytearray(100 * 1024)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 80 * 1024)
    return bytes(b)


def pe_exe():
    b = bytearray(1024)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 0x80)
    b[0x80:0x84] = b"PE\0\0"
    return bytes(b)


def pe_exe_far():
    # A PE whose header sits past the first 64 KiB.
    b = bytearray(100 * 1024)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 80 * 1024)
    b[80 * 1024:80 * 1024 + 4] = b"PE\0\0"
    return bytes(b)


def gb_rom():
    b = bytearray(32768)
    for i in range(len(b)):
        b[i] = (i * 13 + 5) & 0xFF
    b[0x134:0x13C] = b"TESTGAME"
    return bytes(b)


ELF = b"\x7fELF\x02\x01\x01" + bytes(57)
BAT = b"@echo off\r\nGAME.EXE\r\n"


def write_zip(path, members, compression=zipfile.ZIP_DEFLATED, zip64=False):
    with zipfile.ZipFile(path, "w", compression) as z:
        for name, data in members:
            info = zipfile.ZipInfo(name)
            info.compress_type = compression
            if zip64:
                with z.open(info, "w", force_zip64=True) as f:
                    f.write(data)
            else:
                z.writestr(info, data)


def raw_zip(path, entries):
    # entries: (name, method, claimed_csize, claimed_usize, data)
    # Writes local headers + data + central directory as claimed.
    out = bytearray()
    cd = bytearray()
    for name, method, csize, usize, data in entries:
        n = name.encode()
        off = len(out)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, method, 0, 0,
                           0, csize, usize, len(n), 0) + n + data
        cd += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0,
                          method, 0, 0, 0, csize, usize, len(n), 0, 0, 0,
                          0, 0, off) + n
    cd_off = len(out)
    out += cd
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries),
                       len(entries), len(cd), cd_off, 0)
    with open(path, "wb") as f:
        f.write(out)


def seven(tool, path, files, extra=None):
    # files: (name, data) written into a temp folder, then archived.
    work = tempfile.mkdtemp()
    try:
        for name, data in files:
            p = os.path.join(work, name)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        subprocess.run([tool, "a", "-bd", "-bb0", "-y"] + (extra or [])
                       + [os.path.abspath(path)] + [n for n, _ in files],
                       cwd=work, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    finally:
        shutil.rmtree(work)


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    j = lambda n: os.path.join(d, n)
    gb = gb_rom()

    write_zip(j("ok.zip"), [("game.gb", gb), ("docs/readme.txt", b"hi"),
                            ("docs/", b"")])
    write_zip(j("ok_stored.zip"), [("game.gb", gb)], zipfile.ZIP_STORED)
    write_zip(j("ok_zip64.zip"), [("game.gb", gb)], zip64=True)
    write_zip(j("dosgame.zip"), [("GAME.EXE", dos_exe()), ("RUN.BAT", BAT),
                                 ("SETUP.COM", b"\xb4\x09\xcd\x21"),
                                 ("BIG.EXE", dos_exe_far()),
                                 ("DATA.DAT", gb)])
    write_zip(j("slip_dotdot.zip"), [("../x.gb", gb)])
    write_zip(j("slip_abs.zip"), [("/abs.gb", gb)])
    write_zip(j("slip_drive.zip"), [("C:\\x.gb", gb)])
    write_zip(j("slip_bslash.zip"), [("a\\..\\..\\x.gb", gb)])
    write_zip(j("pe_member.zip"), [("game.gb", gb), ("setup.exe", pe_exe())])
    write_zip(j("pe_named_bin.zip"), [("game.bin", pe_exe())])
    write_zip(j("pe_far.zip"), [("GAME.EXE", pe_exe_far())])
    write_zip(j("pe_stored.zip"), [("game.bin", pe_exe())], zipfile.ZIP_STORED)
    write_zip(j("elf_member.zip"), [("game.gb", ELF)])
    write_zip(j("lnk_member.zip"), [("game.gb", gb), ("Play.lnk", b"x")])
    write_zip(j("app_member.zip"), [("Game.app/Contents/Info.plist", b"x")])
    write_zip(j("script_member.zip"), [("start.txt", b"#!/bin/sh\necho\n")])
    write_zip(j("many_ok.zip"), [("f%05d.txt" % i, b"x")
                                 for i in range(10000)], zipfile.ZIP_STORED)
    write_zip(j("many.zip"), [("f%05d.txt" % i, b"x")
                              for i in range(10001)], zipfile.ZIP_STORED)
    # 200 MB claimed from 100 bytes.
    raw_zip(j("bomb.zip"), [("disc.bin", 8, 100, 200 * 1000 * 1000,
                             b"\xff" * 100)])
    # Small members may compress very well: not a bomb.
    raw_zip(j("tiny_ratio.zip"), [("blank.sav", 8, 10, 512 * 1024,
                                   b"\xff" * 10)])
    # 2 GiB + 1 in total, every member under 1000:1.
    third = (2 * 1024 * 1024 * 1024 + 1) // 3
    sizes = [third, third, 2 * 1024 * 1024 * 1024 + 1 - 2 * third]
    raw_zip(j("toobig.zip"), [("disc%d.bin" % i, 8, 1024 * 1024, s,
                               b"\xff" * (1024 * 1024))
                              for i, s in enumerate(sizes)])
    half = 1024 * 1024 * 1024
    raw_zip(j("exactly2g.zip"), [("disc%d.bin" % i, 8, 1200000, half,
                                  b"\xff" * 1200000)
                                 for i in range(2)])
    with open(j("broken.zip"), "wb") as f:
        f.write(b"PK\x03\x04 this is not really a zip")

    tool = shutil.which("7z") or shutil.which("7za")
    if not tool:
        return
    seven(tool, j("ok.7z"), [("game.gb", gb), ("readme.txt", b"hi")])
    seven(tool, j("dosgame.7z"), [("GAME.EXE", dos_exe()), ("RUN.BAT", BAT),
                                  ("DATA.DAT", gb)])
    seven(tool, j("pe.7z"), [("game.gb", gb), ("setup.exe", pe_exe())])
    seven(tool, j("pe_named_bin.7z"), [("game.bin", pe_exe())])
    seven(tool, j("elf.7z"), [("game.gb", ELF)])
    seven(tool, j("lnk.7z"), [("Play.lnk", b"x")])
    seven(tool, j("bomb.7z"), [("disc.bin", bytes(8 * 1024 * 1024))])
    seven(tool, j("many.7z"), [("f%05d.txt" % i, b"x") for i in range(10001)])
    with open(j("broken.7z"), "wb") as f:
        f.write(b"7z\xbc\xaf\x27\x1c junk")


def run(binary):
    # Fixtures in a fresh temp folder, the test binary on it, and the
    # folder removed again whatever happens.
    with tempfile.TemporaryDirectory(prefix="url_safety_fix.") as d:
        sys.argv = [sys.argv[0], d]
        main()
        return subprocess.call([binary, d])


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--run":
        sys.exit(run(sys.argv[2]))
    main()
