#!/usr/bin/env python3
"""Compare two PPM images (P6, .ppm or .ppm.gz) with a per-channel tolerance.

    ppm_diff.py A B [--tol N] [--max-bad K] [--diff-out f.ppm]

A pixel is BAD when any channel differs by more than N (default 0). The run
fails when more than K pixels are bad (default 0). It prints the maximum
per-channel delta, the bad-pixel count and the bad bounding box, always -- a
comparison that says nothing when it passes cannot be told from one that did
not run.

Exit status: 0 match, 1 mismatch (first line "FAIL golden ..."), 2 a size or
format error ("ERROR ..."). --diff-out writes a P6 with every bad pixel
magnified to pure red/white and the rest dimmed, for looking at.

Two helpers keep the golden store byte-stable:
    ppm_diff.py gzip IN OUT     gzip with mtime=0 and no stored name
    ppm_diff.py --selftest      the comparison's own checks (identical images give
                                0, a 1-px delta of 3 at tol 2 gives 1, ...)
"""
import argparse
import gzip
import io
import os
import sys
import tempfile
import zlib


class FormatError(Exception):
    pass


def read_ppm(path):
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        raise FormatError("cannot read %s: %s" % (path, e))
    if raw[:2] == b"\x1f\x8b":
        try:
            raw = gzip.decompress(raw)
        except (OSError, EOFError, zlib.error) as e:
            raise FormatError("%s: bad gzip: %s" % (path, e))
    # header: P6 <ws> W <ws> H <ws> MAXVAL <single ws> data; '#' comments allowed
    pos, toks = 0, []
    if raw[:2] != b"P6":
        raise FormatError("%s: not a binary P6 PPM" % path)
    pos = 2
    while len(toks) < 3:
        while pos < len(raw) and raw[pos:pos + 1].isspace():
            pos += 1
        if pos < len(raw) and raw[pos:pos + 1] == b"#":
            while pos < len(raw) and raw[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(raw) and not raw[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise FormatError("%s: truncated header" % path)
        try:
            toks.append(int(raw[start:pos]))
        except ValueError:
            raise FormatError("%s: bad header field" % path)
    pos += 1  # the single whitespace byte after MAXVAL
    w, h, mx = toks
    if w <= 0 or h <= 0 or mx != 255:
        raise FormatError("%s: unsupported size/maxval %dx%d max %d" % (path, w, h, mx))
    data = raw[pos:]
    if len(data) != w * h * 3:
        raise FormatError("%s: pixel data is %d bytes, expected %d" % (path, len(data), w * h * 3))
    return w, h, data


def compare(a, b, tol):
    """Return (max_delta, bad_count, bbox or None, bad_index_list)."""
    w, h, da = a
    _, _, db = b
    maxd, bad, bbox, idx = 0, 0, None, []
    if da == db:
        return 0, 0, None, idx
    for i in range(w * h):
        o = i * 3
        d = max(abs(da[o] - db[o]), abs(da[o + 1] - db[o + 1]), abs(da[o + 2] - db[o + 2]))
        if d > maxd:
            maxd = d
        if d > tol:
            bad += 1
            idx.append(i)
            x, y = i % w, i // w
            if bbox is None:
                bbox = [x, y, x, y]
            else:
                bbox[0] = min(bbox[0], x); bbox[1] = min(bbox[1], y)
                bbox[2] = max(bbox[2], x); bbox[3] = max(bbox[3], y)
    return maxd, bad, bbox, idx


def write_diff(path, a, bad_idx):
    w, h, da = a
    out = bytearray(len(da))
    for i in range(w * h):
        o = i * 3
        out[o] = da[o] // 4; out[o + 1] = da[o + 1] // 4; out[o + 2] = da[o + 2] // 4
    # magnify: a bad pixel and its 8 neighbours go red, the pixel itself white
    for i in bad_idx:
        x, y = i % w, i // w
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and 0 <= yy < h:
                    o = (yy * w + xx) * 3
                    out[o], out[o + 1], out[o + 2] = 255, 0, 0
        o = i * 3
        out[o], out[o + 1], out[o + 2] = 255, 255, 255
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(out))


def run(path_a, path_b, tol, max_bad, diff_out, out=sys.stdout):
    try:
        a, b = read_ppm(path_a), read_ppm(path_b)
    except FormatError as e:
        print("ERROR format: %s" % e, file=out)
        return 2
    if a[0] != b[0] or a[1] != b[1]:
        print("ERROR size: %s is %dx%d, %s is %dx%d" % (path_a, a[0], a[1], path_b, b[0], b[1]), file=out)
        return 2
    maxd, bad, bbox, idx = compare(a, b, tol)
    where = "bbox %d,%d-%d,%d" % tuple(bbox) if bbox else "bbox none"
    summary = "max delta %d, bad %d (tol %d, allowed %d), %s  [%s vs %s]" % (
        maxd, bad, tol, max_bad, where, os.path.basename(path_a), os.path.basename(path_b))
    if diff_out and bad:
        write_diff(diff_out, a, idx)
    if bad > max_bad:
        print("FAIL golden: " + summary, file=out)
        return 1
    print("ok golden: " + summary, file=out)
    return 0


def write_gzip(src, dst):
    with open(src, "rb") as f:
        raw = f.read()
    buf = io.BytesIO()
    # filename='' and mtime=0: no name, no timestamp, so the bytes depend only on the pixels
    with gzip.GzipFile(filename="", mode="wb", fileobj=buf, mtime=0, compresslevel=9) as g:
        g.write(raw)
    with open(dst, "wb") as f:
        f.write(buf.getvalue())


def selftest():
    fails = 0

    def ppm(w, h, pix):
        return b"P6\n%d %d\n255\n" % (w, h) + bytes(pix)

    def check(name, got, want):
        nonlocal fails
        ok = got == want
        print("%-4s selftest %s: got %r want %r" % ("ok" if ok else "FAIL", name, got, want))
        if not ok:
            fails += 1

    with tempfile.TemporaryDirectory() as d:
        base = [10, 20, 30] * 16               # 4x4
        one = list(base); one[5 * 3] += 3      # pixel 5, red +3
        files = {"a.ppm": ppm(4, 4, base), "b.ppm": ppm(4, 4, base),
                 "c.ppm": ppm(4, 4, one), "d.ppm": ppm(2, 2, [0] * 12),
                 "e.ppm": b"P5\n1 1\n255\n\x00"}
        for n, data in files.items():
            with open(os.path.join(d, n), "wb") as f:
                f.write(data)
        p = lambda n: os.path.join(d, n)
        sink = io.StringIO()
        check("identical -> exit 0", run(p("a.ppm"), p("b.ppm"), 0, 0, None, sink), 0)
        check("identical max delta 0", "max delta 0, bad 0" in sink.getvalue(), True)
        sink = io.StringIO()
        check("delta 3 at tol 2 -> exit 1", run(p("a.ppm"), p("c.ppm"), 2, 0, None, sink), 1)
        check("delta 3 at tol 2 -> bad 1, max 3", "max delta 3, bad 1" in sink.getvalue(), True)
        check("FAIL golden printed", sink.getvalue().startswith("FAIL golden"), True)
        check("delta 3 at tol 3 -> exit 0", run(p("a.ppm"), p("c.ppm"), 3, 0, None, io.StringIO()), 0)
        check("delta 3 at tol 2, max-bad 1 -> exit 0", run(p("a.ppm"), p("c.ppm"), 2, 1, None, io.StringIO()), 0)
        check("size mismatch -> exit 2", run(p("a.ppm"), p("d.ppm"), 0, 0, None, io.StringIO()), 2)
        check("format error -> exit 2", run(p("a.ppm"), p("e.ppm"), 0, 0, None, io.StringIO()), 2)
        check("missing file -> exit 2", run(p("a.ppm"), p("nope.ppm"), 0, 0, None, io.StringIO()), 2)
        write_gzip(p("a.ppm"), p("a.ppm.gz"))
        write_gzip(p("a.ppm"), p("a2.ppm.gz"))
        with open(p("a.ppm.gz"), "rb") as f1, open(p("a2.ppm.gz"), "rb") as f2:
            g1, g2 = f1.read(), f2.read()
        check("gzip is byte-stable", g1 == g2, True)
        check("gzip header has mtime 0 and no name", g1[3] == 0 and g1[4:8] == b"\0\0\0\0", True)
        check(".ppm.gz reads back identical", run(p("a.ppm.gz"), p("a.ppm"), 0, 0, None, io.StringIO()), 0)
        run(p("a.ppm"), p("c.ppm"), 2, 0, p("diff.ppm"), io.StringIO())
        check("--diff-out wrote a P6", open(p("diff.ppm"), "rb").read(2), b"P6")
    print("%s ppm_diff selftest (%d failure(s))" % ("FAIL" if fails else "ok", fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", nargs="?")
    ap.add_argument("b", nargs="?")
    ap.add_argument("c", nargs="?")
    ap.add_argument("--tol", type=int, default=0, help="per-channel tolerance")
    ap.add_argument("--max-bad", type=int, default=0, help="bad pixels allowed")
    ap.add_argument("--diff-out", help="write the magnified difference image here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.a == "gzip":
        if not args.b or not args.c:
            ap.error("gzip needs IN and OUT")
        write_gzip(args.b, args.c)
        return 0
    if not args.a or not args.b:
        ap.error("two images are required")
    return run(args.a, args.b, args.tol, args.max_bad, args.diff_out)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
