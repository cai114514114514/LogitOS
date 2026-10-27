#!/usr/bin/env python3
"""List an LogitFS image as `path size sha256`, sorted by path.

Usage: python3 -I tools/ui_imgls.py <image> [--times]

mkfs stamps every inode with the build time (unless SOURCE_DATE_EPOCH is set),
so two builds of the same tree differ as raw images. This listing ignores the
timestamps (and the allocation order), so equal listings mean equal file
content, names and sizes. The on-disk format constants come from tools/mkfs.py
itself (loaded by path, so nothing is duplicated here).
"""
import hashlib
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("ui_imgls_mkfs", os.path.join(HERE, "mkfs.py"))
mkfs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mkfs)


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        sb = struct.unpack("<13I", self.read(0, 13 * 4))
        (magic, version, bs, _total, _icount, _bm, _bmb,
         self.inode_start, _inb, _data, self.root, _ls, _lb) = sb
        if magic != mkfs.MAGIC or version != mkfs.VERSION or bs != mkfs.BS:
            sys.exit("ui_imgls: not an LogitFS v%d image: %s" % (mkfs.VERSION, path))

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n).ljust(n, b"\0")  # holes past EOF read as zero

    def block(self, b):
        return self.read(b * mkfs.BS, mkfs.BS)

    def inode(self, ino):
        off = self.inode_start * mkfs.BS + ino * mkfs.INODE_SIZE
        raw = self.read(off, mkfs.INODE_SIZE)
        typ, _pad, size = struct.unpack_from("<HHI", raw, 0)
        direct = struct.unpack_from("<%dI" % mkfs.NDIRECT, raw, mkfs.OFF_DIRECT)
        ind, dind = struct.unpack_from("<II", raw, mkfs.OFF_INDIRECT)
        return typ, size, direct, ind, dind

    def data(self, ino):
        typ, size, direct, ind, dind = self.inode(ino)
        nblk = (size + mkfs.BS - 1) // mkfs.BS
        blks = list(direct[:min(nblk, mkfs.NDIRECT)])
        if nblk > mkfs.NDIRECT:
            tab = struct.unpack("<%dI" % mkfs.PPB, self.block(ind))
            blks += tab[:min(nblk - mkfs.NDIRECT, mkfs.PPB)]
        if nblk > mkfs.NDIRECT + mkfs.PPB:
            rest = nblk - mkfs.NDIRECT - mkfs.PPB
            for sib in struct.unpack("<%dI" % mkfs.PPB, self.block(dind)):
                if rest <= 0:
                    break
                tab = struct.unpack("<%dI" % mkfs.PPB, self.block(sib))
                blks += tab[:min(rest, mkfs.PPB)]
                rest -= mkfs.PPB
        return typ, b"".join(self.block(b) for b in blks)[:size]


def walk(img, ino, path, out):
    typ, data = img.data(ino)
    if typ == mkfs.T_FILE:
        out.append((path, len(data), hashlib.sha256(data).hexdigest()))
        return
    out.append((path.rstrip("/") + "/", 0, "-"))
    for i in range(0, len(data), mkfs.DIRENT):
        cino = struct.unpack_from("<I", data, i)[0]
        name = data[i + 4:i + mkfs.DIRENT].split(b"\0", 1)[0].decode("utf-8", "replace")
        if cino == 0 or name in (".", ".."):
            continue
        walk(img, cino, path.rstrip("/") + "/" + name, out)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: ui_imgls.py <image>")
    out = []
    img = Image(sys.argv[1])
    walk(img, img.root, "/", out)
    for path, size, sha in sorted(out):
        print("%s\t%d\t%s" % (path, size, sha))


if __name__ == "__main__":
    main()
