#!/usr/bin/env python3
"""Emit a reference dump for one font, for tests/unit/font_cff_test.c to check against.

Two independent references, because they catch different bugs:

  paths    fontTools' own Type 2 charstring interpreter (for CFF) or glyf
           decompiler, as an exact list of drawing commands in font units. A
           charstring interpreter with a wrong subroutine bias, a mis-sized
           hintmask or a flex operator built from the wrong deltas produces a
           glyph that still LOOKS like a letter, and no "it rendered something"
           assertion catches that. Comparing the path integer-for-integer does.

  bitmaps  FreeType's rasterization of the same glyph at the same pixel size,
           unhinted. This is the end of the pipeline rather than the middle, so
           it also covers the scaling, the curve flattening and the coverage
           accumulation -- but only to a tolerance, since FreeType computes exact
           area coverage over a 26.6 grid and ours samples four sub-scanlines
           over a 24.8 one. The tolerance is asserted by the C test, not here.

Usage: font_ref_gen.py FONT OUT.bin [--px N] [--max-glyphs N] [--bitmaps N]
"""
import argparse
import math
import struct
import sys

from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen

MAGIC = b"FTRF"
VERSION = 1

OP_MOVE, OP_LINE, OP_QUAD, OP_CUBIC, OP_CLOSE = 0, 1, 2, 3, 4


def rnd(v):
    """Match the C side's round-half-up on a 16.16 accumulator: floor(v + 0.5)."""
    return math.floor(v + 0.5)


class CmdPen(BasePen):
    """Records commands in the shape c/lib/text/fontpath.h uses.

    The one transformation applied is expanding a multi-control qCurveTo into
    single quadratics with the implied on-curve midpoints, because that is what a
    TrueType contour means and the rasterizer must see explicit segments.
    """

    def __init__(self, glyphSet):
        BasePen.__init__(self, glyphSet)
        self.cmds = []
        self._start = None

    def _moveTo(self, pt):
        self.cmds.append((OP_MOVE, [pt]))
        self._start = pt

    def _lineTo(self, pt):
        self.cmds.append((OP_LINE, [pt]))

    def _curveToOne(self, p1, p2, p3):
        self.cmds.append((OP_CUBIC, [p1, p2, p3]))

    def _qCurveToOne(self, p1, p2):
        self.cmds.append((OP_QUAD, [p1, p2]))

    def _closePath(self):
        self.cmds.append((OP_CLOSE, []))

    def _endPath(self):
        self.cmds.append((OP_CLOSE, []))


def glyf_path(font, name):
    """glyf outline -> commands, resolving the three implicit rules ourselves.

    Deliberately NOT fontTools' pen path for TrueType: its qCurveTo hands back a
    run of control points and leaves the implied midpoints to the consumer, and
    the point of a reference is that it does not share code or assumptions with
    what it is checking.
    """
    glyf = font["glyf"]
    g = glyf[name]
    g.expand(glyf)
    if g.numberOfContours == 0:
        return []
    if g.isComposite():
        coords, endPts, flags = g.getCoordinates(glyf)
        coords = list(coords)
    else:
        coords, endPts, flags = g.getCoordinates(glyf)
        coords = list(coords)
    cmds = []
    start = 0
    for end in endPts:
        pts = coords[start:end + 1]
        on = [bool(flags[i] & 1) for i in range(start, end + 1)]
        n = len(pts)
        if n >= 2:
            s0 = next((i for i in range(n) if on[i]), None)
            if s0 is None:
                s = (_trunc_half(pts[0][0] + pts[-1][0]),
                     _trunc_half(pts[0][1] + pts[-1][1]))
                s0 = 0
            else:
                s = pts[s0]
            cmds.append((OP_MOVE, [s]))
            ctrl = None
            for j in range(1, n + 1):
                idx = (s0 + j) % n
                p = pts[idx]
                if on[idx]:
                    if ctrl is not None:
                        cmds.append((OP_QUAD, [ctrl, p]))
                        ctrl = None
                    else:
                        cmds.append((OP_LINE, [p]))
                else:
                    if ctrl is not None:
                        mx = _trunc_half(ctrl[0] + p[0])
                        my = _trunc_half(ctrl[1] + p[1])
                        cmds.append((OP_QUAD, [ctrl, (mx, my)]))
                    ctrl = p
            if ctrl is not None:
                cmds.append((OP_QUAD, [ctrl, s]))
            else:
                cmds.append((OP_LINE, [s]))
            cmds.append((OP_CLOSE, []))
        start = end + 1
    return cmds


