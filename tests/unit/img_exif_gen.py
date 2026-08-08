#!/usr/bin/env python3
"""Ground truth for EXIF orientation.

A phone stores its pixels in the sensor's frame and records how to turn them
upright. All eight values are real: 1 upright, 2 mirrored, 3 rotated 180,
4 flipped, 5 transposed, 6 rotated 90 CW, 7 transversed, 8 rotated 90 CCW.
Half of them swap width and height, which is why the source image here is
deliberately not square and not symmetric -- a decoder that applies the wrong
member of a rotate/mirror pair produces a plausible picture of the wrong thing.

Two kinds of case, because the reference has to be exact:

  * LOSSLESS carriers (PNG `eXIf` chunk, WebP `EXIF` chunk). The reference is
    PIL's ImageOps.exif_transpose of the identical file, and the comparison is
    byte-for-byte. This is what actually proves the eight transforms.

  * JPEG APP1, the carrier that matters in practice. JPEG is lossy, so the
    pixels are not compared against another decoder; what is asserted is the
    ORIENTATION VALUE our parser recovers (an integer, exactly right or
    exactly wrong) and the resulting dimensions. The pixel transform itself is
    already pinned by the lossless cases above -- it is the same code.

Also covers a big-endian ("MM") TIFF header and a file with no EXIF at all,
which must come back as orientation 1 and untouched pixels.

Usage: img_exif_gen.py <outdir>
"""
import os, struct, sys, io, zlib
from PIL import Image, ImageOps

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
manifest = []
W, H = 21, 13                       # non-square and odd, so a swap is obvious


def tiff_block(orientation, big=False):
    """A minimal TIFF/EXIF block carrying just tag 0x0112."""
    if big:
        hdr = b"MM\x00\x2a" + struct.pack(">I", 8)
        ifd = struct.pack(">H", 1) + struct.pack(">HHI", 0x0112, 3, 1) \
            + struct.pack(">H", orientation) + b"\0\0" + struct.pack(">I", 0)
    else:
        hdr = b"II\x2a\x00" + struct.pack("<I", 8)
        ifd = struct.pack("<H", 1) + struct.pack("<HHI", 0x0112, 3, 1) \
            + struct.pack("<H", orientation) + b"\0\0" + struct.pack("<I", 0)
    return hdr + ifd


def source():
    im = Image.new("RGBA", (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = ((x * 12 + 3) & 0xFF, (y * 19 + 7) & 0xFF,
                        ((x * y) * 5 + 11) & 0xFF, 255 - ((x + y) & 0x3F))
    # unique corner markers, so a mirror is not mistakeable for a rotation
    px[0, 0] = (255, 0, 0, 255)
    px[W - 1, 0] = (0, 255, 0, 255)
    px[0, H - 1] = (0, 0, 255, 255)
    px[W - 1, H - 1] = (255, 255, 0, 255)
    return im


SRC = source()


def png_with_exif(orientation, big=False):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    b = io.BytesIO()
    SRC.save(b, "PNG")
    raw = b.getvalue()
    if orientation == 0:
        return raw
    # splice an eXIf chunk in right after IHDR
    i = raw.index(b"IHDR") - 4
    ln = struct.unpack(">I", raw[i:i + 4])[0]
    cut = i + 12 + ln
    return raw[:cut] + chunk(b"eXIf", tiff_block(orientation, big)) + raw[cut:]


def webp_with_exif(orientation):
    b = io.BytesIO()
    SRC.save(b, "WEBP", lossless=True, quality=100, exif=tiff_block(orientation))
    return b.getvalue()


def jpeg_with_exif(orientation):
    # PIL hands the `exif` bytes to the APP1 segment verbatim -- it does NOT add
    # the "Exif\0\0" identifier the JPEG spec requires, and without it PIL
    # cannot read its own file back either. Supply the identifier here; the
    # decoder under test requires it, as it should.
    b = io.BytesIO()
    SRC.convert("RGB").save(b, "JPEG", quality=92,
                            exif=b"Exif\0\0" + tiff_block(orientation))
    return b.getvalue()


def emit(name, ext, data, orientation, expected):
    open(os.path.join(OUT, f"{name}.{ext}"), "wb").write(data)
    mode = "exact"
    if expected is None:
        mode = "tagonly"
        ow, oh = (H, W) if orientation >= 5 else (W, H)
    else:
        open(os.path.join(OUT, f"{name}.rgba"), "wb").write(expected[0])
        ow, oh = expected[1]
    manifest.append(f"{name} {ext} {orientation} {ow} {oh} {mode}")
    print(f"  {name:20s} {ext:4s} orient={orientation} -> {ow}x{oh}  [{mode}]")


for o in range(1, 9):
    data = png_with_exif(o)
    ref = ImageOps.exif_transpose(Image.open(io.BytesIO(data)))
    assert Image.open(io.BytesIO(data)).getexif().get(0x0112, 1) == o, o
    emit(f"png_o{o}", "png", data, o, (ref.convert("RGBA").tobytes(), ref.size))

# big-endian TIFF header: the same answer through the other byte order.
data = png_with_exif(6, big=True)
ref = ImageOps.exif_transpose(Image.open(io.BytesIO(data)))
assert Image.open(io.BytesIO(data)).getexif().get(0x0112, 1) == 6
emit("png_o6_be", "png", data, 6, (ref.convert("RGBA").tobytes(), ref.size))

# no EXIF at all: orientation 1, pixels untouched.
data = png_with_exif(0)
emit("png_noexif", "png", data, 1, (SRC.tobytes(), SRC.size))

for o in (1, 3, 6, 8):
    data = webp_with_exif(o)
    got = Image.open(io.BytesIO(data)).getexif().get(0x0112, 1)
    assert got == o, (o, got)
    ref = ImageOps.exif_transpose(Image.open(io.BytesIO(data)))
    emit(f"webp_o{o}", "webp", data, o, (ref.convert("RGBA").tobytes(), ref.size))

for o in range(1, 9):
    data = jpeg_with_exif(o)
    got = Image.open(io.BytesIO(data)).getexif().get(0x0112, 1)
    assert got == o, (o, got)
    emit(f"jpeg_o{o}", "jpg", data, o, None)

open(os.path.join(OUT, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
print(f"generated {len(manifest)} cases")
