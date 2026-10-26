#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the release wallpaper: an original, procedurally generated PNG.

Usage: mkwallpaper.py OUT.png [WIDTH HEIGHT]     (default 1920 x 1200)

Why it exists (2026-10-26): the only wallpaper the tree knew how to make was
tools/mkwallpaper.sh, which converts a proprietary macOS Desktop Picture --
RELEASING.md section 3 says that file must be replaced before any release, and
on a host without it the desktop fell back to wm.c's flat bg_color() gradient,
which a desktop tour read as unfinished. This picture is pure arithmetic: a
dusk sky and four layers of soft ridges, so its source, its licence (MIT, like
this file) and its bytes are all in the repository's hands.

Stdlib only (zlib + struct), so the build needs no imaging package. The ±0.5
ordered noise before rounding is deliberate: an 8-bit gradient this smooth
bands visibly at 1920 px without it.
"""
import math
import struct
import sys
import zlib

# Sky stops, top to bottom: night blue, violet, rose, warm sand.
SKY = [(0.00, (30, 38, 84)), (0.38, (86, 78, 148)),
       (0.70, (206, 138, 140)), (1.00, (242, 196, 160))]
# Ridges, back to front: (baseline, [(amplitude, frequency, phase)], colour,
# edge softness). Lower and darker as they come forward -- aerial perspective.
RIDGES = [
    (0.58, [(0.040, 1.3, 0.4), (0.018, 3.1, 1.9)], (150, 112, 160), 0.010),
    (0.68, [(0.050, 0.9, 2.2), (0.020, 2.7, 0.3)], (104, 80, 138), 0.006),
    (0.79, [(0.045, 1.1, 4.1), (0.015, 3.9, 2.6)], (66, 54, 108), 0.004),
    (0.90, [(0.035, 0.7, 1.2), (0.012, 4.6, 5.0)], (38, 34, 74), 0.003),
]
GLOW = (0.68, 0.60, 0.42, (255, 214, 176), 0.35)   # cx, cy, radius, colour, strength


def lerp(a, b, t):
    return a + (b - a) * t


def sky(v):
    for (t0, c0), (t1, c1) in zip(SKY, SKY[1:]):
        if v <= t1:
            t = (v - t0) / (t1 - t0)
            t = t * t * (3 - 2 * t)
            return [lerp(c0[i], c1[i], t) for i in range(3)]
    return list(SKY[-1][1])


def smoothstep(e0, e1, x):
    t = min(1.0, max(0.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def render(w, h):
    rows = []
    ridge_y = [[base + sum(a * math.sin(f * 2 * math.pi * (x / w) + p) for a, f, p in waves)
                for x in range(w)] for base, waves, _, _ in RIDGES]
    gx, gy, gr, gc, gs = GLOW
    aspect = w / h
    for y in range(h):
        v = y / h
        base = sky(v)
        row = bytearray([0])                          # PNG filter type 0
        for x in range(w):
            u = x / w
            c = list(base)
            d = math.hypot((u - gx) * aspect, v - gy) / gr
            g = gs * math.exp(-d * d)
            c = [lerp(c[i], gc[i], g) for i in range(3)]
            for k, (_, _, rc, soft) in enumerate(RIDGES):
                t = smoothstep(ridge_y[k][x] - soft, ridge_y[k][x] + soft, v)
                if t:
                    # each ridge fades darker toward its foot: mist sits in the valleys
                    depth = 1.0 - 0.18 * smoothstep(ridge_y[k][x], ridge_y[k][x] + 0.25, v)
                    c = [lerp(c[i], rc[i] * depth, t) for i in range(3)]
            n = ((x * 7 + y * 13) % 16) / 16.0 - 0.47   # ordered dither, see docstring
            row += bytes(max(0, min(255, int(ch + n + 0.5))) for ch in c)
        rows.append(bytes(row))
    return b"".join(rows)


def png(w, h, raw):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv):
    if len(argv) not in (2, 4):
        sys.exit(__doc__.split("\n\n")[1])
    w, h = (int(argv[2]), int(argv[3])) if len(argv) == 4 else (1920, 1200)
    with open(argv[1], "wb") as f:
        f.write(png(w, h, render(w, h)))
    print("mkwallpaper: wrote %s (%dx%d)" % (argv[1], w, h))


if __name__ == "__main__":
    main(sys.argv)
