#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the release wallpaper: an original, procedurally generated PNG.

Usage: mkwallpaper.py OUT.png [WIDTH HEIGHT]     (default 1920 x 1200)
       mkwallpaper.py --night OUT.png [WIDTH HEIGHT]   (default 1280 x 800)

--night draws the dark twin, 夜色山丘 (design_language.md 5.5, P12): the same
ridge composition under a night sky with a moon glow and sparse stars. The
default (light) output is byte-identical to what this file made before --night
existed; tests/unit/ui_art_check.py pins its sha256. --no-stars (night only)
omits the stars so the test can diff the two renders and prove where they are.

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


# ---- 夜色山丘 (night hills), design 5.5 ---------------------------------------
NIGHT_SKY = [(0.00, (11, 11, 30)), (0.42, (30, 26, 68)), (0.72, (58, 42, 92)),
             (1.00, (58, 42, 92))]                       # #0B0B1E #1E1A44 #3A2A5C
# The three hill colours of the design, back to front; the farthest ridge of the
# light picture has no night colour of its own, so it takes a haze between the
# horizon and the first hill and keeps the composition four-deep.
NIGHT_RIDGE_COLOURS = [(46, 34, 77), (0x22, 0x1A, 0x3D), (0x18, 0x12, 0x2E), (0x0F, 0x0B, 0x1F)]
# At night that haze ridge is lit only by the sky, so where the light picture's two
# slow waves run nearly level (x 0-380 and 1100-1280 at 1280) it read as a flat band.
# Two low-amplitude faster waves (+-17 dev px at 800) keep it a hill across the full
# width without moving the composition's peak.
NIGHT_RIDGE0_EXTRA = [(0.014, 4.3, 1.1), (0.007, 8.2, 2.9)]
MOON = (0.72, 0.22)                      # centre, fraction of width and of height
MOON_GLOW = ((0x8C, 0x7B, 0xFF), 30 / 255.0, 0.30)   # colour, peak strength, radius / width
MOON_HALO = ((0xB8, 0xAE, 0xFF), 70 / 255.0, 0.055)  # tight halo that makes the glow read as a moon
MOON_DISC = ((0xEC, 0xE8, 0xFF), 0.0125)              # colour, radius / width
MOON_EDGE_PX = 1.5        # the disc edge is anti-aliased over this many dev px at 1x
MOON_LIMB = 0.22          # limb darkening: the rim of the disc is this much dimmer than its centre
STAR_TOP_PT, STAR_BOTTOM_PT, STAR_BAND = 32, 712, 0.40   # never above 32 pt, never from 712 pt (Dock band)
STAR_COUNT = 110
STAR_RGB = (0xE6, 0xE8, 0xFF)


