#!/usr/bin/env python3
"""Generate the committed fixtures that ride on the LogitFS disk image, so the
same bytes are decoded on the host and inside the guest (make test-imgcheck).

One file per format that this line added, small enough to commit:
  still.bmp   24bpp bottom-up BMP
  icon.ico    multi-entry icon, 32bpp DIB with alpha + an 8bpp entry
  still.webp  VP8L lossless
  anim.gif    animated, every disposal method, per-frame delays
  anim.apng   animated PNG, dispose BACKGROUND/PREVIOUS and blend OVER
  rot.jpg     a JPEG whose EXIF says orientation 6 (rotate 90 CW)

Note the .apng extension: .gitignore excludes *.png, and the decoders sniff
magic bytes rather than names, so the animated PNG is committed under a name
git will keep.

Usage: img_fixture_gen.py <outdir>
"""
import os, struct, sys, io, zlib
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
W, H = 40, 28


def pattern(w, h, seed=0):
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 7 + seed * 31) & 0xFF, (y * 11 + seed * 17) & 0xFF,
                        ((x ^ y) * 5 + seed) & 0xFF, 255 - ((x * y) & 0x7F))
    return im


def write(name, data):
    open(os.path.join(OUT, name), "wb").write(data)
    print(f"  {name:14s} {len(data):6d} bytes")


# --- BMP -------------------------------------------------------------------
b = io.BytesIO()
pattern(W, H).convert("RGB").save(b, "BMP")
write("still.bmp", b.getvalue())

# --- WebP lossless ---------------------------------------------------------
b = io.BytesIO()
pattern(W, H).save(b, "WEBP", lossless=True, quality=100, method=5)
write("still.webp", b.getvalue())

