#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two screenshot sets (or two images) and write a diff image per shot.

    ui_shotdiff.py <A> <B> --out <dir> [--mask x,y,w,h]... [--mask-clock]
                   [--expect-identical] [--jpeg 88]

A and B are directories of shots (matched by file stem, so before/NN-x.jpg
meets after/NN-x.png) or two image files. Per shot it prints

    <name> changed=<px> max=<delta> bbox=<x0>,<y0>,<x1>,<y1>

(`bbox=-` when identical) and writes <out>/<name>.jpg: B dimmed, every changed
pixel painted red with a white core. A summary line closes the run. Shots in
only one set are reported as `<name> only-in-A|only-in-B` and count as a
difference.

--mask x,y,w,h     device pixels, blanked in BOTH images first (repeatable).
--mask-clock       masks the right 260x24 POINTS of the menu bar (the clock
                   text) and, when a manifest.tsv beside A or B lists a
                   `clockbox` for the shot, the Clock window. Points are device
                   pixels / backing scale (qmp_ui.pick_scale of the image size).
--expect-identical exit 1 and print `FAIL shot-diff <name> ...` on ANY
                   difference (after masking). Without it the exit status is 0
                   when the comparison ran.

Exit status: 0 ok, 1 --expect-identical violated, 2 usage / unreadable input.
"""
import argparse
import csv
import os
import sys

from PIL import Image, ImageChops, ImageFilter

IMG_EXT = (".jpg", ".jpeg", ".png", ".ppm", ".bmp")
CLOCK_W_PT, CLOCK_H_PT = 260, 24


def pick_scale(w, h):
    """Mirror of qmp_ui.pick_scale / fb.c (design 1280x800 points)."""
    s = min(w * 100 // 1280, h * 100 // 800)
    return max(100, min(300, (s // 25) * 25))


def shots_in(path):
    if os.path.isfile(path):
        return {os.path.splitext(os.path.basename(path))[0]: path}
    out = {}
    for f in sorted(os.listdir(path)):
        if f.lower().endswith(IMG_EXT):
            out[os.path.splitext(f)[0]] = os.path.join(path, f)
    return out


def clock_boxes(path):
    """{shot stem: (x,y,w,h)} from manifest.tsv's clockbox column, if any."""
    d = path if os.path.isdir(path) else os.path.dirname(path)
    m = os.path.join(d, "manifest.tsv")
    out = {}
    if not os.path.exists(m):
        return out
    with open(m, newline="") as fh:
        for row in csv.DictReader(fh, delimiter="\t"):
            box = (row.get("clockbox") or "").strip()
            name = os.path.splitext(row.get("name", ""))[0]
            if box and box != "-":
                try:
                    x, y, w, h = (int(v) for v in box.split(","))
                except ValueError:
                    continue
                out[name] = (x, y, w, h)
    return out


def load(path):
    try:
        im = Image.open(path)
        im.load()
        return im.convert("RGB")
    except Exception as e:  # noqa: BLE001 -- report any decoder error as usage
        raise SystemExit("ui_shotdiff: cannot read %s: %s" % (path, e))


def compare(a, b, boxes):
    """(changed, maxdelta, bbox, diffimage) for two RGB images."""
    if a.size != b.size:
        w, h = max(a.width, b.width), max(a.height, b.height)
        img = Image.new("RGB", (w, h), (255, 0, 0))
        return w * h, 255, (0, 0, w, h), img
    for (x, y, w, h) in boxes:
        blank = Image.new("RGB", (max(0, w), max(0, h)), (0, 0, 0))
        a = a.copy()
        b = b.copy()
        a.paste(blank, (x, y))
        b.paste(blank, (x, y))
    r, g, bl = ImageChops.difference(a, b).split()
    mx = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    hist = mx.histogram()
    changed = sum(hist[1:])
    maxd = max((i for i, n in enumerate(hist) if n), default=0)
    mask = mx.point(lambda v: 255 if v else 0)
    bbox = mask.getbbox()
    base = b.point(lambda v: v * 35 // 100)
    if changed:
        halo = mask.filter(ImageFilter.MaxFilter(5))
        base.paste((255, 40, 40), mask=halo)
        base.paste((255, 255, 255), mask=mask)
    return changed, maxd, bbox, base


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--out", required=True)
    ap.add_argument("--mask", action="append", default=[], metavar="x,y,w,h")
    ap.add_argument("--mask-clock", action="store_true")
    ap.add_argument("--expect-identical", action="store_true")
    ap.add_argument("--jpeg", type=int, default=88)
    args = ap.parse_args(argv)

    for p in (args.a, args.b):
        if not os.path.exists(p):
            print("ui_shotdiff: no such path %s" % p, file=sys.stderr)
            return 2
    fixed = []
    for m in args.mask:
        try:
            x, y, w, h = (int(v) for v in m.split(","))
        except ValueError:
            print("ui_shotdiff: bad --mask %r (want x,y,w,h)" % m, file=sys.stderr)
            return 2
        fixed.append((x, y, w, h))

    sa, sb = shots_in(args.a), shots_in(args.b)
    if os.path.isfile(args.a) and os.path.isfile(args.b):
        name = os.path.splitext(os.path.basename(args.b))[0]    # two files: one pair
        sa = {name: args.a}
        sb = {name: args.b}
    if not sa and not sb:
        print("ui_shotdiff: no images found", file=sys.stderr)
        return 2
    os.makedirs(args.out, exist_ok=True)
    ca = clock_boxes(args.a) if args.mask_clock else {}
    cb = clock_boxes(args.b) if args.mask_clock else {}

    diffs, rows = 0, []
    for name in sorted(set(sa) | set(sb)):
        if name not in sa or name not in sb:
            side = "only-in-A" if name in sa else "only-in-B"
            print("%s %s" % (name, side))
            if args.expect_identical:
                print("FAIL shot-diff %s %s" % (name, side))
            diffs += 1
            rows.append((name, -1, -1, "-"))
            continue
        a, b = load(sa[name]), load(sb[name])
        boxes = list(fixed)
        if args.mask_clock:
            s = pick_scale(b.width, b.height)
            cw, ch = CLOCK_W_PT * s // 100, CLOCK_H_PT * s // 100
            boxes.append((b.width - cw, 0, cw, ch))
            for table in (ca, cb):
                if name in table:
                    boxes.append(table[name])
        changed, maxd, bbox, img = compare(a, b, boxes)
        bb = "-" if bbox is None else "%d,%d,%d,%d" % bbox
        print("%s changed=%d max=%d bbox=%s" % (name, changed, maxd, bb))
        img.save(os.path.join(args.out, name + ".jpg"), quality=args.jpeg)
        rows.append((name, changed, maxd, bb))
        if changed:
            diffs += 1
            if args.expect_identical:
                print("FAIL shot-diff %s changed=%d max=%d" % (name, changed, maxd))
    with open(os.path.join(args.out, "summary.tsv"), "w") as fh:
        fh.write("name\tchanged\tmax\tbbox\n")
        for r in rows:
            fh.write("%s\t%d\t%d\t%s\n" % r)
    total = sum(r[1] for r in rows if r[1] > 0)
    print("ui_shotdiff: %d shot(s), %d differ, %d px changed in total"
          % (len(rows), diffs, total))
    return 1 if (args.expect_identical and diffs) else 0


if __name__ == "__main__":
    sys.exit(main())
