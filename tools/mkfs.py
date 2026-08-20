#!/usr/bin/env python3
"""Build an LogitFS v4 disk image: a hierarchical, inode-based filesystem with a
free-block bitmap, subdirectories, and a write-ahead log (Unix-style).

Usage: mkfs.py <out.img> <host[:/dest/path] | host> ...
  host                -> placed at /<basename(host)>
  host:/docs/note.txt -> placed at that absolute path (dirs auto-created)
  host:name.txt       -> placed at /name.txt

Layout (4 KiB blocks = 8 x 512B sectors):
  block 0                      superblock
  block 1..                    block bitmap (1 bit/block)
  block ..                     inode table (inode_count x 128B)
  block log_start..            write-ahead log: 1 header block + log_blocks-1 data
  block data_start..           data blocks (files + dirs + indirect blocks)

Inode (128B): u16 type(0=free,1=file,2=dir); u16 pad; u32 size;
              u32 direct[12]; u32 indirect; u32 double_indirect;
              i64 atime; i64 mtime; i64 ctime; (rest reserved)
Directory data = array of dirents { u32 ino; char name[60] } (64B each).
A directory is just an inode of type=dir whose data holds its dirents.

v4 -> v3: the log area appeared between the inode table and the data blocks, and
the superblock grew two fields (log_start, log_blocks). Every metadata write at
runtime goes through the log first (c/fs/logitfs.c), so a crash mid-operation
replays or discards a whole transaction instead of leaving the bitmap and the
inode table disagreeing.

Timestamps (atime/mtime/ctime, i64 seconds since the Unix epoch) went into the
inode bytes this tool has always zero-filled, so the superblock VERSION is still
4 on purpose: an image written here is read correctly by code that predates
timestamps, and an image written before them reads back as all-zero, which is
"unknown", not corruption. Bumping the version would have broken the root-device
probe in c/drivers/block/blkdev.c for no gain. mkfs stamps every inode with the
build time, so a freshly built image does not look like it was made in 1970.

The C mirror of every constant and offset here is c/fs/logitfs_fmt.h;
tests/unit/fs_format_test.c reads a real image from this tool and asserts the
two agree field by field.
"""
import sys
import os
import struct
import time

SECTOR = 512
BS = 4096                       # block size
SPB = BS // SECTOR              # sectors per block (8)
MAGIC = 0x4C4F4749              # "LOGI"
VERSION = 4
INODE_SIZE = 128
NDIRECT = 12
PPB = BS // 4                   # u32 pointers per (indirect) block (1024)
DIRENT = 64
NAME_MAX = 60                   # bytes in a dirent name (incl. NUL)
T_FREE, T_FILE, T_DIR = 0, 1, 2

# Inode field offsets (bytes from the start of the 128-byte inode).
OFF_TYPE = 0
OFF_SIZE = 4
OFF_DIRECT = 8
OFF_INDIRECT = OFF_DIRECT + NDIRECT * 4          # 56
OFF_DINDIRECT = OFF_INDIRECT + 4                 # 60
OFF_ATIME = OFF_DINDIRECT + 4                    # 64
OFF_MTIME = OFF_ATIME + 8                        # 72
OFF_CTIME = OFF_MTIME + 8                        # 80
# Owner and mode, the next slice of the same reserved area. mkfs leaves all
# three ZERO on purpose: xmode == 0 means "nobody ever set a mode on this file",
# which the kernel reports as the 0644/0755 default WITH the "not stored" bit
# clear, rather than as a chosen mode of 0000. See c/fs/logitfs_fmt.h.
OFF_XMODE = OFF_CTIME + 8                        # 88
OFF_UID = OFF_XMODE + 4                          # 92
OFF_GID = OFF_UID + 4                            # 96
MODE_SET = 0x8000                                # xmode presence bit

# The write-ahead log's commit record (c/fs/logitfs_fmt.h). mkfs only ever
# writes an EMPTY log -- a fresh image has no transaction outstanding -- but the
# constants live here so mkreplay.py and the tests can seal one the way the
# filesystem does.
LOG_MAGIC = 0x4C4F4735                           # "LOG5"
LOGH_MAGIC, LOGH_GEN, LOGH_COUNT, LOGH_BCRC, LOGH_TARGET = 0, 1, 2, 3, 4
LOGH_HCRC = BS // 4 - 1

TOTAL_BLOCKS = 16384            # 64 MiB image
INODE_COUNT = 256
LOG_BLOCKS = 64                 # 1 header + 63 data blocks; a transaction's
                                # metadata is a handful of blocks, never near this


