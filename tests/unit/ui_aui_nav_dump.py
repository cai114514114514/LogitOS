#!/usr/bin/env python3
"""ui_aui_nav_dump.py DIR FONTDIR -- finish ui_aui_nav_test --dump's review
scenes: draw each <name>.txt text sidecar (the TEXT_RUN2 runs the harness did
NOT box-draw) onto <name>.ppm with the real ui*.ttf faces, and save
<name>.jpg (quality 88). Review output only: no verdict, exits 0 unless an
input is missing or unreadable.

Sidecar line: T x y px weight rrggbb alpha clip_on cx0 cy0 cx1 cy1 hexutf8,
x/y the run's top-left and px its size, all device pixels. The kernel puts the
baseline at y + ascent (ui*.ttf: 1160/1000 em), which is what is drawn here.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FACES = {400: "ui.ttf", 500: "ui-medium.ttf", 600: "ui-semibold.ttf", 700: "ui-bold.ttf"}


def face_for(weight):
    w = min(FACES, key=lambda k: (abs(k - weight), -k))
    return FACES[w]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    d, fonts = sys.argv[1], sys.argv[2]
    cache = {}
    names = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".ppm"))
    if not names:
        print("no .ppm in", d)
        return 1
    for n in names:
        img = Image.open(os.path.join(d, n + ".ppm")).convert("RGBA")
        side = os.path.join(d, n + ".txt")
        lines = open(side, encoding="ascii").read().splitlines() if os.path.exists(side) else []
        for ln in lines:
            p = ln.split(" ")
            if len(p) < 13 or p[0] != "T":
                continue
            x, y, px = float(p[1]), float(p[2]), float(p[3])
            weight, rgb, alpha = int(p[4]), int(p[5], 16), int(p[6])
            clip = [int(v) for v in p[7:12]]
            text = bytes.fromhex(p[12]).decode("utf-8", "replace") if p[12] else ""
            if not text.strip() or alpha <= 0:
                continue
            key = (face_for(weight), round(px * 4) / 4)
            if key not in cache:
                cache[key] = ImageFont.truetype(os.path.join(fonts, key[0]), size=max(1, int(round(px))))
            layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
            ImageDraw.Draw(layer).text((x, y + 1.160 * px), text, font=cache[key], anchor="ls",
                                       fill=((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha))
            if clip[0]:
                mask = Image.new("L", img.size, 0)
                ImageDraw.Draw(mask).rectangle([clip[1], clip[2], clip[3] - 1, clip[4] - 1], fill=255)
                layer.putalpha(Image.composite(layer.getchannel("A"), Image.new("L", img.size, 0), mask))
            img = Image.alpha_composite(img, layer)
        img.convert("RGB").save(os.path.join(d, n + ".jpg"), quality=88)
        print("wrote", os.path.join(d, n + ".jpg"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
