#!/usr/bin/env python3
"""Build an AquaFS disk image.

Usage: mkfs.py <out.img> <host:fsname | host> ...

Layout (512-byte sectors):
  sector 0      superblock { <III: magic, version, num_files }
  sectors 1..2  directory, 16 x 64-byte entries { name[48], <I start, <I size }
  sectors 3..   file data, contiguous
"""
import sys
import os
import math
import struct

SECTOR = 512
MAGIC = 0x41515541          # "AQUA"
DIR_LBA = 1
DIR_SECTORS = 2
ENTRY = 64
DATA_LBA = 3
MAX_FILES = 16


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: mkfs.py <out.img> [host:fsname ...]")
    out = sys.argv[1]
    specs = sys.argv[2:]

    entries = []   # (fsname, start_lba, size)
    blobs = []     # (start_lba, data)
    cur = DATA_LBA
    for spec in specs:
        host, _, fsname = spec.partition(":")
        if not fsname:
            fsname = os.path.basename(host)
        with open(host, "rb") as f:
            data = f.read()
        sectors = max(1, math.ceil(len(data) / SECTOR))
        entries.append((fsname, cur, len(data)))
        blobs.append((cur, data))
        cur += sectors

    if len(entries) > MAX_FILES:
        sys.exit(f"mkfs: too many files ({len(entries)} > {MAX_FILES})")

    img = bytearray(cur * SECTOR)
    struct.pack_into("<III", img, 0, MAGIC, 1, len(entries))
    for i, (name, start, size) in enumerate(entries):
        off = DIR_LBA * SECTOR + i * ENTRY
        nb = name.encode()[:47]
        img[off:off + len(nb)] = nb
        struct.pack_into("<II", img, off + 48, start, size)
    for start, data in blobs:
        off = start * SECTOR
        img[off:off + len(data)] = data

    with open(out, "wb") as f:
        f.write(img)
    print(f"mkfs: {out} -> {cur} sectors, {len(entries)} file(s): "
          + ", ".join(n for n, _, _ in entries))


if __name__ == "__main__":
    main()