class Builder:
    def __init__(self):
        # inode 0 = root directory
        self.itype = [T_FREE] * INODE_COUNT
        self.content = [b""] * INODE_COUNT       # files: bytes; dirs: filled later
        # 0 = never set, which is what every inode was before programs needed an
        # execute bit; see is_program() above.
        self.xmode = [0] * INODE_COUNT
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

    # WHICH FILES GET AN EXECUTE BIT, and why this exists at all.
    #
    # The comment above OFF_XMODE says mkfs leaves the mode zero on purpose, so
    # the kernel reports the 0644/0755 DEFAULT with "not stored" clear. That was
    # right while nothing enforced the bit. On 2026-08-20 execve started calling
    # vfs_access(path, MAY_EXEC) -- a real and correct enforcement of a
    # permission this filesystem has stored durably since the statmeta work --
    # and every program on the image became unlaunchable in the same instant:
    #
    #     [execve] /bin/sh: permission denied (not executable)
    #     login: could not exec /bin/sh
    #
    # A regular file defaulting to 0644 is correct POSIX. What was missing is
    # that a PROGRAM shipped in the image is not an ordinary regular file, and
    # nothing had ever had to say so, because nothing had ever asked.
    #
    # The rule is by DESTINATION, not by content: a file that lands in a
    # program directory or carries the .aex extension is a program. Sniffing
    # for an ELF magic instead would also mark every test FIXTURE that happens
    # to contain one, and would silently mark nothing on the day a program
    # stops being an ELF.
    EXEC_DIRS = ("/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/")

    @staticmethod
    def is_program(dest):
        d = "/" + dest.strip("/")
        return d.endswith(".aex") or any(d.startswith(x) for x in Builder.EXEC_DIRS)

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
        if self.is_program(dest):
            self.xmode[ino] = MODE_SET | 0o755

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
        log_start = inode_start + inode_blocks
        data_start = log_start + LOG_BLOCKS

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
        struct.pack_into("<13I", img, 0,
                         MAGIC, VERSION, BS, TOTAL_BLOCKS, INODE_COUNT,
                         bitmap_start, bitmap_blocks, inode_start, inode_blocks,
                         data_start, self.root, log_start, LOG_BLOCKS)

        # 5) bitmap: blocks 0..nextb-1 are in use (metadata + allocated data)
        for b in range(nextb):
            img[bitmap_start * BS + (b >> 3)] |= 1 << (b & 7)

        # 6) inode table. Every live inode is stamped with the build time: a
        # zero timestamp means "unknown" to the kernel, and an image that has
        # only ever been read would otherwise report the epoch forever.
        now = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
        for ino in range(INODE_COUNT):
            off = inode_start * BS + ino * INODE_SIZE
            struct.pack_into("<HHI", img, off, self.itype[ino], 0, size[ino])
            for i in range(NDIRECT):
                struct.pack_into("<I", img, off + OFF_DIRECT + i * 4, direct[ino][i])
            struct.pack_into("<I", img, off + OFF_INDIRECT, indirect[ino])
            struct.pack_into("<I", img, off + OFF_DINDIRECT, dindirect[ino])
            if self.itype[ino] != T_FREE:
                struct.pack_into("<qqq", img, off + OFF_ATIME, now, now, now)
                # uid/gid stay 0 (root): this image has one user until a real
                # one exists, and writing a made-up owner would be a fact the
                # kernel would then have to be told to disbelieve.
                if self.xmode[ino]:
                    struct.pack_into("<I", img, off + OFF_XMODE, self.xmode[ino])

        return img, nextb


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: mkfs.py <out.img> [host[:/dest] ...]")
    out = sys.argv[1]
    b = Builder()
    for spec in sys.argv[2:]:
        # Skip a Windows drive-letter prefix ("D:\..." / "D:/...") so the first
        # colon of an absolute host path isn't read as the host:dest separator.
        off = 2 if len(spec) > 2 and spec[1] == ":" and spec[2] in "/\\" else 0
        host, sep, dest = spec[off:].partition(":")
        host = spec[:off] + host
        if not sep or not dest:
            dest = "/" + os.path.basename(host)
        with open(host, "rb") as f:
            b.add_file(dest, f.read())
    img, used = b.serialize()
    with open(out, "wb") as f:
        f.write(img)
    print(f"mkfs: {out} -> LogitFS v4, {TOTAL_BLOCKS} blocks "
          f"({TOTAL_BLOCKS * BS // 1024} KiB), {used} used, {b.next_ino} inode(s)")


if __name__ == "__main__":
    main()
