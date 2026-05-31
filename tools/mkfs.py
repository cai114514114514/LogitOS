#!/usr/bin/env python3
"""Build an AquaFS v2 disk image (fixed per-file slots, writable at runtime).

Usage: mkfs.py <out.img> <host:fsname | host> ...

Layout (512-byte sectors):
  sector 0      superblock { <III: magic, version=2, num_files }
  sectors 1..2  directory, 16 x 64-byte entries { name[48], <I start_lba, <I size }
  data          slot i lives at DATA_LBA + i*SLOT_SECTORS (fixed capacity)
"""
import sys
import os
import struct

SECTOR = 512
MAGIC = 0x41515541          # "AQUA"
VERSION = 2
DIR_LBA = 1
DIR_SECTORS = 2
ENTRY = 64
DATA_LBA = 3
MAX_FILES = 16
SLOT_SECTORS = 128          # 64 KiB capacity per file


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: mkfs.py <out.img> [host:fsname ...]")
    out = sys.argv[1]
    specs = sys.argv[2:]
    if len(specs) > MAX_FILES:
        sys.exit(f"mkfs: too many files ({len(specs)} > {MAX_FILES})")

    total_sectors = DATA_LBA + MAX_FILES * SLOT_SECTORS
    img = bytearray(total_sectors * SECTOR)
    struct.pack_into("<III", img, 0, MAGIC, VERSION, len(specs))

    names = []
    for i, spec in enumerate(specs):
        host, _, fsname = spec.partition(":")
        if not fsname:
            fsname = os.path.basename(host)
        with open(host, "rb") as f:
            data = f.read()
        start = DATA_LBA + i * SLOT_SECTORS
        if len(data) > SLOT_SECTORS * SECTOR:
            sys.exit(f"mkfs: {fsname} ({len(data)} B) exceeds slot capacity "
                     f"({SLOT_SECTORS * SECTOR} B)")
        off = DIR_LBA * SECTOR + i * ENTRY
        nb = fsname.encode()[:47]
        img[off:off + len(nb)] = nb
        struct.pack_into("<II", img, off + 48, start, len(data))
        img[start * SECTOR:start * SECTOR + len(data)] = data
        names.append(fsname)

    with open(out, "wb") as f:
        f.write(img)
    print(f"mkfs: {out} -> {total_sectors} sectors, {len(specs)} file(s): "
          + ", ".join(names))


if __name__ == "__main__":
    main()