# --- ICO -------------------------------------------------------------------
def pack_rows(w, h, bpp, pixfn):
    stride = ((w * bpp + 31) // 32) * 4
    rows = []
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            v = pixfn(x, y)
            nb = bpp // 8
            for k in range(nb):
                row[x * nb + k] = (v >> (8 * k)) & 0xFF
        rows.append(bytes(row))
    return rows


def dib32(im):
    w, h = im.size
    s = im.convert("RGBA").tobytes()
    rows = pack_rows(w, h, 32, lambda x, y: (
        s[(y * w + x) * 4 + 2] | (s[(y * w + x) * 4 + 1] << 8)
        | (s[(y * w + x) * 4] << 16) | (s[(y * w + x) * 4 + 3] << 24)))
    andm = [b"\0" * (((w + 31) // 32) * 4)] * h
    body = b"".join(rows[::-1]) + b"".join(andm[::-1])
    return struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(body), 0, 0, 0, 0) + body


def ico(entries):
    d = struct.pack("<HHH", 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    body = b""
    for (w, h, bpp, data) in entries:
        d += struct.pack("<BBBBHHII", w & 0xFF, h & 0xFF, 0, 0, 1, bpp, len(data), off)
        off += len(data)
        body += data
    return d + body


write("icon.ico", ico([(16, 16, 32, dib32(pattern(16, 16, 3))),
                       (32, 32, 32, dib32(pattern(32, 32, 5)))]))

# --- animated GIF ----------------------------------------------------------
PAL = [(0, 0, 0), (255, 0, 0), (0, 200, 40), (30, 60, 255),
       (250, 250, 90), (120, 0, 180), (18, 240, 240), (7, 7, 7)]


def gif_lzw(indices):
    bits, nbits, out = 0, 0, bytearray()

    def put(code):
        nonlocal bits, nbits
        bits |= code << nbits
        nbits += 9
        while nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8
    put(256)
    for i, v in enumerate(indices):
        if i and i % 250 == 0:
            put(256)
        put(v)
    put(257)
    if nbits:
        out.append(bits & 0xFF)
    blocks = bytearray([8])
    o = bytes(out)
    for i in range(0, len(o), 255):
        c = o[i:i + 255]
        blocks.append(len(c))
        blocks += c
    blocks.append(0)
    return bytes(blocks)


def gif(frames, loop=0):
    d = bytearray(b"GIF89a")
    d += struct.pack("<HHBBB", W, H, 0x80 | 0x07, 0, 0)
    for i in range(256):
        d += bytes(PAL[i] if i < len(PAL) else (0, 0, 0))
    d += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", loop) + b"\x00"
    for f in frames:
        pk = (f["disposal"] << 2) | (1 if f["transp"] is not None else 0)
        d += b"\x21\xF9\x04" + bytes([pk]) + struct.pack("<H", f["delay"]) \
             + bytes([f["transp"] if f["transp"] is not None else 0]) + b"\x00"
        d += b"\x2C" + struct.pack("<HHHHB", f["x"], f["y"], f["w"], f["h"], 0)
        d += gif_lzw(f["idx"])
    d += b"\x3B"
    return bytes(d)


def F(**k):
    return dict(dict(x=0, y=0, disposal=1, transp=None, delay=10), **k)


bg = [2 if (x // 5 + y // 3) % 2 == 0 else 1 for y in range(H) for x in range(W)]
write("anim.gif", gif([
    F(w=W, h=H, idx=bg, delay=12),
    F(w=12, h=9, x=4, y=2, idx=[3] * 108, disposal=2, delay=40),
    F(w=12, h=9, x=18, y=8, idx=[4] * 108, disposal=3, delay=7),
    F(w=10, h=6, x=8, y=15, idx=[6 if (i % 3) else 7 for i in range(60)], transp=7, delay=90),
], loop=5))

# --- animated PNG ----------------------------------------------------------
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def idat(px, w, h):
    return zlib.compress(b"".join(b"\0" + px[y * w * 4:(y + 1) * w * 4] for y in range(h)), 9)


def apng(cw, ch, frames, plays=3):
    o = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", cw, ch, 8, 6, 0, 0, 0))
    o += chunk(b"acTL", struct.pack(">II", len(frames), plays))
    seq = 0
    for i, f in enumerate(frames):
        o += chunk(b"fcTL", struct.pack(">IIIIIHHBB", seq, f["w"], f["h"], f["x"], f["y"],
                                        f["num"], f["den"], f["dispose"], f["blend"]))
        seq += 1
        d = idat(f["px"], f["w"], f["h"])
        if i == 0:
            o += chunk(b"IDAT", d)
        else:
            o += chunk(b"fdAT", struct.pack(">I", seq) + d)
            seq += 1
    return o + chunk(b"IEND", b"")


def solid(w, h, c):
    return bytes(c) * (w * h)


AF = lambda **k: dict(dict(x=0, y=0, num=1, den=10, dispose=0, blend=0), **k)
write("anim.apng", apng(W, H, [
    AF(w=W, h=H, px=pattern(W, H, 2).tobytes()),
    AF(w=14, h=10, x=3, y=4, px=solid(14, 10, (255, 200, 0, 255)), dispose=1, num=3, den=20),
    AF(w=14, h=10, x=20, y=12, px=solid(14, 10, (0, 120, 255, 160)), dispose=2, blend=1),
    AF(w=W, h=6, x=0, y=22, px=solid(W, 6, (10, 240, 90, 200)), blend=1, num=1, den=4),
]))

# --- JPEG with EXIF orientation 6 -----------------------------------------
tiff = (b"II\x2a\x00" + struct.pack("<I", 8) + struct.pack("<H", 1)
        + struct.pack("<HHI", 0x0112, 3, 1) + struct.pack("<H", 6) + b"\0\0"
        + struct.pack("<I", 0))
b = io.BytesIO()
pattern(W, H).convert("RGB").save(b, "JPEG", quality=90, exif=b"Exif\0\0" + tiff)
write("rot.jpg", b.getvalue())

print("fixtures written to", OUT)
