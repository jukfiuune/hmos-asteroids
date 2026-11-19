#!/usr/bin/env python3
"""Packs boot animation frames into a bootpic.zip that Huawei's bootanimation can read.

    make-bootpic.py <frames dir> <out.zip>      (frames: config.json plus bootpic_NNN.png)

Stock picture mode draws every frame into a min(width, height) square centred on the screen;
services/patch-bootanimation-fullpic.py makes it draw full screen, so frames are panel-sized (1080x2392).

Huawei's ReadImageFile (OpenHarmony util.cpp, Huawei build) copies every unzReadCurrentFile chunk with
memcpy_s(buf + total, size - total, chunk, len), including the final zero-length read at end of file, and treats a
failure as fatal. Its memcpy_s rejects a destination size of 0, which is what remains once a file has been read in
full, so every entry fails ("memcpy_s failed", pic num 0). Each entry here is deflated and declares an uncompressed
size one byte larger than the data: inflate stops at the real end of stream, the final copy sees 1 byte of room and
succeeds, and minizip only checks the CRC when the declared size has been consumed, so it skips the check.
"""
import os
import struct
import sys
import zipfile

frames, out = sys.argv[1], sys.argv[2]
names = ["config.json"] + sorted(n for n in os.listdir(frames) if n.endswith(".png"))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for n in names:
        z.write(os.path.join(frames, n), n)

data = bytearray(open(out, "rb").read())
patched = 0
with zipfile.ZipFile(out) as z:
    for info in z.infolist():
        # Local header: uncompressed size at +22. (zipfile writes no data descriptors for regular files.)
        lh = info.header_offset
        assert data[lh:lh + 4] == b"PK\x03\x04"
        struct.pack_into("<I", data, lh + 22, info.file_size + 1)
        patched += 1
# Central directory entries: uncompressed size at +24.
cd = data.find(b"PK\x01\x02")
while cd != -1:
    size = struct.unpack_from("<I", data, cd + 24)[0]
    struct.pack_into("<I", data, cd + 24, size + 1)
    name_len, extra_len, comment_len = struct.unpack_from("<HHH", data, cd + 28)
    cd = data.find(b"PK\x01\x02", cd + 46 + name_len + extra_len + comment_len)
open(out, "wb").write(data)
print("%s: %d entries, sizes declared +1" % (out, patched))
