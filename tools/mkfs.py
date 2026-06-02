#!/usr/bin/env python3
"""Build an AquaFS v3 disk image: a hierarchical, inode-based filesystem with a
free-block bitmap and subdirectories (Unix-style).

Usage: mkfs.py <out.img> <host[:/dest/path] | host> ...
  host                -> placed at /<basename(host)>
  host:/docs/note.txt -> placed at that absolute path (dirs auto-created)
  host:name.txt       -> placed at /name.txt

Layout (4 KiB blocks = 8 x 512B sectors):
  block 0                      superblock
  block 1..                    block bitmap (1 bit/block)
  block ..                     inode table (inode_count x 128B)
  block data_start..           data blocks (files + dirs + indirect blocks)

Inode (128B): u16 type(0=free,1=file,2=dir); u16 pad; u32 size;
              u32 direct[12]; u32 indirect; (rest reserved)
Directory data = array of dirents { u32 ino; char name[60] } (64B each).
A directory is just an inode of type=dir whose data holds its dirents.
"""
import sys
import os
import struct

SECTOR = 512
BS = 4096                       # block size
SPB = BS // SECTOR              # sectors per block (8)
MAGIC = 0x41515541              # "AQUA"
VERSION = 3
INODE_SIZE = 128
NDIRECT = 12
PPB = BS // 4                   # u32 pointers per (indirect) block (1024)
DIRENT = 64
NAME_MAX = 60                   # bytes in a dirent name (incl. NUL)
T_FREE, T_FILE, T_DIR = 0, 1, 2

TOTAL_BLOCKS = 4096             # 16 MiB image
INODE_COUNT = 256


