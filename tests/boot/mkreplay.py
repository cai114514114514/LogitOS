#!/usr/bin/env python3
"""Craft (and later verify) a LogitFS v4 image carrying a sealed-but-not-installed
log transaction -- the on-disk state a crash between log seal and install leaves
behind. Mounting the crafted image must replay the transaction (install the
logged block at its target, then clear the header); that is the log_recover path
in c/fs/logitfs.c, which random kill -9 timing almost never hits (the seal->clear
window is a handful of block writes, milliseconds wide).

  mkreplay.py craft <in.img> <out.img> <path-in-fs> <sentinel>
      Copy in.img to out.img with a fake sealed transaction: log header
      {LOGMAGIC, gen=77, count=1, target=<first data block of path>} and one
      body block whose first bytes are the sentinel. The target block itself is
      left untouched -- recovery must be what writes it.

  mkreplay.py check <img> <path-in-fs> <sentinel>
      After a boot on the crafted image: assert the file's first data block now
      holds the body (recovery installed it) and the log header is cleared
      (recovery finished). Exits nonzero with a message on any mismatch.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import mkfs  # layout constants (BS, LOG_MAGIC is local here, structs identical)

BS = mkfs.BS
INODE_SIZE = mkfs.INODE_SIZE
NDIRECT = mkfs.NDIRECT
PPB = mkfs.PPB
DIRENT = mkfs.DIRENT
LOGMAGIC = 0x4C4F4734  # "LOG4" -- c/fs/logitfs.c
GEN = 77

SB = "<13I"


def load(img_path):
    img = bytearray(open(img_path, "rb").read())
    (magic, version, bs, total, icount,
     bitmap_start, bitmap_blocks, inode_start, inode_blocks,
     data_start, root, log_start, log_blocks) = struct.unpack_from(SB, img, 0)
    if magic != mkfs.MAGIC or version != mkfs.VERSION:
        sys.exit("mkreplay: not a LogitFS v%d image" % mkfs.VERSION)
    if bs != BS:
        sys.exit("mkreplay: unexpected block size")
    return img, dict(total=total, icount=icount, inode_start=inode_start,
                     data_start=data_start, log_start=log_start,
                     log_blocks=log_blocks)


def read_inode(img, geo, ino):
    off = geo["inode_start"] * BS + ino * INODE_SIZE
    itype, _pad, size = struct.unpack_from("<HHI", img, off)
    direct = list(struct.unpack_from("<%dI" % NDIRECT, img, off + 8))
    indirect, = struct.unpack_from("<I", img, off + 8 + NDIRECT * 4)
    return itype, size, direct, indirect


def file_blocks(img, geo, ino):
    """Block numbers holding the inode's data (direct + single indirect; the
    files used here are far below the double-indirect threshold)."""
    _t, size, direct, indirect = read_inode(img, geo, ino)
    nblk = (size + BS - 1) // BS
    blks = [b for b in direct if b]
    if nblk > len(blks) and indirect:
        n_si = min(nblk - NDIRECT, PPB)
        blks += list(struct.unpack_from("<%dI" % n_si, img, indirect * BS))
    return size, blks[:nblk]


def lookup(img, geo, path):
    ino = 0  # root
    parts = [p for p in path.split("/") if p]
    for depth, want in enumerate(parts):
        itype, size, direct, indirect = read_inode(img, geo, ino)
        if itype != mkfs.T_DIR:
            sys.exit("mkreplay: %s: component %d is not a directory" % (path, depth))
        _sz, blks = file_blocks(img, geo, ino)
        found = None
        for b in blks:
            for off in range(0, BS, DIRENT):
                cino, = struct.unpack_from("<I", img, b * BS + off)
                name = bytes(img[b * BS + off + 4:b * BS + off + 64]).split(b"\0")[0]
                if cino and name == want.encode():
                    found = cino
                    break
            if found is not None:
                break
        if found is None:
            sys.exit("mkreplay: %s: not found" % path)
        ino = found
    return ino


def body_block(sentinel):
    s = sentinel.encode()
    if len(s) >= BS:
        sys.exit("mkreplay: sentinel too long")
    return s + b"R" * (BS - len(s))


def craft(img_in, img_out, path, sentinel):
    img, geo = load(img_in)
    ino = lookup(img, geo, path)
    size, blks = file_blocks(img, geo, ino)
    if not blks:
        sys.exit("mkreplay: %s: empty file, nothing to target" % path)
    if len(sentinel) > size:
        sys.exit("mkreplay: sentinel longer than the file (%d bytes)" % size)
    target = blks[0]
    body = body_block(sentinel)
    # Seal: header with one valid target; body in the first log data slot.
    hdr = bytearray(BS)
    struct.pack_into("<4I", hdr, 0, LOGMAGIC, GEN, 1, target)
    img[geo["log_start"] * BS:(geo["log_start"] + 1) * BS] = hdr
    img[(geo["log_start"] + 1) * BS:(geo["log_start"] + 2) * BS] = body
    open(img_out, "wb").write(bytes(img))
    print("mkreplay: sealed a fake transaction targeting %s (ino %d, block %d, "
          "%d bytes) at log blocks %d..%d"
          % (path, ino, target, size, geo["log_start"], geo["log_start"] + 1))


def check(img_path, path, sentinel):
    img, geo = load(img_path)
    magic, = struct.unpack_from("<I", img, geo["log_start"] * BS)
    if magic == LOGMAGIC:
        sys.exit("mkreplay: FAIL: log header still sealed -- recovery never ran "
                 "(or never cleared it)")
    ino = lookup(img, geo, path)
    size, blks = file_blocks(img, geo, ino)
    got = bytes(img[blks[0] * BS:blks[0] * BS + BS])
    want = body_block(sentinel)
    if got != want:
        sys.exit("mkreplay: FAIL: %s block %d does not hold the logged body -- "
                 "the transaction was not installed" % (path, blks[0]))
    print("mkreplay: OK: %s block %d holds the replayed body, log header cleared"
          % (path, blks[0]))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    op = sys.argv[1]
    if op == "craft" and len(sys.argv) == 6:
        craft(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    elif op == "check" and len(sys.argv) == 5:
        check(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
