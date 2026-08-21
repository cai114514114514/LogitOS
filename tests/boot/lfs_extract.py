#!/usr/bin/env python3
"""Copy one file OFF a LogitFS v4 image, so the host can look at what the
machine wrote.

WHY THIS EXISTS. `make test-coredump-os` boots the machine, makes a program
fault, and the kernel writes an ELF64 core dump to /core.1 on the real disk.
The strongest thing that can be done with that file is to hand it to gdb -- and
gdb is on the host, not on the machine. Without this, the on-device half could
only assert that the kernel PRINTED plausible numbers, which is the kernel
agreeing with itself.

Deliberately read-only and deliberately separate from tests/boot/lfs_setexec.py
even though the two share their inode walk. That file MUTATES an image and says
in its own header that it is a workaround which should disappear; merging a
reader into it would tie the reader's lifetime to the workaround's.

Refuses rather than guesses, the same rule lfs_setexec.py states: a bad magic, a
missing path, a directory where a file was asked for, or a size of zero all stop
the program. A quiet zero-byte output is exactly what would let a harness
"verify" a dump that was never written.
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
PPB = BS // 4


def die(msg):
    sys.exit("lfs_extract: " + msg)


def sb(img):
    f = struct.unpack_from("<13I", img, 0)
    if f[0] != MAGIC:
        die("not a LogitFS image (magic %#x)" % f[0])
    return {"inode_start": f[7], "root": f[10]}


def ino_off(g, ino):
    return g["inode_start"] * BS + ino * INODE_SIZE


def blocks_of(img, g, ino):
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
            die("%s: %s is not a directory" % (path, part))
        hit = [c for n, c in readdir(img, g, ino) if n == part]
        if not hit:
            die("%s: no such path on the image" % path)
        ino = hit[0]
    return ino


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--help":
        sys.exit(__doc__)
    if len(sys.argv) == 3 and sys.argv[2] == "--ls":
        with open(sys.argv[1], "rb") as f:
            img = bytearray(f.read())
        g = sb(img)
        for name, cino in readdir(img, g, g["root"]):
            t = itype(img, g, cino)
            _, size = blocks_of(img, g, cino) if t == T_FILE else ([], 0)
            print("%-24s %s %d" % (name, "dir" if t == T_DIR else "file", size))
        return
    if len(sys.argv) != 4:
        sys.exit("usage: lfs_extract.py <image> <path-on-image> <out-file>\n"
                 "       lfs_extract.py <image> --ls")
    img_path, path, out = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(img_path, "rb") as f:
        img = bytearray(f.read())
    g = sb(img)
    ino = resolve(img, g, path)
    if itype(img, g, ino) != T_FILE:
        die("%s is not a regular file" % path)
    blks, size = blocks_of(img, g, ino)
    if size == 0:
        die("%s is zero bytes -- refusing to report success" % path)
    data = b"".join(bytes(img[b * BS:(b + 1) * BS]) for b in blks)[:size]
    with open(out, "wb") as f:
        f.write(data)
    print("lfs_extract: %s -> %s, %d bytes" % (path, out, size))


if __name__ == "__main__":
    main()
