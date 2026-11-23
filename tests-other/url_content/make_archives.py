#!/usr/bin/env python3
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
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


def ne_exe():
    # Win16 "NE" executable: MZ stub, "NE" at e_lfanew (not PE).
    b = bytearray(1024)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 0x80)
    b[0x80:0x82] = b"NE"
    return bytes(b)


def dos_sys():
    # DOS device driver header: next-driver pointer FFFF:FFFF.
    return b"\xff\xff\xff\xff\x00\x80" + bytes(250)

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


def lh(name, data, method=0, csize=None, flags=0, sig=0x04034B50):
    # One local header (+ name) with the sizes as claimed.
    cs = len(data) if csize is None else csize
    return struct.pack("<IHHHHHIIIHH", sig, 20, flags, method, 0, 0,
                       zlib.crc32(data), cs, len(data), len(name), 0) + name


def cdh(name, data, off, method=0, csize=None, usize=None, flags=0):
    # One central directory entry with the sizes as claimed.
    cs = len(data) if csize is None else csize
    us = len(data) if usize is None else usize
    return struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, flags,
                       method, 0, 0, zlib.crc32(data), cs, us, len(name),
                       0, 0, 0, 0, 0, off) + name


def eocd(n, cd_size, cd_off, comment=b"", entries=None):
    e = n if entries is None else entries
    return struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, e, e, cd_size,
                       cd_off, len(comment)) + comment


def one_member_zip(path, name, data, method=0, csize=None, usize=None,
                   flags=0, lsig=0x04034B50, trailer=b"", comment=b"",
                   entries=None):
    # A stored one-member zip built by hand, with optional lies.
    body = lh(name, data, method, csize, flags, lsig) + data
    c = cdh(name, data, 0, method, csize, usize, flags)
    with open(path, "wb") as f:
        f.write(body + c + eocd(1, len(c), len(body), comment, entries)
                + trailer)


def two_eocd_zip(path):
    # Two central directories (review C1): a harmful one whose EOCD
    # comment runs to the end of the file (RetroArch's choice), and a
    # harmless one whose EOCD sits inside that comment (what a
    # "comment fits" scan picks).
    d, n = pe_exe(), b"GAME.EXE"
    g, gn = bytes(4096), b"game.gb"
    blob = lh(n, d) + d
    goff = len(blob)
    blob += lh(gn, g) + g
    evil = cdh(n, d, 0)
    evil_off = len(blob)
    blob += evil
    good = cdh(gn, g, goff)
    good_off = len(blob)
    blob += good
    tail = eocd(1, len(good), good_off) + bytes(8)
    blob += eocd(1, len(evil), evil_off, tail)
    with open(path, "wb") as f:
        f.write(blob)


def short_member_zips(d):
    # Members with fewer bytes in the file than their declared size
    # (re-review I1): RetroArch returns the declared size anyway, the
    # rest being uninitialised memory.
    def rlh(name, method, cs, us):
        return struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, method, 0, 0,
                           0, cs, us, len(name), 0) + name

    def rcd(name, off, method, cs, us):
        return struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0,
                           method, 0, 0, 0, cs, us, len(name), 0, 0, 0, 0,
                           0, off) + name
    us = 0x1000
    for name, fname in ((b"GAME.EXE", "stored_past_eof.zip"),
                        (b"game.bin", "stored_past_eof_bin.zip")):
        # Stored, csize = usize = 0x1000; the local header and "MZ" sit
        # in the EOCD comment, at the very end of the file.
        c = rcd(name, 0, 0, us, us)
        c = rcd(name, len(c) + 22, 0, us, us)
        with open(os.path.join(d, fname), "wb") as f:
            f.write(c + eocd(1, len(c), 0, rlh(name, 0, us, us) + b"MZ"))
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    z = co.compress(b"MZ") + co.flush()
    for name, fname in ((b"GAME.EXE", "deflate_short.zip"),
                        (b"game.bin", "deflate_short_bin.zip")):
        # A deflate stream that gives only "MZ", usize = 0x1000.
        body = rlh(name, 8, len(z), us) + z
        c = rcd(name, 0, 8, len(z), us)
        with open(os.path.join(d, fname), "wb") as f:
            f.write(body + c + eocd(1, len(c), len(body)))
    # A DOS MZ whose PE offset (past 64 KiB, inside the declared size)
    # lies past the end of the deflate stream.
    b = bytearray(70 * 1024)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 100 * 1024)
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    z = co.compress(bytes(b)) + co.flush()
    body = rlh(b"data.bin", 8, len(z), 200 * 1024) + z
    c = rcd(b"data.bin", 0, 8, len(z), 200 * 1024)
    with open(os.path.join(d, "deflate_short_far.zip"), "wb") as f:
        f.write(body + c + eocd(1, len(c), len(body)))


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
    dos_world = [("GAME.EXE", dos_exe()), ("SETUP.PIF", bytes(545)),
                 ("MOUSE.SYS", dos_sys()), ("SOUND.DRV", ne_exe()),
                 ("WIN16.EXE", ne_exe())]
    write_zip(j("dos_world.zip"), dos_world)
    write_zip(j("dos_world_pe.zip"), dos_world + [("WIN.DLL", pe_exe())])
    write_zip(j("vbs_member.zip"), [("game.gb", gb),
                                    ("run.vbs", b'MsgBox "hi"\r\n')])
    # Review C1/C2 and the reader-agreement cases (hand-made headers).
    pe = pe_exe()
    two_eocd_zip(j("two_eocd.zip"))
    one_member_zip(j("csize_lie.zip"), b"GAME.EXE", pe, csize=2)
    one_member_zip(j("csize_honest.zip"), b"GAME.EXE", pe)
    one_member_zip(j("encrypted_flag.zip"), b"game.bin", pe, flags=1)
    one_member_zip(j("no_local_sig.zip"), b"game.bin", pe, lsig=0)
    one_member_zip(j("eocd_not_at_end.zip"), b"game.gb", gb[:512],
                   trailer=b"junk")
    one_member_zip(j("eocd_in_comment.zip"), b"game.gb", gb[:512],
                   comment=b"see PK\x05\x06 here")
    one_member_zip(j("zip64_marker.zip"), b"game.gb", gb[:512],
                   entries=0xFFFF)
    one_member_zip(j("usize_ffff.zip"), b"game.gb", gb[:512],
                   csize=0xFFFFFFFF, usize=0xFFFFFFFF)
    one_member_zip(j("dir_with_pe.zip"), b"x/", pe)
    short_member_zips(d)
    write_zip(j("cmd_member.zip"), [("game.gb", gb), ("run.cmd", b"@echo hi\r\n")])
    write_zip(j("stream_name.zip"), [("run.vbs::$DATA", b'MsgBox "hi"')])
    write_zip(j("colon_name.zip"), [("game:alt.gb", gb)])
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


def run(binary, agree=None):
    # Fixtures in a fresh temp folder, the test binary on it (and the
    # RetroArch-reader agreement test on every archive), and the folder
    # removed again whatever happens.
    with tempfile.TemporaryDirectory(prefix="url_safety_fix.") as d:
        sys.argv = [sys.argv[0], d]
        main()
        rc = subprocess.call([binary, d])
        if agree:
            files = sorted(os.path.join(d, n) for n in os.listdir(d)
                           if n.endswith((".zip", ".7z")))
            rc = subprocess.call([agree] + files) or rc
        return rc


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--run":
        sys.exit(run(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None))
    main()