def star_list(w, h):
    """Fixed-seed LCG stars: (x, y, size, alpha), sizes 1-2 dev px at 1x (x scale),
    alpha 90..160, only inside the upper 40 % and clear of the 32 pt / 712 pt bands."""
    scale = max(1, w // 1280)
    top, bottom = STAR_TOP_PT * scale, min(STAR_BOTTOM_PT * scale, int(h * STAR_BAND))
    state = 0x4C6F6769                                    # "Logi"
    out = []

    def nxt():
        nonlocal state
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        return state >> 8
    while len(out) < STAR_COUNT:
        x, y, r, a = nxt() % w, nxt() % h, nxt(), nxt()
        size = (1 + (r & 1)) * scale
        if y < top or y + size > bottom or x + size > w:
            continue
        out.append((x, y, size, 90 + a % 71))
    return out


def render_night(w, h, stars=True):
    rows = []
    ridge_y = [[base + sum(a * math.sin(f * 2 * math.pi * (x / w) + p)
                           for a, f, p in (waves + NIGHT_RIDGE0_EXTRA if k == 0 else waves))
                for x in range(w)] for k, (base, waves, _, _) in enumerate(RIDGES)]
    mx, my = MOON[0] * w, MOON[1] * h
    edge = MOON_EDGE_PX * w / 1280.0
    (gc, gs, gr), (hc, hs, hr), (dc, dr) = MOON_GLOW, MOON_HALO, MOON_DISC
    gr, hr, dr = gr * w, hr * w, dr * w
    gx0, gx1 = max(0, int(mx - gr)), min(w, int(mx + gr) + 1)
    n_ridge = len(RIDGES)
    for y in range(h):
        v = y / h
        base = None
        # sky (same smoothstep blend as the light picture)
        for (t0, c0), (t1, c1) in zip(NIGHT_SKY, NIGHT_SKY[1:]):
            if v <= t1:
                t = (v - t0) / (t1 - t0) if t1 > t0 else 1.0
                t = t * t * (3 - 2 * t)
                base = [lerp(c0[i], c1[i], t) for i in range(3)]
                break
        if base is None:
            base = list(NIGHT_SKY[-1][1])
        dy = y - my
        row = bytearray([0])
        for x in range(w):
            c = base
            if gx0 <= x < gx1:
                dx = x - mx
                d2 = dx * dx + dy * dy
                if d2 < gr * gr:
                    t = math.sqrt(d2) / gr
                    g = gs * (1.0 - t * t * (3 - 2 * t))          # smooth to 0 at the radius
                    if d2 < hr * hr * 9:
                        g2 = hs * math.exp(-d2 / (hr * hr))
                    else:
                        g2 = 0.0
                    c = [c[i] + (gc[i] - c[i]) * g for i in range(3)]
                    if g2 > 0.001:
                        c = [c[i] + (hc[i] - c[i]) * g2 for i in range(3)]
                    if d2 < (dr + edge) ** 2:
                        r = math.sqrt(d2)
                        cover = 1.0 - smoothstep(dr - edge, dr + edge, r)
                        q = min(1.0, r / dr)
                        lit = 1.0 - MOON_LIMB * (1.0 - math.sqrt(max(0.0, 1.0 - q * q)))
                        c = [c[i] + (dc[i] * lit - c[i]) * cover * 0.93 for i in range(3)]
            for k in range(n_ridge):
                ry, soft = ridge_y[k][x], RIDGES[k][3]
                if v < ry - soft:
                    continue
                t = smoothstep(ry - soft, ry + soft, v)
                depth = 1.0 - 0.18 * smoothstep(ry, ry + 0.25, v)
                rc = NIGHT_RIDGE_COLOURS[k]
                c = [lerp(c[i], rc[i] * depth, t) for i in range(3)]
            n = ((x * 7 + y * 13) % 16) / 16.0 - 0.47
            row += bytes(max(0, min(255, int(ch + n + 0.5))) for ch in c)
        rows.append(row)
    if stars:
        for sx, sy, size, a in star_list(w, h):
            for yy in range(sy, sy + size):
                row = rows[yy]
                for xx in range(sx, sx + size):
                    o = 1 + xx * 3
                    for i in range(3):
                        row[o + i] = int(row[o + i] + (STAR_RGB[i] - row[o + i]) * a / 255.0 + 0.5)
    return b"".join(bytes(r) for r in rows)


def png(w, h, raw):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv):
    night = "--night" in argv
    stars = "--no-stars" not in argv
    argv = [a for a in argv if a not in ("--night", "--no-stars")]
    if len(argv) not in (2, 4) or (not night and not stars):
        sys.exit(__doc__.split("\n\n")[1])
    dw, dh = (1280, 800) if night else (1920, 1200)
    w, h = (int(argv[2]), int(argv[3])) if len(argv) == 4 else (dw, dh)
    with open(argv[1], "wb") as f:
        f.write(png(w, h, render_night(w, h, stars) if night else render(w, h)))
    print("mkwallpaper: wrote %s (%dx%d)" % (argv[1], w, h))


if __name__ == "__main__":
    main(sys.argv)
