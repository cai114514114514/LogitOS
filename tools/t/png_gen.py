#!/usr/bin/env python3
"""Generate a matrix of PNG test cases (colour types, bit depths, interlace,
tRNS) with PIL as the ground-truth encoder/decoder. For each case writes
<name>.png + <name>.rgba (expected straight RGBA8) + a manifest line.
Usage: png_gen.py <outdir>"""
import os, sys, struct
from PIL import Image

OUT = sys.argv[1]; os.makedirs(OUT, exist_ok=True)
W, Hh = 23, 17                      # odd dims -> sub-byte stride padding + varied Adam7 passes
manifest = []

def ihdr(png):                     # (depth, ctype, interlace) from the file
    d = open(png, "rb").read()
    o = d.index(b"IHDR") + 4
    return d[o+8], d[o+9], d[o+12]

def case(name, im, expected=None, **save):
    png = os.path.join(OUT, name + ".png")
    im.save(png, "PNG", **save)
    if expected is None:
        expected = Image.open(png).convert("RGBA").tobytes()   # PIL decodes the real file
    open(os.path.join(OUT, name + ".rgba"), "wb").write(expected)
    manifest.append(f"{name} {im.width} {im.height}")
    dep, ct, il = ihdr(png)
    print(f"  {name:14s} {im.width}x{im.height}  depth={dep} ctype={ct} interlace={il}")

def grad(mode):
    im = Image.new(mode, (W, Hh)); px = im.load()
    for y in range(Hh):
        for x in range(W):
            if   mode == "L":    px[x, y] = (x*11 + y*7) & 0xff
            elif mode == "LA":   px[x, y] = ((x*9) & 0xff, (y*15) & 0xff)
            elif mode == "RGB":  px[x, y] = ((x*11) & 0xff, (y*13) & 0xff, ((x+y)*7) & 0xff)
            elif mode == "RGBA": px[x, y] = ((x*11) & 0xff, (y*13) & 0xff, ((x*y)*3) & 0xff, (x*7+y*5) & 0xff)
            elif mode == "1":    px[x, y] = 1 if ((x + y) & 1) else 0
    return im

case("gray8",   grad("L"))
case("graya8",  grad("LA"))
case("rgb8",    grad("RGB"))
case("rgba8",   grad("RGBA"))
case("bilevel1", grad("1"))                                    # 1-bit grayscale

# PIL won't write Adam7, so hand-roll an interlaced RGBA8 PNG (filter None) with
# the same pattern as rgba8; expected output is identical (interlace == layout only).
import zlib
def _chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
def rgba_px(x, y): return ((x*11) & 0xff, (y*13) & 0xff, ((x*y)*3) & 0xff, (x*7+y*5) & 0xff)
ox, oy = [0,4,0,2,0,1,0], [0,0,4,0,2,0,1]
sx, sy = [8,8,4,4,2,2,1], [8,8,8,4,4,2,2]
raw = bytearray()
for p in range(7):
    cols = (W-ox[p]+sx[p]-1)//sx[p] if W > ox[p] else 0
    rows = (Hh-oy[p]+sy[p]-1)//sy[p] if Hh > oy[p] else 0
    for r in range(rows):
        raw.append(0)                                          # filter: None
        for c in range(cols):
            raw += bytes(rgba_px(ox[p]+c*sx[p], oy[p]+r*sy[p]))
png = bytes.fromhex("89504e470d0a1a0a") + _chunk(b"IHDR", struct.pack(">IIBBBBB", W, Hh, 8, 6, 0, 0, 1)) \
      + _chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + _chunk(b"IEND", b"")
open(os.path.join(OUT, "rgba8_adam7.png"), "wb").write(png)
exp = bytearray()
for y in range(Hh):
    for x in range(W): exp += bytes(rgba_px(x, y))
open(os.path.join(OUT, "rgba8_adam7.rgba"), "wb").write(exp)
manifest.append(f"rgba8_adam7 {W} {Hh}")
print(f"  {'rgba8_adam7':14s} {W}x{Hh}  depth=8 ctype=6 interlace=1 (hand-rolled Adam7)")

# 16-bit grayscale: our decoder reduces 16->8 via >>8, so build expected to match.
im16 = Image.new("I;16", (W, Hh)); px = im16.load(); exp = bytearray()
for y in range(Hh):
    for x in range(W):
        v = (x*1234 + y*4321) & 0xffff; g = v >> 8; exp += bytes([g, g, g, 255]); px[x, y] = v
case("gray16", im16, bytes(exp))

# palette + per-index alpha tRNS
imp = Image.new("P", (W, Hh)); pal = []
for i in range(256): pal += [(i*7) & 0xff, (i*5) & 0xff, (i*3) & 0xff]
imp.putpalette(pal); px = imp.load()
for y in range(Hh):
    for x in range(W): px[x, y] = (x*3 + y*2) & 0xff
case("pal8", imp)
case("pal8_trns", imp, transparency=bytes(range(48)))          # first 48 indices: varied alpha

# colour-key tRNS for gray + rgb
case("gray8_key", grad("L"),   transparency=50)
case("rgb8_key",  grad("RGB"), transparency=(11, 13, 7))

open(os.path.join(OUT, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
print(f"generated {len(manifest)} cases")
