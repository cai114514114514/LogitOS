#!/usr/bin/env python3
"""Generate the rich-terminal pixel test's fixture image.

Generated rather than committed because the repository does not track PNGs
(.gitignore: *.png -- the wallpaper is regenerated the same way). It is a solid
rectangle in a colour nothing else in the UI uses, which is exactly what makes
"the image is on screen at the size it was asked for" answerable by an exact
colour search over the composited frame: a gradient or a photo would blend at
the edges and turn a size assertion into a tolerance.

Usage: dot_gen.py <out.png> [w] [h]
"""

import struct
import sys
import zlib

out = sys.argv[1]
W = int(sys.argv[2]) if len(sys.argv) > 2 else 60
H = int(sys.argv[3]) if len(sys.argv) > 3 else 40
RGB = (0xFF, 0x00, 0xE5)

raw = b"".join(b"\x00" + bytes(RGB) * W for _ in range(H))


def chunk(tag, data):
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 9))
       + chunk(b"IEND", b""))
with open(out, "wb") as fh:
    fh.write(png)
print("dot_gen: %s  %dx%d rgb%s" % (out, W, H, RGB))
