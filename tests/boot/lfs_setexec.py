#!/usr/bin/env python3
"""Stamp the execute bit onto files already on a LogitFS v4 image.

WHY THIS EXISTS, and it is a workaround for something that is not this file's
bug. c/kernel/exec/exec.c now asks vfs_access(path, MAY_EXEC) before execve,
and tools/mkfs.py deliberately leaves every inode's xmode ZERO ("nobody ever
set a mode"), which c/fs/logitfs.c reports as the 0644 default with VA_STORED
clear. c/fs/vfs_meta.c gives root no bypass for MAY_EXEC when no x bit is set
anywhere -- Linux's rule, argued in that file -- so on a stock image EVERY
program refuses to exec, /bin/sh included, and the machine never reaches a
shell. That is an in-flight change's missing half (mkfs must stamp /bin, or the
check must treat an unstored mode as executable); it is not the loader's.

So this is a TEST FIXTURE, not a fix: it edits a COPY of an image so a boot
harness can measure something else entirely. It is deliberately NOT wired into
any build rule -- when the real fix lands this file stops being needed, and a
workaround wired into the disk rule is one nobody would notice had gone stale.

Refuses rather than guesses: a bad magic, a missing path or a name that is not
a directory stops the program. A fixture that silently changed nothing would
produce a boot that fails for the original reason with no sign the fixture ran.
"""
import struct
import sys

BS = 4096
INODE_SIZE = 128
NDIRECT = 12
DIRENT = 64
NAME_MAX = 60
T_FREE, T_FILE, T_DIR = 0, 1, 2
MAGIC = 0x4C4F4749          # tools/mkfs.py
OFF_SIZE = 4
OFF_DIRECT = 8
OFF_INDIRECT = OFF_DIRECT + NDIRECT * 4
OFF_DINDIRECT = OFF_INDIRECT + 4
OFF_XMODE = OFF_DINDIRECT + 4 + 24          # after atime/mtime/ctime
MODE_SET = 0x8000
PPB = BS // 4


def sb(img):
    f = struct.unpack_from("<13I", img, 0)
    if f[0] != MAGIC:
        sys.exit("lfs_setexec: not a LogitFS image (magic %#x)" % f[0])
    return {"inode_start": f[7], "root": f[10]}


def ino_off(g, ino):
    return g["inode_start"] * BS + ino * INODE_SIZE


def blocks_of(img, g, ino):
    """Every data block of `ino`, in order, following both indirect levels."""
    off = ino_off(g, ino)
    size = struct.unpack_from("<I", img, off + OFF_SIZE)[0]
    n = (size + BS - 1) // BS
    out = []
    for i in range(min(n, NDIRECT)):
        out.append(struct.unpack_from("<I", img, off + OFF_DIRECT + i * 4)[0])
    if n > NDIRECT:
        ib = struct.unpack_from("<I", img, off + OFF_INDIRECT)[0]
        for j in range(min(n - NDIRECT, PPB)):
            out.append(struct.unpack_from("<I", img, ib * BS + j * 4)[0])
    if n > NDIRECT + PPB:
        dib = struct.unpack_from("<I", img, off + OFF_DINDIRECT)[0]
        base = NDIRECT + PPB
        nptr = (n - base + PPB - 1) // PPB
        for k in range(nptr):
            sib = struct.unpack_from("<I", img, dib * BS + k * 4)[0]
            for m in range(PPB):
                bi = base + k * PPB + m
                if bi >= n:
                    break
                out.append(struct.unpack_from("<I", img, sib * BS + m * 4)[0])
    return out, size


def readdir(img, g, ino):
    blks, size = blocks_of(img, g, ino)
    data = b"".join(bytes(img[b * BS:(b + 1) * BS]) for b in blks)[:size]
    ents = []
    for o in range(0, len(data) - DIRENT + 1, DIRENT):
        cino = struct.unpack_from("<I", data, o)[0]
        name = data[o + 4:o + 4 + NAME_MAX].split(b"\0")[0].decode("utf-8", "replace")
        if name:
            ents.append((name, cino))
    return ents


def itype(img, g, ino):
    return struct.unpack_from("<H", img, ino_off(g, ino))[0]


def resolve(img, g, path):
    ino = g["root"]
    for part in [p for p in path.split("/") if p]:
        if itype(img, g, ino) != T_DIR:
            sys.exit("lfs_setexec: %s: not a directory" % path)
        hit = [c for n, c in readdir(img, g, ino) if n == part]
        if not hit:
            sys.exit("lfs_setexec: %s: no such path on the image" % path)
        ino = hit[0]
    return ino


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: lfs_setexec.py <image> [dir ...]   (default: /bin)")
    img_path = sys.argv[1]
    dirs = sys.argv[2:] or ["/bin"]
    with open(img_path, "rb") as f:
        img = bytearray(f.read())
    g = sb(img)

    changed = 0
    for d in dirs:
        dino = resolve(img, g, d)
        if itype(img, g, dino) != T_DIR:
            sys.exit("lfs_setexec: %s is not a directory" % d)
        for name, cino in readdir(img, g, dino):
            if name in (".", "..") or itype(img, g, cino) != T_FILE:
                continue
            struct.pack_into("<I", img, ino_off(g, cino) + OFF_XMODE,
                             MODE_SET | 0o755)
            changed += 1
        print("lfs_setexec: %s -> %d regular file(s) now 0755" % (d, changed))

    if not changed:
        sys.exit("lfs_setexec: nothing changed -- refusing to report success")
    with open(img_path, "wb") as f:
        f.write(img)
    print("lfs_setexec: wrote %s" % img_path)


if __name__ == "__main__":
    main()
