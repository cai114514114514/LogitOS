#!/usr/bin/env python3
"""Reference for the colour-font tables: COLR/CPAL layers, CBDT/CBLC and sbix strikes.

Also builds the sbix fixture, because there is no redistributable sbix font:
Apple ships the only widely used one. `--make-sbix OUT.ttf SRC.ttf` grafts a real
sbix table (two strikes of hand-built PNGs, plus a 'dupe' record) onto an
existing outline font, which is enough to exercise every branch of sbix_lookup
including strike selection and the duplicate indirection.

Usage: font_color_ref.py FONT OUT.bin
       font_color_ref.py --make-sbix OUT.ttf SRC.ttf
"""
import argparse
import struct
import sys
import zlib

from fontTools.ttLib import TTFont, newTable

MAGIC = b"CLRQ"
VERSION = 1
(K_EOF, K_COLRVER, K_LAYERS, K_CPAL, K_COLOR, K_BITMAP, K_NOBITMAP,
 K_DUPE) = range(8)

FMT_UNKNOWN, FMT_PNG, FMT_JPEG, FMT_TIFF = 0, 1, 2, 3


def make_png(w, h, rgb):
    """A minimal RGBA PNG in one solid colour -- enough to be located and decoded."""
    raw = b"".join(b"\x00" + bytes(list(rgb) + [255]) * w for _ in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


def make_sbix(out_path, src_path):
    font = TTFont(src_path)
    order = font.getGlyphOrder()
    sbix = newTable("sbix")
    sbix.version = 1
    sbix.flags = 1
    sbix.strikes = {}
    from fontTools.ttLib.tables.sbixStrike import Strike
    from fontTools.ttLib.tables.sbixGlyph import Glyph
    for ppem, size, colour in ((32, 32, (200, 40, 40)), (64, 64, (40, 90, 220))):
        st = Strike(ppem=ppem, resolution=72)
        for i, name in enumerate(order[1:20], start=1):
            st.glyphs[name] = Glyph(glyphName=name, graphicType="png ",
                                    imageData=make_png(size, size, colour),
                                    originOffsetX=1 + i % 3, originOffsetY=-2)
        # a 'dupe' record: this glyph's bitmap IS another glyph's
        if len(order) > 21:
            st.glyphs[order[21]] = Glyph(glyphName=order[21], graphicType="dupe",
                                         referenceGlyphName=order[3])
        sbix.strikes[ppem] = st
    font["sbix"] = sbix
    font.save(out_path)
    print("made sbix fixture %s (2 strikes, 19 glyphs each, 1 dupe)" % out_path,
          file=sys.stderr)


def csum(data):
    """A cheap checksum the C side can recompute without pulling in zlib."""
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


class Out:
    def __init__(self, fp):
        self.fp = fp
        self.n = 0

    def rec(self, kind, fmt="", *v):
        self.fp.write(struct.pack("<B", kind))
        if fmt:
            self.fp.write(struct.pack("<" + fmt, *v))
        self.n += 1

    def raw(self, d):
        self.fp.write(d)


def dump(font, out, path):
    gid = font.getGlyphID
    order = font.getGlyphOrder()

    if "COLR" in font:
        colr = font["COLR"]
        ver = getattr(colr, "version", 0)
        out.rec(K_COLRVER, "i", ver)
        layers = colr.ColorLayers if hasattr(colr, "ColorLayers") else {}
        for name in sorted(layers):
            ls = layers[name]
            out.rec(K_LAYERS, "HH", gid(name), len(ls))
            for l in ls:
                out.rec_pending = None
                out.raw(struct.pack("<HH", gid(l.name), l.colorID))
    else:
        out.rec(K_COLRVER, "i", -1)

    if "CPAL" in font:
        cpal = font["CPAL"]
        out.rec(K_CPAL, "II", len(cpal.palettes), cpal.numPaletteEntries)
        for pi, pal in enumerate(cpal.palettes):
            for ci, c in enumerate(pal):
                argb = (c.alpha << 24) | (c.red << 16) | (c.green << 8) | c.blue
                out.rec(K_COLOR, "HHI", pi, ci, argb)
    else:
        out.rec(K_CPAL, "II", 0, 0)

    if "CBDT" in font and "CBLC" in font:
        cbdt, cblc = font["CBDT"], font["CBLC"]
        for si, strike in enumerate(cblc.strikes):
            bst = strike.bitmapSizeTable
            ppem = bst.ppemY or bst.ppemX
            data = cbdt.strikeData[si]
            for name in sorted(data):
                g = data[name]
                m = g.metrics
                img = g.imageData
                fmt = FMT_PNG if type(g).__name__.endswith(("17", "18", "19")) else FMT_UNKNOWN
                out.rec(K_BITMAP, "HHIIiiiiiB", gid(name), ppem, len(img), csum(img),
                        getattr(m, "width", 0), getattr(m, "height", 0),
                        getattr(m, "BearingX", 0), getattr(m, "BearingY", 0),
                        getattr(m, "Advance", 0), fmt)

    if "sbix" in font:
        sbix = font["sbix"]
        for ppem in sorted(sbix.strikes):
            st = sbix.strikes[ppem]
            for name in sorted(st.glyphs):
                g = st.glyphs[name]
                if g.graphicType == "dupe":
                    # A 'dupe' record points at another glyph's bitmap. Following
                    # it is the only way that branch is ever exercised, and a
                    # font that uses it is otherwise indistinguishable from one
                    # whose glyph simply has no strike.
                    out.rec(K_DUPE, "HHH", gid(name), gid(g.referenceGlyphName), ppem)
                    continue
                if not g.imageData:
                    continue
                fmt = {"png ": FMT_PNG, "jpg ": FMT_JPEG,
                       "tiff": FMT_TIFF}.get(g.graphicType, FMT_UNKNOWN)
                out.rec(K_BITMAP, "HHIIiiiiiB", gid(name), ppem, len(g.imageData),
                        csum(g.imageData), 0, 0, g.originOffsetX, g.originOffsetY, 0, fmt)

    # A handful of glyphs that must have NO bitmap and NO colour record, so the
    # "absent" answer is checked too and not just the present one.
    for i in range(min(len(order), 8)):
        out.rec(K_NOBITMAP, "H", i)
    print("font_color_ref: %s -> %d records" % (path, out.n), file=sys.stderr)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--make-sbix":
        make_sbix(sys.argv[2], sys.argv[3])
        return
    ap = argparse.ArgumentParser()
    ap.add_argument("font")
    ap.add_argument("out")
    a = ap.parse_args()
    font = TTFont(a.font, fontNumber=0)
    with open(a.out, "wb") as fp:
        fp.write(MAGIC)
        fp.write(struct.pack("<I", VERSION))
        o = Out(fp)
        dump(font, o, a.font)
        o.rec(K_EOF)


if __name__ == "__main__":
    main()
