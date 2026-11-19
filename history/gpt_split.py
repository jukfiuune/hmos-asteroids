#!/usr/bin/env python3
"""Splits the tail off a GPT partition into a new partition, for the asteroids userdata/hmosdata split.

Works on dumps instead of a device: the primary table (LBA 0 up to the first usable LBA) and the backup table (the
entry array plus the backup header, ending at the last LBA), with the LUN's logical block size (4096 on this UFS).

    gpt_split.py --block 4096 --primary p.bin --backup b.bin --shrink userdata --new hmosdata --size-mib 65536 \
        --out-primary p.new --out-backup b.new [--dry-run]

The shrunk partition keeps its start; the new one takes the freed tail, starting on a 1 MiB boundary and ending
where the shrunk one ended. Nothing else changes: every other entry, the disk GUID and the usable range are kept,
and the CRCs of both headers and both entry arrays are recomputed. The script refuses tables that are not valid
GPT, that disagree with each other, or where the new layout would overlap anything.
"""
import argparse
import struct
import sys
import uuid
import zlib

HDR = struct.Struct("<8sIIIIQQQQ16sQIII")
LINUX_DATA = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")


def die(msg):
    sys.exit("gpt_split: " + msg)


def parse_header(raw):
    f = HDR.unpack_from(raw)
    h = dict(zip(("sig", "rev", "size", "crc", "res", "my", "alt", "first", "last", "guid", "entries_lba",
                  "count", "esize", "ecrc"), f))
    if h["sig"] != b"EFI PART":
        die("no GPT signature")
    check = bytearray(raw[:h["size"]])
    check[16:20] = b"\0\0\0\0"
    if zlib.crc32(check) != h["crc"]:
        die("header CRC mismatch")
    return h


def pack_header(h):
    raw = bytearray(HDR.pack(h["sig"], h["rev"], h["size"], 0, 0, h["my"], h["alt"], h["first"], h["last"],
                             h["guid"], h["entries_lba"], h["count"], h["esize"], h["ecrc"]))
    raw[16:20] = struct.pack("<I", zlib.crc32(raw[:h["size"]]))
    return bytes(raw)


def entry_name(e):
    return e[56:128].decode("utf-16-le").split("\0")[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--block", type=int, required=True)
    ap.add_argument("--primary", required=True)
    ap.add_argument("--backup", required=True)
    ap.add_argument("--shrink", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--size-mib", type=int, required=True)
    ap.add_argument("--out-primary")
    ap.add_argument("--out-backup")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    bs = a.block

    prim = bytearray(open(a.primary, "rb").read())
    back = bytearray(open(a.backup, "rb").read())
    ph = parse_header(prim[bs:2 * bs])
    if ph["my"] != 1:
        die("primary header is not at LBA 1")
    esize, count = ph["esize"], ph["count"]
    elen = esize * count
    eblocks = -(-elen // bs)
    pe_off = ph["entries_lba"] * bs
    entries = bytearray(prim[pe_off:pe_off + elen])
    if len(entries) != elen or zlib.crc32(entries) != ph["ecrc"]:
        die("primary entry array missing or CRC mismatch")

    # The backup dump runs from the backup entry array to the backup header at the last LBA. The array need not sit
    # right before the header: Qualcomm reserves 16 KiB for it even when the entries take less.
    if len(back) % bs or len(back) < (eblocks + 1) * bs:
        die("backup dump must be whole blocks, entries through header")
    bh = parse_header(back[-bs:])
    if bh["my"] != ph["alt"] or bh["alt"] != 1:
        die("backup header does not match the primary")
    if bh["entries_lba"] != bh["my"] - (len(back) // bs - 1):
        die("backup dump must start at the backup entry array (LBA %d), %d blocks"
            % (bh["entries_lba"], bh["my"] - bh["entries_lba"] + 1))
    if bytes(back[:elen]) != bytes(entries):
        die("backup entry array differs from the primary")

    # Locate the partition to shrink and a free slot; refuse if the new name already exists.
    target, free = None, None
    for i in range(count):
        e = entries[i * esize:(i + 1) * esize]
        if e[:16] == b"\0" * 16:
            free = i if free is None else free
            continue
        n = entry_name(e)
        if n == a.new:
            die("a partition named %s already exists" % a.new)
        if n == a.shrink:
            target = i
    if target is None:
        die("no partition named " + a.shrink)
    if free is None:
        die("no free entry slot")

    e = entries[target * esize:(target + 1) * esize]
    start, end = struct.unpack_from("<QQ", e, 32)
    align = (1 << 20) // bs
    new_blocks = a.size_mib * (1 << 20) // bs
    new_start = (end + 1 - new_blocks) // align * align
    if new_start <= start + align:
        die("%s is too small to give up %d MiB" % (a.shrink, a.size_mib))
    if end > ph["last"]:
        die("%s ends past the last usable LBA" % a.shrink)

    # Overlap check against every other partition.
    for i in range(count):
        o = entries[i * esize:(i + 1) * esize]
        if i == target or o[:16] == b"\0" * 16:
            continue
        s, t = struct.unpack_from("<QQ", o, 32)
        if not (t < new_start or s > end):
            die("new range overlaps %s" % entry_name(o))

    struct.pack_into("<Q", entries, target * esize + 40, new_start - 1)
    ne = bytearray(esize)
    ne[0:16] = LINUX_DATA.bytes_le
    ne[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<QQQ", ne, 32, new_start, end, 0)
    ne[56:56 + 2 * len(a.new)] = a.new.encode("utf-16-le")
    entries[free * esize:(free + 1) * esize] = ne

    mib = lambda b: b * bs / (1 << 20)
    print("%s: %d..%d (%.0f MiB) -> %d..%d (%.0f MiB)" % (a.shrink, start, end, mib(end - start + 1), start,
                                                          new_start - 1, mib(new_start - start)))
    print("%s: %d..%d (%.0f MiB), entry slot %d" % (a.new, new_start, end, mib(end - new_start + 1), free))
    if a.dry_run:
        return

    ecrc = zlib.crc32(entries)
    ph["ecrc"] = bh["ecrc"] = ecrc
    prim[pe_off:pe_off + elen] = entries
    prim[bs:bs + ph["size"]] = pack_header(ph)
    back[:elen] = entries
    back[-bs:-bs + bh["size"]] = pack_header(bh)
    open(a.out_primary, "wb").write(prim)
    open(a.out_backup, "wb").write(back)
    print("wrote %s and %s" % (a.out_primary, a.out_backup))


if __name__ == "__main__":
    main()
