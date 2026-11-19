#!/usr/bin/env python3
"""Makes the donor's bootanimation (7.0.0.107, sha256 60a696e0...) draw picture-mode frames over the whole screen.

    patch-bootanimation-fullpic.py <bootanimation in> <out>

BootPicturePlayer::InitPicCoordinates (OpenHarmony boot_picture_player.cpp) sizes the drawing rect as a
min(width, height) square centred on the screen, built for square OpenHarmony logos; a phone-shaped animation is
squeezed to half its height. Both branches now store realHeight_ = windowHeight_ and realWidth_ = windowWidth_ and
skip the centring, so pointX_ and pointY_ keep their initial 0. Member offsets in this build: windowWidth_ +92,
windowHeight_ +96, realHeight_ +104, realWidth_ +108, pointX_ +112, pointY_ +116 (w9 = width, w0 = height here).
"""
import struct
import sys

TEXT_DELTA = 0x1000  # second LOAD: vaddr 0xe000 at file offset 0xd000
PATCHES = [
    (0x16270, 0x0e040d20, 0x290d2660),  # dup v0.2s, w9            -> stp w0, w9, [x19, #104]
    (0x16274, 0x6b090008, 0x1400000e),  # subs w8, w0, w9          -> b 0x162ac
    (0x16294, 0x290d0260, 0x290d2660),  # stp w0, w0, [x19, #104]  -> stp w0, w9, [x19, #104]
    (0x16298, 0x540008a6, 0x14000005),  # b.vs 0x163ac             -> b 0x162ac
]

data = bytearray(open(sys.argv[1], "rb").read())
for va, old, new in PATCHES:
    current = struct.unpack_from("<I", data, va - TEXT_DELTA)[0]
    if current == new:
        continue
    if current != old:
        sys.exit("%#x: found %#010x, expected %#010x (a different bootanimation build?)" % (va, current, old))
    struct.pack_into("<I", data, va - TEXT_DELTA, new)
open(sys.argv[2], "wb").write(data)
print("%s: picture frames drawn full screen" % sys.argv[2])
