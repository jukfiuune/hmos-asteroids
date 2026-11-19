#!/usr/bin/env python3
"""Points a donor library's HUKS key calls at libdbkeys.so (sheng's software key store, db_key.c) instead of
libhukssdk.z.so, which needs the TEE-backed HUKS service.

    patch-huks-imports.py <in> <out> <S|D> <name>...

The DT_NEEDED entry libhukssdk.z.so becomes libdbkeys.so (libdbkeys.so itself needs libhukssdk, so the param-set helpers
still resolve there), and each named import HksX becomes SksX or DksX (sheng's database and data-service key spaces).
Only the dynamic string table changes; every string must occur exactly once.
"""
import re
import sys

src, out, prefix, names = sys.argv[1], sys.argv[2], sys.argv[3].encode(), sys.argv[4:]
data = bytearray(open(src, "rb").read())


def replace(old, new):
    hits = [m.start() for m in re.finditer(b"(?<=\\x00)" + re.escape(old) + b"\\x00", data)]
    if len(hits) != 1:
        sys.exit("%s: %r found %d times" % (src, old, len(hits)))
    data[hits[0]:hits[0] + len(new)] = new


replace(b"libhukssdk.z.so", b"libdbkeys.so\0\0\0")
for name in names:
    replace(b"Hks" + name.encode(), prefix + b"ks" + name.encode())
open(out, "wb").write(data)
print("%s: libdbkeys.so, %s" % (out, ", ".join(prefix.decode() + "ks" + n for n in names)))
