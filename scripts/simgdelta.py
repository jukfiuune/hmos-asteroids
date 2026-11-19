#!/usr/bin/env python3
"""Rebuilds a new Android sparse image from an old one plus the blocks that changed, for slow links.

    simgdelta.py recipe <new.simg>                 (on the machine with the new image) writes the recipe to stdout:
                                                   the new image's headers plus a hash of every raw 4K block
    simgdelta.py rebuild <recipe> <old.simg> <out> (here) writes <out> from the old image's matching blocks and
                                                   prints the byte ranges of <out> still missing, one "off len" a line
    simgdelta.py serve <new.simg>                  (there) reads "off len" lines on stdin, writes those bytes out
    simgdelta.py patch <out>                       (here) reads the served bytes on stdin into the missing ranges

Raw chunk data in a sparse image is filesystem-block aligned, so the same file contents in a rebuilt filesystem
show up as the same blocks even when they moved.
"""
import hashlib
import struct
import sys

RAW, FILL, DONT_CARE, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4


def h(b):
    return hashlib.blake2b(b, digest_size=16).digest()


def chunks(f):
    """Yields (chunk header bytes, type, blocks, body offset, body length) and leaves f after each body."""
    hdr = f.read(28)
    magic, _, _, fhs, chs, bs, _, count, _ = struct.unpack("<IHHHHIIII", hdr)
    assert magic == 0xED26FF3A and bs == 4096
    f.seek(fhs)
    yield hdr + f.read(fhs - 28), None, 0, 0, 0
    for _ in range(count):
        ch = f.read(chs)
        ctype, _, nb, tot = struct.unpack("<HHII", ch[:12])
        yield ch, ctype, nb, f.tell(), tot - chs


def recipe(path):
    out = sys.stdout.buffer
    with open(path, "rb") as f:
        for ch, ctype, nb, off, ln in chunks(f):
            out.write(struct.pack("<I", len(ch)) + ch)
            if ctype == RAW:
                for _ in range(nb):
                    out.write(h(f.read(4096)))
            elif ctype is not None:
                out.write(f.read(ln))


def rebuild(rpath, old, outp):
    index = {}
    with open(old, "rb") as f:
        for ch, ctype, nb, off, ln in chunks(f):
            if ctype == RAW:
                for i in range(nb):
                    index.setdefault(h(f.read(4096)), off + i * 4096)
            elif ctype is not None:
                f.seek(ln, 1)
    print("old image: %d distinct blocks" % len(index), file=sys.stderr)
    missing, have, need = [], 0, 0
    with open(rpath, "rb") as r, open(old, "rb") as o, open(outp, "wb") as out:
        first = True
        while True:
            n = r.read(4)
            if not n:
                break
            ch = r.read(struct.unpack("<I", n)[0])
            out.write(ch)
            if first:
                first = False
                continue
            ctype, _, nb, tot = struct.unpack("<HHII", ch[:12])
            if ctype == RAW:
                for _ in range(nb):
                    src = index.get(r.read(16))
                    if src is None:
                        if missing and missing[-1][0] + missing[-1][1] == out.tell():
                            missing[-1][1] += 4096
                        else:
                            missing.append([out.tell(), 4096])
                        out.write(bytes(4096))
                        need += 1
                    else:
                        o.seek(src)
                        out.write(o.read(4096))
                        have += 1
            else:
                out.write(r.read(tot - len(ch)))
    print("reused %d blocks, %d to fetch (%.1f MiB)" % (have, need, need * 4096 / 2**20), file=sys.stderr)
    for off, ln in missing:
        print(off, ln)


def serve(path):
    out = sys.stdout.buffer
    with open(path, "rb") as f:
        for line in sys.stdin:
            off, ln = map(int, line.split())
            f.seek(off)
            out.write(f.read(ln))


def patch(outp, listp):
    inp = sys.stdin.buffer
    with open(outp, "r+b") as out, open(listp) as l:
        for line in l:
            off, ln = map(int, line.split())
            data = inp.read(ln)
            assert len(data) == ln, "short read at %d" % off
            out.seek(off)
            out.write(data)


if __name__ == "__main__":
    {"recipe": recipe, "rebuild": rebuild, "serve": serve, "patch": patch}[sys.argv[1]](*sys.argv[2:])
