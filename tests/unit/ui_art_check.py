#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""ui-art-assets: probes for the generated app icons and the night wallpaper.

  python3 -I ui_art_check.py --mkicons BIN --wallpaper tools/mkwallpaper.py --out DIR
          [--sheet PNG] [--expect-fail CHECK] [--icon-set DIR] [--negative]

Prints `ok <check>` / `FAIL <check> <detail>` per check and exits non-zero on any
FAIL. Checks (design_language.md 3.7.4, 3.7.5, 5.5):

  determinism   two runs of mkicons give byte-identical files and sheet
  size          every icon is 256 x 256 RGBA 8-bit
  corner        pixel (2,2) is transparent (the squircle, not a square)
  gradient      the centre column at y=8 and y=238 is the tile gradient +-4
  coverage      the glyph covers 15..60 % of its 56 % box
  rim           the 1 dev px top rim (#FFFFFF @ 90) lightens row 0 of every dark tile;
                the edge hairline of a white tile (top luma > 0.85) darkens, and of a
                near-black tile (bottom luma < 0.10) lightens, the side at (1,128), and
                the tile is back to its gradient +-4 by (7,128) (a hairline, not a wash)
  sheet         the contact sheet is two panels, #F4F4F8 and #1E1E24
  wallpaper-size   wallpaper-dark 1280 x 800 and @2x 2560 x 1600
  sky-luma      luma at (640,200) < 50 (and the 2x twin at (1280,400))
  stars         star pixels (render minus the --no-stars render) exist and none is
                at y < 32 pt, at y >= 712 pt, or below the upper 40 %
  light-sha     the default wallpaper is byte-identical to before --night existed

--expect-fail CHECK is the negative control: the run must print FAIL CHECK, no
other FAIL, and exit non-zero (any other outcome is a defect in the control).
--icon-set DIR checks only that DIR holds the 13 icon files (test-app-icons);
with --negative it deletes one in a copy and requires the check to fail.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

STEMS = ["files", "settings", "browser", "clock", "monitor", "preview", "studio",
         "assistant", "terminal", "textedit", "gallery", "widgets", "logit"]
# (top, bottom, light tile?, brand gradient?) from c/include/ui/logit_apps.h, 3.7.4 / 3.1.6.
TILES = {
    "files": (0x62C6FF, 0x1E7CF2, 0, 0), "settings": (0x9A9AA6, 0x4A4A55, 0, 0),
    "browser": (0x55D3FF, 0x3360FF, 0, 0), "clock": (0xFFFFFF, 0xE6E6EC, 1, 0),
    "monitor": (0x2E2E36, 0x101015, 0, 0), "preview": (0xFFFFFF, 0xECEEF4, 1, 0),
    "studio": (0xA98BFF, 0x6A2BD9, 0, 0), "assistant": (0x7C5CFF, 0x38A8FF, 0, 1),
    "terminal": (0x2C2C34, 0x131318, 0, 0), "textedit": (0xFFFFFF, 0xF0F0F4, 1, 0),
    "gallery": (0xFF9F0A, 0xFF375F, 0, 0), "widgets": (0x34C7A5, 0x2280F0, 0, 0),
    "logit": (0x7C5CFF, 0x38A8FF, 0, 1),
}
BRAND = [(0.0, 0x7C5CFF), (0.55, 0x4F7BFF), (1.0, 0x38A8FF)]
LIGHT_WALLPAPER_SHA = "c9803c41241ce9ee78124bd803ee66b0a0ccfd483fd11ccf97683926f223cce9"  # 1920x1200, pre --night

fails = []


def ok(check, detail=""):
    print("ok %s %s" % (check, detail) if detail else "ok %s" % check)


def fail(check, detail):
    fails.append(check)
    print("FAIL %s %s" % (check, detail))


def decode_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: " + path)
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if zlib.crc32(kind + body) & 0xFFFFFFFF != struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0]:
            raise ValueError("bad chunk CRC in " + path)
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _, _, _ = ihdr
    if depth != 8 or ctype not in (2, 6):
        raise ValueError("unsupported PNG format %r in %s" % (ihdr, path))
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride): row[i] = (row[i] + row[i - bpp]) & 255
        elif f == 2:
            for i in range(stride): row[i] = (row[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride): row[i] = (row[i] + (((row[i - bpp] if i >= bpp else 0) + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = row[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        elif f != 0:
            raise ValueError("bad filter %d" % f)
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return w, h, bpp, bytes(out)


def px(img, x, y):
    w, h, bpp, d = img
    o = (y * w + x) * bpp
    return tuple(d[o:o + bpp])


def lerp_rgb(c0, c1, t):
    return tuple(((c0 >> s) & 255) + (((c1 >> s) & 255) - ((c0 >> s) & 255)) * t for s in (16, 8, 0))


def expected(stem, x, y):
    top, bottom, _, brand = TILES[stem]
    if brand:
        t = (x + y + 1) / 512.0
        for (t0, c0), (t1, c1) in zip(BRAND, BRAND[1:]):
            if t <= t1:
                return lerp_rgb(c0, c1, (t - t0) / (t1 - t0))
        return lerp_rgb(BRAND[-1][1], BRAND[-1][1], 0)
    return lerp_rgb(top, bottom, (y + 0.5) / 256.0)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("FAIL tool %s exited %d" % (cmd[0], r.returncode))
    return r.stdout


def luma(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def luma601(rgb):
    return (0.299 * ((rgb >> 16) & 255) + 0.587 * ((rgb >> 8) & 255) + 0.114 * (rgb & 255)) / 255.0


def check_icons(args, work):
    d1, d2 = os.path.join(work, "a"), os.path.join(work, "b")
    os.makedirs(d1), os.makedirs(d2)
    s1, s2 = os.path.join(work, "sheet1.png"), os.path.join(work, "sheet2.png")
    run([args.mkicons, "--sheet", s1, d1])
    run([args.mkicons, "--sheet", s2, d2])
    same = all(open(os.path.join(d1, s + ".png"), "rb").read() == open(os.path.join(d2, s + ".png"), "rb").read()
               for s in STEMS) and open(s1, "rb").read() == open(s2, "rb").read()
    (ok if same else lambda c, d="": fail(c, "two runs differ"))("determinism")
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        for s in STEMS:
            shutil.copyfile(os.path.join(d1, s + ".png"), os.path.join(args.out, s + ".png"))
    if args.sheet:
        os.makedirs(os.path.dirname(os.path.abspath(args.sheet)), exist_ok=True)
        shutil.copyfile(s1, args.sheet)
    for stem in STEMS:
        try:
            img = decode_png(os.path.join(d1, stem + ".png"))
        except (ValueError, OSError, zlib.error) as e:
            fail("size", "%s: %s" % (stem, e)); continue
        w, h, bpp, _ = img
        if (w, h, bpp) != (256, 256, 4):
            fail("size", "%s is %dx%d bpp %d" % (stem, w, h, bpp)); continue
        ok("size", stem)
        c = px(img, 2, 2)
        (ok("corner", stem) if c[3] == 0 else fail("corner", "%s alpha at (2,2) = %d" % (stem, c[3])))
        bad = []
        for y in (8, 238):
            e, g = expected(stem, 128, y), px(img, 128, y)
            if g[3] != 255 or any(abs(g[i] - e[i]) > 4 for i in range(3)):
                bad.append("y=%d got %s want %s" % (y, g, tuple(round(v) for v in e)))
        (fail("gradient", "%s %s" % (stem, "; ".join(bad))) if bad else ok("gradient", stem))
        # glyph coverage: pixels of the 56 % box that are not the tile gradient
        lo = int((256 - 143.36) / 2) + 1
        hi = int((256 + 143.36) / 2) - 1
        n = cnt = 0
        for y in range(lo, hi):
            for x in range(lo, hi):
                n += 1
                e, g = expected(stem, x, y), px(img, x, y)
                if max(abs(g[i] - e[i]) for i in range(3)) > 24:
                    cnt += 1
        cov = 100.0 * cnt / n
        (ok("coverage", "%s %.1f%%" % (stem, cov)) if 15.0 <= cov <= 60.0 else fail("coverage", "%s %.1f%% outside 15..60" % (stem, cov)))
        top, _, light, brand = TILES[stem]
        if not light:
            g = px(img, 128, 0)
            e = expected(stem, 128, 0)
            lift = sum(g[i] - e[i] for i in range(3)) / 3.0
            (ok("rim", "%s +%.0f" % (stem, lift)) if lift >= 15 else fail("rim", "%s row 0 lifted only %.1f (want >= 15)" % (stem, lift)))
        # the edge hairline (mkicons draw_edge): white and near-black tiles only
        _, bottom, _, _ = TILES[stem]
        kind = 0 if brand else (-1 if luma601(top) > 0.85 else (1 if luma601(bottom) < 0.10 else 0))
        if kind:
            g, e = px(img, 1, 128), expected(stem, 1, 128)
            shift = sum(g[i] - e[i] for i in range(3)) / 3.0 * kind
            g7, e7 = px(img, 7, 128), expected(stem, 7, 128)
            inner = max(abs(g7[i] - e7[i]) for i in range(3))
            if shift < 12:
                fail("rim", "%s edge at (1,128) moved only %.1f toward the %s (want >= 12)" % (stem, shift, "dark" if kind < 0 else "light"))
            elif inner > 4:
                fail("rim", "%s edge reaches (7,128): off the gradient by %d" % (stem, inner))
            else:
                ok("rim", "%s edge %+.0f" % (stem, shift * kind))
    # contact sheet
    try:
        sh = decode_png(s1)
        w, h = sh[0], sh[1]
        good = px(sh, 0, 0)[:3] == (0xF4, 0xF4, 0xF8) and px(sh, w - 1, h - 1)[:3] == (0x1E, 0x1E, 0x24) \
            and px(sh, w // 2 - 1, 0)[:3] == (0xF4, 0xF4, 0xF8) and px(sh, w // 2, 0)[:3] == (0x1E, 0x1E, 0x24)
        (ok("sheet", "%dx%d" % (w, h)) if good else fail("sheet", "panel colours wrong"))
    except (ValueError, OSError, zlib.error) as e:
        fail("sheet", str(e))


def check_wallpaper(args, work):
    mk = [sys.executable, "-I", args.wallpaper]
    light = os.path.join(work, "light.png")
    run(mk + [light])
    sha = hashlib.sha256(open(light, "rb").read()).hexdigest()
    (ok("light-sha") if sha == LIGHT_WALLPAPER_SHA else fail("light-sha", "%s != %s" % (sha, LIGHT_WALLPAPER_SHA)))
    sizes = ((1280, 800, 1), (2560, 1600, 2))
    for w, h, k in sizes:
        name = "wallpaper-dark.png" if k == 1 else "wallpaper-dark@2x.png"
        p, q = os.path.join(work, name), os.path.join(work, "nostars" + name)
        run(mk + ["--night", p, str(w), str(h)])
        run(mk + ["--night", "--no-stars", q, str(w), str(h)])
        if k == 1:
            p2 = os.path.join(work, "again.png")
            run(mk + ["--night", p2, str(w), str(h)])
            (ok("determinism", "wallpaper") if open(p, "rb").read() == open(p2, "rb").read() else fail("determinism", "night wallpaper differs between runs"))
        a, b = decode_png(p), decode_png(q)
        if (a[0], a[1]) != (w, h):
            fail("wallpaper-size", "%s is %dx%d" % (name, a[0], a[1])); continue
        ok("wallpaper-size", "%s %dx%d" % (name, w, h))
        s = px(a, 640 * k, 200 * k)
        (ok("sky-luma", "%s %.1f" % (name, luma(s))) if luma(s) < 50 else fail("sky-luma", "%s luma %.1f >= 50" % (name, luma(s))))
        stars, bad = 0, []
        for y in range(h):
            ra, rb = a[3][y * w * 3:(y + 1) * w * 3], b[3][y * w * 3:(y + 1) * w * 3]
            if ra == rb:
                continue
            for x in range(w):
                if ra[x * 3:x * 3 + 3] != rb[x * 3:x * 3 + 3]:
                    stars += 1
                    if y < 32 * k or y >= 712 * k or y >= int(h * 0.40):
                        bad.append((x, y))
        if stars < 20:
            fail("stars", "%s has only %d star pixels" % (name, stars))
        elif bad:
            fail("stars", "%s: %d star pixels in a forbidden band, first %s" % (name, len(bad), bad[0]))
        else:
            ok("stars", "%s %d px" % (name, stars))
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            shutil.copyfile(p, os.path.join(args.out, name))


def check_icon_set(d, negative):
    miss = [s for s in STEMS if not os.path.isfile(os.path.join(d, s + ".png"))]
    if negative:
        if miss:
            sys.exit("FAIL control: the set is already incomplete: %s" % miss)
        with tempfile.TemporaryDirectory(prefix="ui-art-set-") as t:
            for s in STEMS[1:]:
                shutil.copyfile(os.path.join(d, s + ".png"), os.path.join(t, s + ".png"))
            miss = [s for s in STEMS if not os.path.isfile(os.path.join(t, s + ".png"))]
            d = t
        if not miss:
            sys.exit("FAIL control: a missing icon went unnoticed")
        print("PASS negative control: missing %s is refused" % miss)
        return
    for s in STEMS:
        p = os.path.join(d, s + ".png")
        if s in miss:
            fail("icon-set", "%s.png is missing from %s" % (s, d)); continue
        w, h, bpp, _ = decode_png(p)
        if (w, h, bpp) != (256, 256, 4):
            fail("icon-set", "%s.png is %dx%d bpp %d" % (s, w, h, bpp))
    if not fails:
        ok("icon-set", "%d files" % len(STEMS))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mkicons"), ap.add_argument("--wallpaper"), ap.add_argument("--out"), ap.add_argument("--sheet")
    ap.add_argument("--expect-fail"), ap.add_argument("--icon-set"), ap.add_argument("--negative", action="store_true")
    args = ap.parse_args()
    if args.icon_set:
        check_icon_set(args.icon_set, args.negative)
        return 1 if fails else 0
    if not (args.mkicons and args.wallpaper):
        ap.error("--mkicons and --wallpaper are required")
    with tempfile.TemporaryDirectory(prefix="ui-art-") as work:
        check_icons(args, work)
        check_wallpaper(args, work)
    if args.expect_fail:
        want = args.expect_fail
        if want not in fails:
            sys.exit("FAIL control: expected FAIL %s, got %s" % (want, sorted(set(fails)) or "no failure"))
        if set(fails) != {want}:
            sys.exit("FAIL control: failed for other reasons too: %s" % sorted(set(fails)))
        print("PASS negative control: FAIL %s and nothing else" % want)
        return 0
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
