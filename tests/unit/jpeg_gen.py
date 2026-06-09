#!/usr/bin/env python3
"""Generate baseline JPEG test cases for the from-scratch decoder (src/lib/image/jpeg.c).

JPEG is LOSSY, so we never compare our decoder against the original pixels. Instead
PIL encodes a baseline JPEG, and libjpeg `djpeg` decodes the IDENTICAL bytes as the
reference. Both djpeg (default -dct int = "islow") and our decoder use the same
integer IDCT, and we run djpeg with -nosmooth (box chroma upsampling) so it matches
our nearest-neighbour chroma upsample -> the two outputs differ only by colour-
conversion rounding (<= a couple LSB per channel).

For each case writes <name>.jpg + <name>.ref (reference straight RGBA8) + a manifest
line "<name> W H mode" where mode in {ok, fail}. fail-mode cases (progressive, CMYK)
must be REJECTED by our decoder (return -1), proving graceful handling.

Usage: jpeg_gen.py <outdir>"""
import os, sys, subprocess, shutil
from PIL import Image

OUT = sys.argv[1]; os.makedirs(OUT, exist_ok=True)
DJPEG = shutil.which("djpeg") or "/opt/homebrew/bin/djpeg"
manifest = []


def read_pnm(data):
    """Parse a binary PPM (P6) or PGM (P5) into (w, h, rgba-bytes)."""
    assert data[:2] in (b"P6", b"P5"), "djpeg did not emit P6/P5"
    gray = data[:2] == b"P5"
    # header: magic, width, height, maxval -- whitespace separated, '#' comments
    fields, i, n = [], 2, len(data)
    while len(fields) < 3:
        while i < n and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":
            while i < n and data[i:i+1] != b"\n":
                i += 1
            continue
        start = i
        while i < n and not data[i:i+1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    w, h, maxval = fields
    assert maxval == 255
    i += 1  # single whitespace after maxval
    px = data[i:]
    rgba = bytearray()
    if gray:
        for k in range(w * h):
            g = px[k]; rgba += bytes([g, g, g, 255])
    else:
        for k in range(w * h):
            rgba += bytes([px[k*3], px[k*3+1], px[k*3+2], 255])
    return w, h, bytes(rgba)


def djpeg_ref(jpg_path):
    out = subprocess.run([DJPEG, "-nosmooth", "-dct", "int", "-pnm", jpg_path],
                         capture_output=True, check=True).stdout
    return read_pnm(out)


def grad(mode, W, H):
    im = Image.new(mode, (W, H)); px = im.load()
    for y in range(H):
        for x in range(W):
            if mode == "L":
                px[x, y] = (x * 11 + y * 7) & 0xff
            elif mode == "RGB":
                # gradients + a sharp diagonal edge to exercise AC coefficients
                r = (x * 9) & 0xff
                g = (y * 13) & 0xff
                b = (255 if ((x ^ y) & 8) else 30)
                px[x, y] = (r, g, b)
            elif mode == "CMYK":
                px[x, y] = ((x * 7) & 0xff, (y * 5) & 0xff, ((x + y) * 3) & 0xff, 40)
    return im


def case_ok(name, im, **save):
    jpg = os.path.join(OUT, name + ".jpg")
    im.save(jpg, "JPEG", optimize=False, progressive=False, **save)
    w, h, ref = djpeg_ref(jpg)
    open(os.path.join(OUT, name + ".ref"), "wb").write(ref)
    manifest.append(f"{name} {w} {h} ok")
    print(f"  {name:16s} {w}x{h}  ok (ref via djpeg -nosmooth)")


def case_fail(name, jpg_bytes, W, H):
    """A case our decoder must REJECT (no .ref needed)."""
    open(os.path.join(OUT, name + ".jpg"), "wb").write(jpg_bytes)
    manifest.append(f"{name} {W} {H} fail")
    print(f"  {name:16s} {W}x{H}  fail-expected")


# Odd dims stress MCU-edge handling (image not a whole number of MCUs).
W, H = 23, 17

# Grayscale baseline (single component).
case_ok("gray", grad("L", W, H), quality=90)

# Colour, three subsampling modes. PIL: subsampling 0=4:4:4, 1=4:2:2, 2=4:2:0.
case_ok("rgb_444", grad("RGB", W, H), quality=92, subsampling=0)
case_ok("rgb_422", grad("RGB", W, H), quality=92, subsampling=1)
case_ok("rgb_420", grad("RGB", W, H), quality=92, subsampling=2)

# A larger 4:2:0 with restart intervals (DRI/RSTn path) -- PIL emits restart markers.
big = grad("RGB", 64, 48)
jpg = os.path.join(OUT, "rgb_420_rst.jpg")
big.save(jpg, "JPEG", quality=90, subsampling=2, restart_marker_blocks=4,
         optimize=False, progressive=False)
w, h, ref = djpeg_ref(jpg)
open(os.path.join(OUT, "rgb_420_rst.ref"), "wb").write(ref)
manifest.append(f"rgb_420_rst {w} {h} ok")
print(f"  {'rgb_420_rst':16s} {w}x{h}  ok (4:2:0 + restart markers)")

# Square 4:4:4 (whole-MCU, simplest path) for a clean baseline.
case_ok("rgb_444_sq", grad("RGB", 32, 32), quality=95, subsampling=0)

# --- graceful-rejection cases ---
# Progressive JPEG (SOF2): must be rejected.
import io
buf = io.BytesIO()
grad("RGB", W, H).save(buf, "JPEG", quality=90, progressive=True, subsampling=0)
case_fail("rgb_prog", buf.getvalue(), W, H)

# CMYK / 4-component: must be rejected.
buf = io.BytesIO()
grad("CMYK", W, H).save(buf, "JPEG", quality=90)
case_fail("cmyk", buf.getvalue(), W, H)

open(os.path.join(OUT, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
print(f"generated {len(manifest)} cases")
