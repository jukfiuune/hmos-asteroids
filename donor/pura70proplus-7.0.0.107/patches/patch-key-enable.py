#!/usr/bin/env python3
"""Makes the donor's libkey_enable.z.so (7.0.0.107) treat the code-signing profile-key enrollment as a no-op success,
for a kernel without Huawei's code-sign device.

    patch-key-enable.py <libkey_enable.z.so in> <out>

When installd delivers a HAP's provisioning profile, CodeSignUtils::EnableKeyInProfile calls this library's
EnableKeyInProfileByRust, which opens /dev/code_sign and enrolls the profile's signing cert so the kernel enforces
code integrity for that app. Our kernel has no /dev/code_sign (open fails, errno 2), so the call returns an error and
the whole install fails at DeliverySignProfile (-773). This is the debug-install counterpart of the fs-verity skip in
patch-code-sign.py: the function now returns 0 (CS_SUCCESS) immediately. No code signature is enrolled or enforced,
which matches the rest of this port; release apps already pass through the (also unenforced) fs-verity path.

Patched at the function entry (stp x29, x30, [sp, #-96]! -> mov w0, #0 ; ret): nothing is pushed before the ret, so
the stack stays balanced and 0 is returned (its error paths return non-zero: 1, 3, 23).
"""
import struct
import sys

TEXT_DELTA = 0x1000  # R E LOAD: vaddr 0x11724 at file offset 0x10724
VA = 0x12c1c         # EnableKeyInProfileByRust
PATCH = [(VA, 0xa9ba7bfd, 0x52800000, "mov w0, #0"),       # was: stp x29, x30, [sp, #-96]!
         (VA + 4, 0xa9016ffc, 0xd65f03c0, "ret")]          # was: stp x28, x27, [sp, #16]

data = bytearray(open(sys.argv[1], "rb").read())
for va, old, new, what in PATCH:
    off = va - TEXT_DELTA
    current = struct.unpack_from("<I", data, off)[0]
    if current != new:
        if current != old:
            sys.exit("%#x: found %#010x, expected %#010x (a different libkey_enable build?)" % (va, current, old))
        struct.pack_into("<I", data, off, new)
    print("%s %#x: %s" % (sys.argv[2], va, what))
open(sys.argv[2], "wb").write(data)
