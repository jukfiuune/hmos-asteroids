#!/usr/bin/env python3
"""Makes the donor's libcode_sign_utils.z.so (7.0.0.107, sha256 2dd39556...) treat every file as already carrying
fs-verity, as sheng's port does for its donor.

    patch-code-sign.py <libcode_sign_utils.z.so in> <out>

installd enables fs-verity with Huawei's code-signing arguments when it installs a HAP; a stock kernel rejects that
ioctl (ENOTTY), the install fails with CS_ERR_ENABLE (-768), and SceneBoard and most system apps never install. The
check before it (FS_IOC_GETFLAGS, then FS_VERITY_FL) returns CS_ERR_FSVERITY_NOT_ENABLED (-770) when the flag is
missing; it now returns 0, so the enable step is skipped. No code signature is enforced.
"""
import struct
import sys

TEXT_DELTA = 0x1000  # second LOAD: vaddr 0xb24c at file offset 0xa24c
VA, OLD, NEW = 0x11f60, 0x12806028, 0x52800008  # mov w8, #-770 -> mov w8, #0

data = bytearray(open(sys.argv[1], "rb").read())
current = struct.unpack_from("<I", data, VA - TEXT_DELTA)[0]
if current != NEW:
    if current != OLD:
        sys.exit("%#x: found %#010x, expected %#010x (a different libcode_sign_utils build?)" % (VA, current, OLD))
    struct.pack_into("<I", data, VA - TEXT_DELTA, NEW)
open(sys.argv[2], "wb").write(data)
print("%s: fs-verity check always passes" % sys.argv[2])
