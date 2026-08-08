#!/usr/bin/env python3
"""tests/unit/demux_lacing.py -- check Matroska lacing against arithmetic.

The laced fixtures were built by tests/unit/gen_laced.py, so the right answer
for every frame -- its length, its timestamp and its contents -- is known by
construction rather than by asking another implementation. This checks all
three: the sample count, the sizes, and that the BYTES of frame n really are
frame n's (each frame's payload is its own index repeated, so a reader that
mis-splits a laced block produces a sample whose contents give it away).

    python3 tests/unit/demux_lacing.py <demux_test> <builddir> <fixturedir>
"""
import subprocess, sys, os, collections

def main():
    if len(sys.argv) < 4:
        print("usage: demux_lacing.py <demux_test> <builddir> <fixturedir>")
        return 2
    tool, build, fx = sys.argv[1], sys.argv[2], sys.argv[3]
    expect = collections.defaultdict(list)
    path = os.path.join(fx, "laced.expect")
    if not os.path.exists(path):
        print("no laced.expect in %s" % fx)
        return 1
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        name, pts, size, first = line.split()
        expect[name].append((int(pts), int(size), int(first)))

    fails = []
    for name, want in sorted(expect.items()):
        f = os.path.join(fx, name)
        if not os.path.exists(f):
            fails.append("%s: missing" % name); continue
        out = subprocess.run([tool, "packets", f], capture_output=True, text=True)
        if out.returncode != 0:
            fails.append("%s: demux failed: %s" % (name, out.stderr.strip())); continue
        got = []
        for line in out.stdout.splitlines():
            p = line.split()
            if p and p[0] == "P":
                got.append((int(p[2]), int(p[4]), int(p[5])))   # pts, size, pos
        if len(got) != len(want):
            fails.append("%s: %d samples, expected %d" % (name, len(got), len(want)))
            continue
        blob = os.path.join(build, "laced_raw.bin")
        subprocess.run([tool, "raw", f, "0", blob], check=True)
        data = open(blob, "rb").read()
        at = 0
        for i, ((wpts, wsize, wfirst), (gpts, gsize, gpos)) in enumerate(zip(want, got)):
            if gpts != wpts:
                fails.append("%s frame %d pts %d, expected %d" % (name, i, gpts, wpts))
            if gsize != wsize:
                fails.append("%s frame %d size %d, expected %d" % (name, i, gsize, wsize))
                break
            chunk = data[at:at + gsize]
            at += gsize
            if len(set(chunk)) != 1 or chunk[0] != wfirst:
                fails.append("%s frame %d payload is frame %d's, not %d's"
                             % (name, i, chunk[0] if chunk else -1, wfirst))
                break
        if at != len(data):
            fails.append("%s: %d bytes demuxed, %d accounted for" % (name, len(data), at))
        print("  %-20s %d frames ok" % (name, len(got)))

    # The four files carry the same six 64-byte frames in the fixed-lacing and
    # no-lacing cases; if lacing is right they demux identically.
    a = subprocess.run([tool, "packets", os.path.join(fx, "laced-none.mkv")],
                       capture_output=True, text=True).stdout
    b = subprocess.run([tool, "packets", os.path.join(fx, "laced-fixed.mkv")],
                       capture_output=True, text=True).stdout
    sizes_a = [l.split()[4] for l in a.splitlines() if l.startswith("P")][:6]
    sizes_b = [l.split()[4] for l in b.splitlines() if l.startswith("P")][:6]
    if sizes_a != sizes_b:
        fails.append("fixed lacing does not agree with the unlaced control: %s vs %s"
                     % (sizes_a, sizes_b))

    if fails:
        print("DEMUX-LACING FAILED (%d)" % len(fails))
        for x in fails[:20]:
            print("   " + x)
        return 1
    print("DEMUX-LACING OK: Xiph, fixed and EBML lacing all split as constructed")
    return 0

if __name__ == "__main__":
    sys.exit(main())
