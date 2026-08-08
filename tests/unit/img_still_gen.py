#!/usr/bin/env python3
"""Ground truth for the still-image decoders added alongside PNG: BMP, ICO and
WebP-lossless. Same contract as png_gen.py -- for each case write <name>.<ext>
plus <name>.rgba (the expected straight RGBA8) and a manifest line.

Where the reference comes from, per case, because it is not the same everywhere:

  * PIL decodes the IDENTICAL file we hand our decoder. That is the real
    reference and covers most cases: PIL's BMP/ICO readers and its libwebp
    binding are independent implementations, and every one of these formats is
    lossless, so the bar is byte-for-byte with no tolerance anywhere.

  * A few cases PIL cannot read (RLE4, and 32bpp BMP with a real alpha mask --
    PIL drops BMP alpha unconditionally). For those the file is written by a
    trivially-invertible encoder from known source pixels: each 8-bit channel is
    placed at the bit position its mask names, so the expected output IS the
    source array. The encoder being trivial is the point; the decoder is not.
    Such cases are marked "self" in the printed log so nobody mistakes them for
    a cross-check against another implementation.

Usage: img_still_gen.py <outdir>
"""
import os, struct, sys, io, zlib
from PIL import Image

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
manifest = []
W, H = 23, 17          # odd width -> every bpp hits row padding


def case(name, ext, data, expected, ref):
    open(os.path.join(OUT, f"{name}.{ext}"), "wb").write(data)
    open(os.path.join(OUT, f"{name}.rgba"), "wb").write(expected)
    w = len(expected) // 4
    manifest.append(f"{name} {ext} {case.w} {case.h}")
    print(f"  {name:16s} {ext:4s} {case.w}x{case.h}  ref={ref}")


def emit(name, ext, data, w, h, expected, ref):
    case.w, case.h = w, h
    assert len(expected) == w * h * 4, (name, len(expected), w * h * 4)
    case(name, ext, data, expected, ref)


def pil_ref(data):
    """Decode the exact bytes we are about to ship, with PIL, as RGBA."""
    im = Image.open(io.BytesIO(data))
    im.load()
    return im.convert("RGBA"), im.size


def emit_pil(name, ext, data, ref="PIL"):
    im, (w, h) = pil_ref(data)
    emit(name, ext, data, w, h, im.tobytes(), ref)