class Builder:
    def __init__(self):
        # inode 0 = root directory
        self.itype = [T_FREE] * INODE_COUNT
        self.content = [b""] * INODE_COUNT       # files: bytes; dirs: filled later
        self.children = {}                        # ino -> list[(name, child_ino)]
        self.next_ino = 0
        self.root = self.alloc_inode(T_DIR)       # ino 0

    def alloc_inode(self, t):
        ino = self.next_ino
        if ino >= INODE_COUNT:
            sys.exit(f"mkfs: out of inodes (> {INODE_COUNT})")
        self.next_ino += 1
        self.itype[ino] = t
        if t == T_DIR:
            self.children[ino] = []
        return ino

    def lookup(self, dino, name):
        for n, c in self.children[dino]:
            if n == name:
                return c
        return None

    def get_or_make_dir(self, parts):
        cur = self.root
        for p in parts:
            if not p:
                continue
            c = self.lookup(cur, p)
            if c is None:
                c = self.alloc_inode(T_DIR)
                self.children[cur].append((p, c))
            elif self.itype[c] != T_DIR:
                sys.exit(f"mkfs: {p} exists and is not a directory")
            cur = c
        return cur

    def add_file(self, dest, data):
        parts = [p for p in dest.split("/") if p]
        if not parts:
            sys.exit("mkfs: empty destination path")
        parent = self.get_or_make_dir(parts[:-1])
        leaf = parts[-1]
        if self.lookup(parent, leaf) is not None:
            sys.exit(f"mkfs: duplicate path {dest}")
        if len(leaf.encode()) >= NAME_MAX:
            sys.exit(f"mkfs: name too long: {leaf}")
        ino = self.alloc_inode(T_FILE)
        self.content[ino] = data
        self.children[parent].append((leaf, ino))

    def serialize(self):
        # 1) materialize directory contents now that all inos are assigned
        for ino in range(self.next_ino):
            if self.itype[ino] == T_DIR:
                buf = bytearray()
                for name, cino in self.children[ino]:
                    e = bytearray(DIRENT)
                    struct.pack_into("<I", e, 0, cino)
                    nb = name.encode()[:NAME_MAX - 1]
                    e[4:4 + len(nb)] = nb
                    buf += e
                self.content[ino] = bytes(buf)

        # 2) geometry
        bitmap_start = 1
        bitmap_blocks = (TOTAL_BLOCKS + 8 * BS - 1) // (8 * BS)
        inode_start = bitmap_start + bitmap_blocks
        inode_blocks = (INODE_COUNT * INODE_SIZE + BS - 1) // BS
        data_start = inode_start + inode_blocks

        img = bytearray(TOTAL_BLOCKS * BS)
        nextb = data_start

        def alloc_block():
            nonlocal nextb
            if nextb >= TOTAL_BLOCKS:
                sys.exit("mkfs: out of data blocks (image too small)")
            b = nextb
            nextb += 1
            return b

        # 3) lay out every inode's content into data blocks (+ single/double indirect)
        direct = [[0] * NDIRECT for _ in range(INODE_COUNT)]
        indirect = [0] * INODE_COUNT
        dindirect = [0] * INODE_COUNT
        size = [0] * INODE_COUNT
        for ino in range(self.next_ino):
            if self.itype[ino] == T_FREE:
                continue
            data = self.content[ino]
            size[ino] = len(data)
            nblk = (len(data) + BS - 1) // BS
            blks = [alloc_block() for _ in range(nblk)]
            for i, b in enumerate(blks):
                chunk = data[i * BS:(i + 1) * BS]
                img[b * BS:b * BS + len(chunk)] = chunk
            for i in range(min(nblk, NDIRECT)):
                direct[ino][i] = blks[i]
            if nblk > NDIRECT:
                ib = alloc_block(); indirect[ino] = ib          # single indirect
                n_si = min(nblk - NDIRECT, PPB)
                for j in range(n_si):
                    struct.pack_into("<I", img, ib * BS + j * 4, blks[NDIRECT + j])
            if nblk > NDIRECT + PPB:                            # double indirect
                if nblk - NDIRECT - PPB > PPB * PPB:
                    sys.exit(f"mkfs: inode {ino} too large for double indirect")
                dib = alloc_block(); dindirect[ino] = dib
                base = NDIRECT + PPB
                nptr = (nblk - base + PPB - 1) // PPB
                for k in range(nptr):
                    sib = alloc_block()
                    struct.pack_into("<I", img, dib * BS + k * 4, sib)
                    for m in range(PPB):
                        bi = base + k * PPB + m
                        if bi >= nblk:
                            break
                        struct.pack_into("<I", img, sib * BS + m * 4, blks[bi])

        # 4) superblock
        struct.pack_into("<11I", img, 0,
                         MAGIC, VERSION, BS, TOTAL_BLOCKS, INODE_COUNT,
                         bitmap_start, bitmap_blocks, inode_start, inode_blocks,
                         data_start, self.root)

        # 5) bitmap: blocks 0..nextb-1 are in use (metadata + allocated data)
        for b in range(nextb):
            img[bitmap_start * BS + (b >> 3)] |= 1 << (b & 7)

        # 6) inode table
        for ino in range(INODE_COUNT):
            off = inode_start * BS + ino * INODE_SIZE
            struct.pack_into("<HHI", img, off, self.itype[ino], 0, size[ino])
            for i in range(NDIRECT):
                struct.pack_into("<I", img, off + 8 + i * 4, direct[ino][i])
            struct.pack_into("<I", img, off + 8 + NDIRECT * 4, indirect[ino])
            struct.pack_into("<I", img, off + 8 + NDIRECT * 4 + 4, dindirect[ino])  # double indirect

        return img, nextb


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: mkfs.py <out.img> [host[:/dest] ...]")
    out = sys.argv[1]
    b = Builder()
    for spec in sys.argv[2:]:
        host, sep, dest = spec.partition(":")
        if not sep or not dest:
            dest = "/" + os.path.basename(host)
        with open(host, "rb") as f:
            b.add_file(dest, f.read())
    img, used = b.serialize()
    with open(out, "wb") as f:
        f.write(img)
    print(f"mkfs: {out} -> AquaFS v3, {TOTAL_BLOCKS} blocks "
          f"({TOTAL_BLOCKS * BS // 1024} KiB), {used} used, {b.next_ino} inode(s)")


if __name__ == "__main__":
    main()