def _trunc_half(total):
    """C's `(a + b) / 2` on ints: division truncates toward zero."""
    return int(total / 2) if total < 0 else total // 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("font")
    ap.add_argument("out")
    ap.add_argument("--px", type=int, default=32)
    ap.add_argument("--max-glyphs", type=int, default=100000)
    ap.add_argument("--bitmaps", type=int, default=64)
    args = ap.parse_args()

    font = TTFont(args.font, fontNumber=0)
    upem = font["head"].unitsPerEm
    order = font.getGlyphOrder()
    n = min(len(order), args.max_glyphs)
    is_cff = "CFF " in font or "CFF2" in font
    glyphset = font.getGlyphSet() if is_cff else None
    hmtx = font["hmtx"]

    paths = []
    for gid in range(n):
        name = order[gid]
        if is_cff:
            pen = CmdPen(glyphset)
            glyphset[name].draw(pen)
            cmds = pen.cmds
            # A charstring with no path still emits an endPath in some pens;
            # drop a lone CLOSE so "empty" means the same on both sides.
            if len(cmds) == 1 and cmds[0][0] == OP_CLOSE:
                cmds = []
        else:
            cmds = glyf_path(font, name)
        adv = hmtx[name][0]
        paths.append((gid, adv, cmds))

    bitmaps = []
    try:
        import freetype
        face = freetype.Face(args.font)
        face.set_pixel_sizes(0, args.px)
        flags = (freetype.FT_LOAD_NO_HINTING | freetype.FT_LOAD_RENDER
                 | freetype.FT_LOAD_NO_BITMAP)
        step = max(1, n // args.bitmaps) if args.bitmaps else 1
        for gid in range(0, n, step):
            if len(bitmaps) >= args.bitmaps:
                break
            try:
                face.load_glyph(gid, flags)
            except Exception:
                continue
            b = face.glyph.bitmap
            if b.width == 0 or b.rows == 0:
                continue
            rows = []
            for r in range(b.rows):
                rows.append(bytes(b.buffer[r * b.pitch:r * b.pitch + b.width]))
            bitmaps.append((gid, face.glyph.bitmap_left, face.glyph.bitmap_top,
                            b.width, b.rows, b"".join(rows)))
    except ImportError:
        print("font_ref_gen: freetype-py absent, no bitmap reference", file=sys.stderr)

    with open(args.out, "wb") as fp:
        fp.write(MAGIC)
        fp.write(struct.pack("<IIIII", VERSION, upem, args.px, len(paths), len(bitmaps)))
        for gid, adv, cmds in paths:
            fp.write(struct.pack("<iiI", gid, adv, len(cmds)))
            for op, pts in cmds:
                xs = [0, 0, 0]
                ys = [0, 0, 0]
                for i, p in enumerate(pts):
                    xs[i] = rnd(p[0])
                    ys[i] = rnd(p[1])
                fp.write(struct.pack("<B6i", op, xs[0], xs[1], xs[2], ys[0], ys[1], ys[2]))
        for gid, left, top, w, h, data in bitmaps:
            fp.write(struct.pack("<iiiII", gid, left, top, w, h))
            fp.write(data)

    print("font_ref_gen: %s -> %d paths, %d bitmaps at %dpx (upem %d)%s"
          % (args.font, len(paths), len(bitmaps), args.px, upem,
             ", CFF" if is_cff else ", glyf"))


if __name__ == "__main__":
    main()