# --------------------------------------------------------------------------
# source patterns
# --------------------------------------------------------------------------
def grad(mode, w=W, h=H):
    im = Image.new(mode, (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if mode == "RGB":
                px[x, y] = ((x * 11) & 0xFF, (y * 13) & 0xFF, ((x + y) * 7) & 0xFF)
            elif mode == "RGBA":
                px[x, y] = ((x * 11) & 0xFF, (y * 13) & 0xFF, ((x * y) * 3) & 0xFF, (x * 7 + y * 5) & 0xFF)
            elif mode == "L":
                px[x, y] = (x * 11 + y * 7) & 0xFF
            elif mode == "1":
                px[x, y] = 1 if ((x + y) & 1) else 0
            elif mode == "P":
                px[x, y] = (x * 3 + y * 5) & 0xFF
    if mode == "P":
        im.putpalette([v for i in range(256) for v in ((i * 7) & 0xFF, (i * 13) & 0xFF, (i * 29) & 0xFF)])
    return im


# --------------------------------------------------------------------------
# a minimal BMP writer, so we can produce the variants PIL will not write
# --------------------------------------------------------------------------
def bmp(bpp, w, h, rows, palette=None, comp=0, masks=None, hdrsz=40, topdown=False):
    """rows: list of already-packed scanlines, TOP row first."""
    extra = b""
    if masks and hdrsz == 40:
        comp = 3 if len(masks) == 3 else 6
        extra = struct.pack("<" + "I" * len(masks), *masks)
    elif masks and hdrsz >= 108:
        pass                                       # masks live inside the header
    palb = b""
    if palette is not None:
        if hdrsz == 12:
            palb = b"".join(struct.pack("<BBB", c[2], c[1], c[0]) for c in palette)
        else:
            palb = b"".join(struct.pack("<BBBB", c[2], c[1], c[0], 0) for c in palette)
    body = b"".join(rows if topdown else rows[::-1])
    off = 14 + hdrsz + len(extra) + len(palb)
    if hdrsz == 12:
        dib = struct.pack("<IHHHH", 12, w, h, 1, bpp)
    else:
        hh = -h if topdown else h
        dib = struct.pack("<IiiHHIIiiII", hdrsz, w, hh, 1, bpp, comp, len(body),
                          2835, 2835, len(palette) if palette else 0, 0)
        if hdrsz >= 108:
            r, g, b, a = masks if masks else (0, 0, 0, 0)
            dib += struct.pack("<IIII", r, g, b, a)
            dib += b"\x20\x6e\x69\x57"             # 'Win ' colour space
            dib += b"\0" * (hdrsz - len(dib))
    return b"BM" + struct.pack("<IHHI", off + len(body), 0, 0, off) + dib + extra + palb + body


def pack_rows(w, h, bpp, pixfn):
    stride = ((w * bpp + 31) // 32) * 4
    rows = []
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            v = pixfn(x, y)
            if bpp >= 8:
                nb = bpp // 8
                for k in range(nb):
                    row[x * nb + k] = (v >> (8 * k)) & 0xFF
            else:
                per = 8 // bpp
                shift = 8 - bpp - (x % per) * bpp
                row[(x * bpp) // 8] |= (v & ((1 << bpp) - 1)) << shift
        rows.append(bytes(row))
    return rows


# ---- BMP cases PIL both writes and reads --------------------------------
for name, mode in (("bmp24", "RGB"), ("bmp8pal", "P"), ("bmp1", "1"), ("bmp32", "RGBA")):
    b = io.BytesIO()
    try:
        grad(mode).save(b, "BMP")
    except OSError:
        continue
    emit_pil(name, "bmp", b.getvalue())

# ---- BMP variants PIL reads but will not write ---------------------------
pal16 = [((i * 17) & 0xFF, (255 - i * 13) & 0xFF, (i * 5) & 0xFF) for i in range(16)]
rows4 = pack_rows(W, H, 4, lambda x, y: (x * 3 + y * 5) & 0x0F)
emit_pil("bmp4", "bmp", bmp(4, W, H, rows4, palette=pal16), "PIL")

rows_core = pack_rows(W, H, 24, lambda x, y: ((x * 11) << 16) | ((y * 13) << 8) | (((x + y) * 7) & 0xFF))
# 24bpp stores B,G,R -> build the value little-endian as B | G<<8 | R<<16
rows_core = pack_rows(W, H, 24, lambda x, y: (((x + y) * 7) & 0xFF) | (((y * 13) & 0xFF) << 8) | (((x * 11) & 0xFF) << 16))
emit_pil("bmp_core", "bmp", bmp(24, W, H, rows_core, hdrsz=12), "PIL")
emit_pil("bmp_topdown", "bmp", bmp(24, W, H, rows_core, topdown=True), "PIL")

rows555 = pack_rows(W, H, 16, lambda x, y: (((x * 11) >> 3 & 31) << 10) | (((y * 13) >> 3 & 31) << 5) | (((x + y) * 7) >> 3 & 31))
emit_pil("bmp16_555", "bmp", bmp(16, W, H, rows555), "PIL")
rows565 = pack_rows(W, H, 16, lambda x, y: (((x * 11) >> 3 & 31) << 11) | (((y * 13) >> 2 & 63) << 5) | (((x + y) * 7) >> 3 & 31))
emit_pil("bmp16_565", "bmp", bmp(16, W, H, rows565, masks=(0xF800, 0x07E0, 0x001F)), "PIL")

pal256 = [((i * 7) & 0xFF, (i * 13) & 0xFF, (i * 29) & 0xFF) for i in range(256)]


def rle8(w, h, pixfn):
    """RLE8 exercising encoded runs, absolute runs, EOL and a delta."""
    out = bytearray()
    for y in range(h):
        x = 0
        while x < w:
            v = pixfn(x, y)
            run = 1
            while x + run < w and pixfn(x + run, y) == v and run < 255:
                run += 1
            if run >= 3:
                out += bytes((run, v))
                x += run
            else:
                lit = []
                while x < w and len(lit) < 254:
                    v2 = pixfn(x, y)
                    if x + 2 < w and pixfn(x + 1, y) == v2 and pixfn(x + 2, y) == v2:
                        break
                    lit.append(v2)
                    x += 1
                if len(lit) < 3:
                    for v2 in lit:
                        out += bytes((1, v2))
                else:
                    out += bytes((0, len(lit))) + bytes(lit)
                    if len(lit) & 1:
                        out += b"\0"
        out += b"\0\0"
    out += b"\0\1"
    return bytes(out)


rlepix = lambda x, y: (x * 9 + y * 3) // 4 % 256
emit_pil("bmp_rle8", "bmp", bmp(8, W, H, [rle8(W, H, rlepix)], palette=pal256, comp=1), "PIL")

# ---- cases PIL cannot read: self-referential, trivially-invertible encoder --
src = grad("RGBA").tobytes()
rows32a = pack_rows(W, H, 32, lambda x, y: (
    src[(y * W + x) * 4 + 2]                      # B  -> bits 0..7
    | (src[(y * W + x) * 4 + 1] << 8)             # G  -> bits 8..15
    | (src[(y * W + x) * 4 + 0] << 16)            # R  -> bits 16..23
    | (src[(y * W + x) * 4 + 3] << 24)))          # A  -> bits 24..31
emit("bmp32_alpha", "bmp",
     bmp(32, W, H, rows32a, hdrsz=108,
         masks=(0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)),
     W, H, src, "self(V5 alpha mask; PIL drops BMP alpha)")


def rle4(w, h, pixfn):
    out = bytearray()
    for y in range(h):
        x = 0
        while x < w:
            v = pixfn(x, y)
            run = 1
            while x + run < w and pixfn(x + run, y) == v and run < 254:
                run += 1
            if run >= 4:
                out += bytes((run, (v << 4) | v))
                x += run
            else:
                lit = [pixfn(x + i, y) for i in range(min(6, w - x))]
                if len(lit) < 3:
                    for i, v2 in enumerate(lit):
                        out += bytes((1, (v2 << 4) | v2))
                    x += len(lit)
                else:
                    packed = bytearray()
                    for i in range(0, len(lit), 2):
                        hi = lit[i]
                        lo = lit[i + 1] if i + 1 < len(lit) else 0
                        packed.append((hi << 4) | lo)
                    out += bytes((0, len(lit))) + bytes(packed)
                    if len(packed) & 1:
                        out += b"\0"
                    x += len(lit)
        out += b"\0\0"
    out += b"\0\1"
    return bytes(out)


rle4pix = lambda x, y: (x // 3 + y // 2) & 0x0F
# RLE rows are stored bottom-up like every other non-top-down BMP, so stream
# row 0 is DISPLAY row H-1. Getting this backwards is invisible on a symmetric
# test pattern, which is why this one is not symmetric.
exp4 = bytearray()
for y in range(H):
    for x in range(W):
        c = pal16[rle4pix(x, H - 1 - y)]
        exp4 += bytes((c[0], c[1], c[2], 255))
emit("bmp_rle4", "bmp", bmp(4, W, H, [rle4(W, H, rle4pix)], palette=pal16, comp=2),
     W, H, bytes(exp4), "self(RLE4; PIL cannot read it)")

# --------------------------------------------------------------------------
# ICO
# --------------------------------------------------------------------------
def ico(entries):
    """entries: list of (w, h, bpp, payload_bytes)."""
    n = len(entries)
    dir_ = struct.pack("<HHH", 0, 1, n)
    off = 6 + 16 * n
    body = b""
    for (w, h, bpp, data) in entries:
        dir_ += struct.pack("<BBBBHHII", w & 0xFF, h & 0xFF, 0, 0, 1, bpp, len(data), off)
        off += len(data)
        body += data
    return dir_ + body


def dib_icon(im, bpp):
    """A headerless DIB + 1-bit AND mask, the classic icon payload."""
    w, h = im.size
    if bpp == 32:
        src = im.convert("RGBA").tobytes()
        rows = pack_rows(w, h, 32, lambda x, y: (
            src[(y * w + x) * 4 + 2] | (src[(y * w + x) * 4 + 1] << 8)
            | (src[(y * w + x) * 4] << 16) | (src[(y * w + x) * 4 + 3] << 24)))
        pal = None
        androws = [b"\0" * (((w + 31) // 32) * 4)] * h
    else:
        p = im.convert("P", palette=Image.ADAPTIVE, colors=256)
        pl = p.getpalette()[: 256 * 3]
        pal = [(pl[i * 3], pl[i * 3 + 1], pl[i * 3 + 2]) for i in range(256)]
        idx = p.tobytes()
        rows = pack_rows(w, h, 8, lambda x, y: idx[y * w + x])
        # transparent border: AND-mask bit set == transparent
        mstride = ((w + 31) // 32) * 4
        androws = []
        for y in range(h):
            r = bytearray(mstride)
            for x in range(w):
                if x == 0 or y == 0:
                    r[x >> 3] |= 0x80 >> (x & 7)
            androws.append(bytes(r))
    palb = b""
    if pal:
        palb = b"".join(struct.pack("<BBBB", c[2], c[1], c[0], 0) for c in pal)
    body = b"".join(rows[::-1]) + b"".join(androws[::-1])
    dib = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, bpp, 0, len(body), 0, 0,
                      256 if pal else 0, 0)
    return dib + palb + body


small = grad("RGBA", 16, 16)
big = grad("RGBA", 32, 32)

pngbytes = io.BytesIO()
big.save(pngbytes, "PNG")
emit_pil("ico_png", "ico", ico([(32, 32, 32, pngbytes.getvalue())]), "PIL")
emit_pil("ico_dib32", "ico", ico([(16, 16, 32, dib_icon(small, 32))]), "PIL")
emit_pil("ico_dib8", "ico", ico([(16, 16, 8, dib_icon(small.convert("RGB"), 8))]), "PIL")
# multi-entry: the 32x32 must win over the 16x16
emit_pil("ico_multi", "ico",
         ico([(16, 16, 32, dib_icon(small, 32)), (32, 32, 32, dib_icon(big, 32))]), "PIL")

# --------------------------------------------------------------------------
# WebP lossless
# --------------------------------------------------------------------------
def webp_lossless(im):
    b = io.BytesIO()
    im.save(b, "WEBP", lossless=True, quality=100, method=4)
    return b.getvalue()


emit_pil("webp_rgb", "webp", webp_lossless(grad("RGB")), "PIL/libwebp")
emit_pil("webp_rgba", "webp", webp_lossless(grad("RGBA")), "PIL/libwebp")
# A flat-ish image encodes with the colour-indexing transform + pixel bundling.
flat = Image.new("RGB", (W, H))
fp = flat.load()
for y in range(H):
    for x in range(W):
        fp[x, y] = [(255, 0, 0), (0, 128, 255), (12, 200, 7), (255, 255, 255)][(x // 5 + y // 4) % 4]
emit_pil("webp_indexed", "webp", webp_lossless(flat), "PIL/libwebp")
# A big smooth gradient exercises the predictor + cross-colour transforms and
# long LZ77 matches; 129 wide so the meta-Huffman block grid is not square.
smooth = Image.new("RGB", (129, 71))
sp = smooth.load()
for y in range(71):
    for x in range(129):
        sp[x, y] = ((x * 2) & 0xFF, (y * 3 + x) & 0xFF, (x * y) & 0xFF)
emit_pil("webp_smooth", "webp", webp_lossless(smooth), "PIL/libwebp")
# Photographic noise: forces literal-heavy coding and the colour cache.
import random
random.seed(7)
noise = Image.new("RGBA", (64, 48))
np_ = noise.load()
for y in range(48):
    for x in range(64):
        np_[x, y] = (random.randrange(256), random.randrange(256), random.randrange(256),
                     random.choice([255, 255, 255, 128, 0]))
emit_pil("webp_noise", "webp", webp_lossless(noise), "PIL/libwebp")
# 1-pixel wide: the predictor's top-right tap has nowhere to go.
thin = Image.new("RGB", (1, 40))
for y in range(40):
    thin.putpixel((0, y), (y * 6 & 0xFF, 255 - y * 5 & 0xFF, y * 3 & 0xFF))
emit_pil("webp_thin", "webp", webp_lossless(thin), "PIL/libwebp")

open(os.path.join(OUT, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
print(f"generated {len(manifest)} cases")
