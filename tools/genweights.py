#!/usr/bin/env python3
"""Write a weight file bigger than the machine's RAM, WITHOUT holding it in RAM.

    genweights.py <out> <MiB> [seed]

WHY THIS TOOL EXISTS AND IS NOT tools/lmshape.c
-----------------------------------------------
The experiment this feeds needs one thing from a model file -- that it be
LARGER THAN PHYSICAL MEMORY and that every page of it be CHECKABLE from ring 3
without reading the whole thing back on the host. Real weights are neither:
they are whatever the trainer produced, and verifying a page of them means
having the trainer's output to compare against. So the bytes here are
generated, and the tool is deliberately its own file: c/apps/lm and
tools/lmshape.c belong to another line that is mid-edit, and a shared
dependency between two live workflows is a way for one to break the other.

STREAMING IS THE POINT, not an optimisation. The file is 640 MiB against a host
that could hold it -- but the whole claim under test is "bigger than memory",
and a generator that materialises its own output has already assumed the thing
it is meant to be testing. `chunk` bytes are live at a time and nothing else.

THE LAYOUT
----------
Page 0 is a LOGITLM-shaped header: magic, version, page size, page count, seed,
the three mark coefficients, and a plausible set of model dimensions. It is
read by the guest THROUGH THE MAPPING, so the page count the walker trusts came
off the disk rather than off its own command line -- which is what makes a
mapping that silently returned zeroes fail rather than pass with 0 pages.

Every page p >= 1 is filled with deterministic pseudo-random bytes and then has
four MARKS written over it, at word offsets 0, 170, 340 and 511 of the page:

    value at word w of page p  =  p * 1000003 + w * 7 + 12345

Both operands are in it on purpose; this is the formula
fsroot/as/examples/mempress.as already argues for, and the argument carries
over unchanged. It distinguishes the three ways a paging path goes wrong and a
simpler pattern does not:

    every page reads back zero       -> the page was never filled from the file
    page N reads back as page M      -> the offset arithmetic is wrong
    one word in a page is wrong      -> the transfer is off by an offset

Four words rather than 512: the walker is a bytecode interpreter and 4096 bytes
a page over 163,840 pages is hours of VM time, while four words spread across
the page catch every failure above. The words BETWEEN the marks are the
pseudo-random filler and the guest never checks them -- their job is to make
the image incompressible, so that neither the host filesystem nor QEMU can
serve a 640 MiB file out of a hole and make demand paging look faster than it
is.

The pseudo-random stream is one Mersenne Twister seeded once, consumed in
order, so the file is byte-for-byte reproducible from (MiB, seed) alone. It is
NOT per-page-derivable, which is fine: the marks carry the verification and the
filler carries none.
"""
import os
import struct
import sys
import random

PAGE = 4096
WORDS = PAGE // 8
MAGIC = b"LOGITLMW"
VERSION = 1

MARK_A = 1000003
MARK_B = 7
MARK_C = 12345
MARK_WORDS = (0, 170, 340, WORDS - 1)


def mark(p, w):
    return p * MARK_A + w * MARK_B + MARK_C


def header(n_pages, seed):
    """Page 0. Shaped like a model file's header so the walker reads a real
    description of the file rather than a length it was told on the side."""
    h = bytearray(PAGE)
    h[0:8] = MAGIC
    struct.pack_into("<I", h, 8, VERSION)
    struct.pack_into("<I", h, 12, PAGE)
    struct.pack_into("<Q", h, 16, n_pages)
    struct.pack_into("<Q", h, 24, seed)
    struct.pack_into("<Q", h, 32, MARK_A)
    struct.pack_into("<Q", h, 40, MARK_B)
    struct.pack_into("<Q", h, 48, MARK_C)
    struct.pack_into("<I", h, 56, len(MARK_WORDS))
    for i, w in enumerate(MARK_WORDS):
        struct.pack_into("<I", h, 60 + 4 * i, w)
    # A LOGITLM-shaped tail: dim / layers / heads / vocab / seq. Nothing reads
    # these yet. They are here because the file is meant to stand in for a real
    # one, and a header that describes nothing is a header a reader learns to
    # skip.
    struct.pack_into("<5I", h, 96, 1024, 24, 16, 151936, 32768)
    return bytes(h)


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: genweights.py <out> <MiB> [seed]")
    out = sys.argv[1]
    mib = int(sys.argv[2])
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 0x10617

    n_pages = mib * 1024 * 1024 // PAGE
    if n_pages < 2:
        sys.exit("genweights: need at least 2 pages")

    rng = random.Random(seed)
    PAGES_PER_CHUNK = 256                      # 1 MiB live at a time, and no more
    written = 0
    with open(out, "wb") as f:
        f.write(header(n_pages, seed))
        written += 1
        while written < n_pages:
            n = min(PAGES_PER_CHUNK, n_pages - written)
            buf = bytearray(rng.randbytes(n * PAGE))
            for i in range(n):
                p = written + i
                base = i * PAGE
                for w in MARK_WORDS:
                    struct.pack_into("<q", buf, base + w * 8, mark(p, w))
            f.write(buf)
            written += n
    size = os.path.getsize(out)
    print(f"genweights: {out} -> {size} bytes ({size / (1 << 20):.1f} MiB), "
          f"{n_pages} pages, seed {seed:#x}, "
          f"marks at words {MARK_WORDS} of every page >= 1")
    if size != n_pages * PAGE:
        sys.exit(f"genweights: wrote {size}, expected {n_pages * PAGE}")


if __name__ == "__main__":
    main()
