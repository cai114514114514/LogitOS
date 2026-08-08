#!/usr/bin/env python3
"""tests/unit/gen_laced.py -- hand-built Matroska files that use LACING.

ffmpeg's Matroska muxer never writes lacing, so there is no way to obtain a
laced file from the tool the rest of this suite differentials against. That is
not a problem, it is an opportunity: a fixture built here has frame boundaries
that are known BY CONSTRUCTION, so the check is against arithmetic rather than
against another implementation's opinion. It is the same reason the H.265 unit
tests drive the CABAC decoder from the spec's arithmetic *encoder*.

Lacing matters. It exists so a muxer does not pay a block header for every
24 ms of audio, and it is common in files from mkvmerge and from hardware
recorders. A parser without it does not fail loudly: it hands the decoder the
first frame of every group and calls the remaining eleven part of it.

Three schemes, all in use, all written here:

  Xiph   sizes as runs of 0xFF terminated by a byte < 0xFF (from Ogg). The
         encoding of 255 is "255, 0" and of 254 is "254" -- an off-by-one that
         a reader gets wrong on exactly one frame size in 256.
  fixed  no sizes at all; the remaining bytes divide equally.
  EBML   the first size as an unsigned VINT, then SIGNED deltas, so a run of
         equal sizes costs one byte each. The bias is 2^(7n-1)-1 and depends
         on the length of each individual delta.

Each frame's payload is its own index repeated, so a checker can tell WHICH
frame it got as well as how long it was.

    python3 tests/unit/gen_laced.py <outdir>
"""
import sys, os, struct

def vint(v, force_len=None):
    """EBML unsigned variable-length integer (the size form, marker stripped)."""
    n = force_len or 1
    while v >= (1 << (7 * n)) - 1:
        n += 1
    out = bytearray(n)
    x = v | (1 << (7 * n))
    for i in range(n):
        out[n - 1 - i] = (x >> (8 * i)) & 0xFF
    return bytes(out)

def svint(v):
    """EBML SIGNED variable-length integer, as lacing deltas use."""
    n = 1
    while True:
        bias = (1 << (7 * n - 1)) - 1
        u = v + bias
        if 0 <= u < (1 << (7 * n)) - 1:
            return vint(u, force_len=n)
        n += 1

def elem(eid, payload):
    return bytes.fromhex(eid) + vint(len(payload)) + payload

def uint_el(eid, value):
    b = struct.pack(">Q", value).lstrip(b"\0") or b"\0"
    return elem(eid, b)

def str_el(eid, s):
    return elem(eid, s.encode())

def float_el(eid, f):
    return elem(eid, struct.pack(">d", f))

def xiph_sizes(sizes):
    out = bytearray()
    for s in sizes:
        while s >= 255:
            out.append(255)
            s -= 255
        out.append(s)
    return bytes(out)

def ebml_sizes(sizes):
    out = bytearray(vint(sizes[0]))
    prev = sizes[0]
    for s in sizes[1:]:
        out += svint(s - prev)
        prev = s
    return bytes(out)

def simple_block(track, rel_ts, keyframe, lacing, frames):
    """lacing: 0 none, 1 Xiph, 2 fixed, 3 EBML."""
    flags = (0x80 if keyframe else 0) | (lacing << 1)
    body = bytearray(vint(track))
    body += struct.pack(">h", rel_ts)
    body.append(flags)
    if lacing:
        body.append(len(frames) - 1)
        if lacing == 1:
            body += xiph_sizes([len(f) for f in frames[:-1]])
        elif lacing == 3:
            body += ebml_sizes([len(f) for f in frames[:-1]])
        # fixed carries no sizes at all
    for f in frames:
        body += f
    return elem("A3", bytes(body))

def build(path, lacing, groups, start_index=0):
    """groups: list of lists of frame lengths. Returns the expected samples."""
    hdr = elem("1A45DFA3",
               uint_el("4286", 1) + uint_el("42F7", 1) + uint_el("42F2", 4) +
               uint_el("42F3", 8) + str_el("4282", "matroska") +
               uint_el("4287", 4) + uint_el("4285", 2))

    info = elem("1549A966", uint_el("2AD7B1", 1000000) + float_el("4489", 1000.0))
    # DefaultDuration: 4 ms, which is what 64 bytes of 8 kHz 16-bit mono lasts.
    # It is the container's statement of how far apart the frames in one laced
    # block are, and both this parser and ffmpeg's spread a lace by it.
    track = elem("AE", uint_el("D7", 1) + uint_el("73C5", 1) + uint_el("83", 2) +
                       uint_el("23E383", 4000000) +
                       str_el("86", "A_PCM/INT/LIT") +
                       elem("E1", float_el("B5", 8000.0) + uint_el("9F", 1) +
                                  uint_el("6264", 16)))
    tracks = elem("1654AE6B", track)

    expect, idx = [], start_index
    clusters = bytearray()
    for g, lengths in enumerate(groups):
        frames = [bytes([(idx + i) & 0xFF]) * lengths[i] for i in range(len(lengths))]
        blk = simple_block(1, 0, True, lacing, frames)
        clusters += elem("1F43B675", uint_el("E7", g * 10) + blk)
        # DefaultDuration is 4 ms and the tick is 1 ms, so frame i of a lace
        # lands 4 ticks after frame i-1. An unlaced block has one frame and so
        # keeps the cluster's own stamp.
        for i, ln in enumerate(lengths):
            expect.append((g * 10 + (i * 4 if lacing else 0), ln, (idx + i) & 0xFF))
        idx += len(lengths)

    seg = elem("18538067", info + tracks + bytes(clusters))
    open(path, "wb").write(hdr + seg)
    return expect

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/media"
    os.makedirs(out, exist_ok=True)
    lines = []

    # Xiph: sizes that straddle the 255 boundary in both directions, which is
    # where the "255 is encoded as 255,0" rule bites.
    e = build(os.path.join(out, "laced-xiph.mkv"), 1,
              [[10, 254, 255, 256, 7], [1, 2, 3], [509, 8]])
    lines += [("laced-xiph.mkv",) + x for x in e]

    # Fixed: no sizes on the wire at all, so a reader that expects them eats
    # the payload. Two clusters so the division is done twice.
    e = build(os.path.join(out, "laced-fixed.mkv"), 2,
              [[64] * 6, [100] * 3])
    lines += [("laced-fixed.mkv",) + x for x in e]

    # EBML: ascending, descending and equal deltas, and one delta big enough to
    # need a two-byte signed VINT (bias 8191 rather than 63).
    e = build(os.path.join(out, "laced-ebml.mkv"), 3,
              [[100, 120, 90, 90, 5000, 4990], [7, 7, 7, 7]])
    lines += [("laced-ebml.mkv",) + x for x in e]

    # A control: the SAME frames with no lacing at all. If the laced files and
    # this one do not produce identical sample sequences, lacing is wrong.
    e = build(os.path.join(out, "laced-none.mkv"), 0,
              [[64], [64], [64], [64], [64], [64]])
    lines += [("laced-none.mkv",) + x for x in e]

    with open(os.path.join(out, "laced.expect"), "w") as f:
        f.write("# file pts_ticks size first_byte -- built by gen_laced.py\n")
        for name, pts, size, first in lines:
            f.write("%s %d %d %d\n" % (name, pts, size, first))
    print("wrote 4 laced fixtures and laced.expect to %s" % out)

if __name__ == "__main__":
    main()
