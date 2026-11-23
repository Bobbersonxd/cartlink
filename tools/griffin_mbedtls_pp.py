#!/usr/bin/env python3
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# For every bundled mbedTLS source, compares the preprocessed tokens
# that come from the .c file itself when it is compiled alone and when
# it is compiled inside its griffin/griffin_mbedtls_*.c part. A macro
# from an earlier source in the part that changes a later one's code
# shows up as a difference. Used by tools/griffin_mbedtls_check.sh.
# Usage: griffin_mbedtls_pp.py <repo root> <cc> <-std=...>

import glob
import os
import re
import subprocess
import sys

ROOT, CC, STD = sys.argv[1], sys.argv[2], sys.argv[3]
FL = [STD, "-DHAVE_SSL", "-DHAVE_NETWORKING", "-DHAVE_BUILTINMBEDTLS",
      "-I" + ROOT + "/deps/mbedtls", "-I" + ROOT + "/libretro-common/include"]
MARK = re.compile(r'^# (\d+) "([^"]+)"')
# __FILE__ is the path as it was included: compare the file name only.
FILE_STR = re.compile(r'"[^" ]*/deps/mbedtls/([^"/]+\.c)"')


def per_file(path, cwd):
    out = subprocess.check_output([CC, "-E"] + FL + [path], cwd=cwd, text=True)
    cur, acc = None, {}
    for line in out.splitlines():
        m = MARK.match(line)
        if m:
            cur = os.path.normpath(os.path.join(cwd, m.group(2)))
            continue
        if cur and cur.endswith(".c") and "/deps/mbedtls/" in cur:
            acc.setdefault(cur, []).append(line)
    return {k: FILE_STR.sub(r'"\1"', " ".join(" ".join(v).split()))
            for k, v in acc.items()}


parts = {}
for f in sorted(glob.glob(ROOT + "/griffin/griffin_mbedtls_*.c")):
    for k, v in per_file(os.path.basename(f), ROOT + "/griffin").items():
        if k in parts:
            sys.exit("%s is in two parts" % k)
        parts[k] = v
bad = 0
for k in sorted(parts):
    alone = per_file(k, os.path.dirname(k)).get(k, "")
    if alone != parts[k]:
        bad += 1
        a, b = alone.split(), parts[k].split()
        i = 0
        while i < min(len(a), len(b)) and a[i] == b[i]:
            i += 1
        print("DIFF %s at token %d: %s | %s" % (os.path.basename(k), i,
              " ".join(a[max(0, i - 8):i + 8]), " ".join(b[max(0, i - 8):i + 8])))
print("%s %s: %d sources with code, %d differ" % (CC, STD, len(parts), bad))
sys.exit(1 if bad else 0)
