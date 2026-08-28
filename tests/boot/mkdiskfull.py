#!/usr/bin/env python3
"""Craft a LogitFS v4 image whose free-block BITMAP has been patched to leave
only a handful of free blocks -- what test-closefull (run-closefull-test.sh)
needs to prove close() reports a write that never reached the disk (CLAUDE.md
structural gap #3: "the only symptom of a failed write is that the file
quietly is not there").

Filling a real 512 MiB image by actually writing ~510 MB through it is not
the same experiment and is not what this does: this tool edits the persisted
free-block BITMAP directly -- the same bytes tools/mkfs.py writes and
balloc()/bit_test() (c/fs/logitfs.c) read at runtime -- and nothing else. No
inode, no directory entry, no file's content byte moves, so the image still
mounts, still fscks clean, and every file already on it still reads back
exactly as before. Only the accounting of which blocks are free changes.

  mkdiskfull.py <in.img> <out.img> <keep_free_blocks>
      Copy in.img to out.img with every CURRENTLY FREE data block marked
      used, except the <keep_free_blocks> highest-numbered free ones (left
      exactly as mkfs.py wrote them). A write that needs more blocks than
      that must fail with VFS_ENOSPC once it reaches balloc().

The bit convention (bitmap[b>>3] & (1<<(b&7)), absolute block number b) and
every field this reads come from tools/mkfs.py itself, not a second copy of
its numbers -- see mkreplay.py's file comment for why that matters here.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import mkfs  # noqa: E402  (sys.path munge must run first) -- BS/MAGIC/VERSION

SB = "<13I"


def load(img_path):
    img = bytearray(open(img_path, "rb").read())
    (magic, version, bs, total, _icount,
     bitmap_start, _bitmap_blocks, _inode_start, _inode_blocks,
     data_start, _root, log_start, _log_blocks) = struct.unpack_from(SB, img, 0)
    if magic != mkfs.MAGIC or version != mkfs.VERSION:
        sys.exit("mkdiskfull: %s: not a LogitFS v%d image" % (img_path, mkfs.VERSION))
    if bs != mkfs.BS:
        sys.exit("mkdiskfull: %s: unexpected block size %d" % (img_path, bs))

    # REFUSE AN IMAGE THAT STILL HAS A TRANSACTION IN ITS LOG, and this is the
    # one check in this file that is not about the bitmap.
    #
    # Everything below edits the PERSISTED bitmap. LogitFS stages bitmap
    # blocks through the journal (metadata-only + ordered data), so a commit
    # record sitting in the log names, among its bodies, THE VERY BLOCK THIS
    # TOOL JUST REWROTE -- and recovery runs at every mount, before anything
    # else. The image therefore boots, replays, and installs the pre-fill
    # bitmap over the fill. The disk is not full, the big write succeeds, and
    # close() correctly returns 0.
    #
    # WHAT THAT LOOKS LIKE, WHICH IS WHY IT IS WORTH A HARD REFUSAL: the gate
    # prints "FAIL: CLOSETEST-BIG rc=0 -- close() reported success for a write
    # that could not have fit in 2 free block(s); the fix did not reach
    # SYS_CLOSE". It accuses the kernel change under test, by name, of not
    # being there -- when the kernel is fine and the EXPERIMENT never
    # happened. Measured 2026-08-28: the only difference between the red run
    # and the green one was the line "[fs] log: replayed 8 block(s) from an
    # interrupted transaction" 170 lines earlier in the serial log.
    #
    # How a shared build/disk.img acquires one: any harness that boots it
    # WITHOUT -snapshot and is killed, or simply ends, mid-transaction. Five
    # of the six durability harnesses deliberately run without -snapshot, and
    # several agents boot this tree at once. `make build/disk.img` after
    # touching tools/mkfs.py writes a clean image; so does building into a
    # private BUILD=build-yours.
    #
    # The constants come from mkfs, not from a second copy of them here --
    # same reason mkreplay.py imports them (see its file comment).
    log_magic, = struct.unpack_from("<I", img, log_start * mkfs.BS + mkfs.LOGH_MAGIC * 4)
    if log_magic == mkfs.LOG_MAGIC:
        sys.exit(
            "mkdiskfull: %s carries a COMMITTED-BUT-UNINSTALLED transaction in its\n"
            "            log (block %d). Mounting it replays that transaction, which\n"
            "            would reinstall the bitmap this tool is about to edit and\n"
            "            silently undo the fill -- the gate would then fail blaming\n"
            "            the kernel. Refusing.\n"
            "            Fix: give it a clean image --\n"
            "                touch tools/mkfs.py && make build/disk.img\n"
            "            or build into a private tree: make test-closefull BUILD=build-yours"
            % (img_path, log_start))

    return img, dict(total=total, data_start=data_start, bitmap_start=bitmap_start)


def bit_test(img, geo, b):
    off = geo["bitmap_start"] * mkfs.BS + (b >> 3)
    return (img[off] >> (b & 7)) & 1


def bit_set(img, geo, b):
    off = geo["bitmap_start"] * mkfs.BS + (b >> 3)
    img[off] |= 1 << (b & 7)


def craft(img_in, img_out, keep_free):
    img, geo = load(img_in)
    free = [b for b in range(geo["data_start"], geo["total"])
            if not bit_test(img, geo, b)]
    if len(free) <= keep_free:
        sys.exit("mkdiskfull: %s has only %d free block(s), cannot fill it and "
                  "still leave %d free" % (img_in, len(free), keep_free))
    to_fill = free[:len(free) - keep_free]          # keep the LAST keep_free
    for b in to_fill:
        bit_set(img, geo, b)
    open(img_out, "wb").write(bytes(img))
    print("mkdiskfull: %s -> %s: filled %d free block(s), left %d free (%d bytes)"
          % (img_in, img_out, len(to_fill), keep_free, keep_free * mkfs.BS))


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    craft(sys.argv[1], sys.argv[2], int(sys.argv[3]))


if __name__ == "__main__":
    main()
