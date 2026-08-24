#!/usr/bin/env python3
"""Generate c/lib/video/mjpeg_deftables.inc -- the four ITU-T.81 Annex K.3
"default" JPEG Huffman tables (DC luminance, DC chrominance, AC luminance,
AC chrominance) that c/lib/video/mjpeg.c splices into any MJPEG frame whose
encoder omitted its DHT segment (the AVI "MJPG" convention, among others).

WHY EXTRACTED, NOT TYPED. This tree's rule for a spec table (see
tools/gen_vp8_tables.py) is: generate it mechanically from a reference,
never hand-type the numbers, because a single wrong byte in a Huffman table
does not shade a pixel -- it desynchronises the entropy decoder into noise,
and one wrong byte in 348 is not findable by reading a diff.

The reference here is not one decoder's source file but an EXECUTABLE fact:
"default" Huffman tables are, by the standard's own definition, the exact
bit-length/value lists in Annex K.3, and every baseline JPEG encoder that
offers a non-optimizing mode must reproduce them byte-for-byte or it is not
conforming. So this script extracts the DHT segment TWO independent
encoders actually write when asked for the non-optimized/default tables --
libjpeg-turbo's `cjpeg` (no -optimize) and ffmpeg's mjpeg encoder
(-huffman default) -- across THREE unrelated source images, and requires
all six extractions to agree byte-for-byte before writing anything. An
encoder disagreeing with itself across images would mean "default" is not
actually content-invariant (a contradiction in terms); an encoder
disagreeing with the other would mean at least one of the two is not really
emitting Annex K.3. Either failure aborts the generator instead of guessing.

Needs: cjpeg (libjpeg-turbo) and ffmpeg, both on PATH.

Usage: gen_mjpeg_tables.py [outfile]
       (default outfile: c/lib/video/mjpeg_deftables.inc)
"""
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

OUT = sys.argv[1] if len(sys.argv) > 1 else "c/lib/video/mjpeg_deftables.inc"

CJPEG = shutil.which("cjpeg")
FFMPEG = shutil.which("ffmpeg")
if not CJPEG or not FFMPEG:
    sys.exit("gen_mjpeg_tables: need both cjpeg and ffmpeg on PATH")

NAMES = {(0, 0): "dc_luma", (0, 1): "dc_chroma", (1, 0): "ac_luma", (1, 1): "ac_chroma"}
EXPECT_N = {(0, 0): 12, (0, 1): 12, (1, 0): 162, (1, 1): 162}


def parse_dht_tables(data):
    """Walk JPEG markers and collect every DHT (class, id) -> (bits[1..16], vals)."""
    i = 2  # past SOI
    tables = {}
    n = len(data)
    while i + 1 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        if m in (0x01,) or (0xD0 <= m <= 0xD7):
            i += 2
            continue
        if m == 0xD9:  # EOI
            break
        if i + 3 >= n:
            break
        seglen = (data[i + 2] << 8) | data[i + 3]
        seg = data[i + 4:i + 2 + seglen]
        if m == 0xC4:
            o = 0
            while o < len(seg):
                tc, th = seg[o] >> 4, seg[o] & 0xF
                o += 1
                bits = list(seg[o:o + 16])
                o += 16
                tot = sum(bits)
                vals = list(seg[o:o + tot])
                o += tot
                tables[(tc, th)] = (bits, vals)
        if m == 0xDA:  # SOS: entropy data follows, nothing else to read
            break
        i += 2 + seglen
    return tables


def random_ppm(path, w, h, seed):
    rng = random.Random(seed)
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(rng.randrange(256) for _ in range(w * h * 3)))


def cjpeg_tables(tmp, w, h, seed):
    ppm = os.path.join(tmp, "s.ppm")
    jpg = os.path.join(tmp, "s_cjpeg.jpg")
    random_ppm(ppm, w, h, seed)
    # No -optimize: libjpeg-turbo falls back to the standard/default tables.
    subprocess.run([CJPEG, "-quality", "90", "-outfile", jpg, ppm], check=True,
                    capture_output=True)
    return parse_dht_tables(open(jpg, "rb").read())


def ffmpeg_tables(tmp, w, h, pattern):
    jpg = os.path.join(tmp, "s_ffmpeg.jpg")
    # A procedurally different SOURCE (not the same PPM bytes pushed through a
    # second tool) so this is a genuinely independent generation. testsrc2 has
    # no seed option; varying the lavfi pattern and the size across the three
    # runs is what supplies that independence instead.
    subprocess.run([FFMPEG, "-v", "error", "-y", "-f", "lavfi",
                     "-i", f"{pattern}=size={w}x{h}:rate=1",
                     "-frames:v", "1", "-c:v", "mjpeg", "-huffman", "default",
                     "-pix_fmt", "yuvj420p", "-update", "1", jpg],
                    check=True, capture_output=True)
    return parse_dht_tables(open(jpg, "rb").read())


def main():
    tmp = tempfile.mkdtemp(prefix="mjpeg_tables_")
    # Three unrelated sizes/patterns; the last is odd-sized on purpose (not a
    # whole number of MCUs) to rule out any size-dependent table selection.
    cases = [(32, 32, "testsrc2"), (48, 40, "mandelbrot"), (17, 23, "testsrc")]
    runs = []
    for k, (w, h, pattern) in enumerate(cases):
        runs.append(("cjpeg", w, h, cjpeg_tables(tmp, w, h, seed=100 + k)))
        runs.append(("ffmpeg", w, h, ffmpeg_tables(tmp, w, h, pattern)))

    ref = None
    for who, w, h, tables in runs:
        for key in NAMES:
            if key not in tables:
                sys.exit(f"gen_mjpeg_tables: {who} {w}x{h} is missing table {NAMES[key]}")
            bits, vals = tables[key]
            if len(vals) != EXPECT_N[key]:
                sys.exit(f"gen_mjpeg_tables: {who} {w}x{h} {NAMES[key]} has "
                          f"{len(vals)} values, want {EXPECT_N[key]}")
            if sum(bits) != len(vals):
                sys.exit(f"gen_mjpeg_tables: {who} {w}x{h} {NAMES[key]} bits "
                          f"sum {sum(bits)} != {len(vals)} values")
        if ref is None:
            ref = tables
        else:
            for key in NAMES:
                if tables[key] != ref[key]:
                    sys.exit(f"gen_mjpeg_tables: {who} {w}x{h} {NAMES[key]} "
                             f"DISAGREES with the first extraction -- "
                             f"'default' tables are not actually invariant, "
                             f"or one encoder is not emitting Annex K.3. "
                             f"Refusing to write anything.")

    shutil.rmtree(tmp, ignore_errors=True)

    lines = []
    lines.append("/* GENERATED by tools/gen_mjpeg_tables.py -- DO NOT EDIT BY HAND.")
    lines.append(" *")
    lines.append(" * The four ITU-T.81 Annex K.3 standard (\"default\") JPEG Huffman")
    lines.append(" * tables, extracted mechanically from the DHT segments TWO independent")
    lines.append(" * encoders write when told to use the default/non-optimized tables")
    lines.append(" * (libjpeg-turbo `cjpeg` with no -optimize, and ffmpeg's mjpeg encoder")
    lines.append(" * with -huffman default), across 3 unrelated source images each -- all")
    lines.append(" * 6 extractions agreed byte-for-byte before this file was written. See")
    lines.append(" * tools/gen_mjpeg_tables.py; regenerate with:")
    lines.append(" *   python3 tools/gen_mjpeg_tables.py")
    lines.append(" *")
    lines.append(" * shape (must-not-drift, printed so a diff is legible):")
    for key in [(0, 0), (0, 1), (1, 0), (1, 1)]:
        bits, vals = ref[key]
        bsha = hashlib.sha1(bytes(bits)).hexdigest()[:16]
        vsha = hashlib.sha1(bytes(vals)).hexdigest()[:16]
        lines.append(f" *   {NAMES[key]:10s} nval={len(vals):3d}  bits_sha1={bsha}  vals_sha1={vsha}")
    lines.append(" */")
    lines.append("")
    for key in [(0, 0), (0, 1), (1, 0), (1, 1)]:
        bits, vals = ref[key]
        name = NAMES[key]
        lines.append(f"static const unsigned char mjpeg_std_{name}_bits[17] = {{")
        lines.append("    0, " + ", ".join(str(b) for b in bits) + ",")
        lines.append("};")
        lines.append(f"static const unsigned char mjpeg_std_{name}_vals[{len(vals)}] = {{")
        for o in range(0, len(vals), 16):
            lines.append("    " + ", ".join(str(v) for v in vals[o:o + 16]) + ",")
        lines.append("};")
        lines.append("")

    os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    total = sum(len(ref[k][1]) for k in ref)
    print(f"wrote {OUT}: 4 tables, {total} values total, "
          f"cross-checked cjpeg vs ffmpeg over {len(cases)} images each")
    for key in [(0, 0), (0, 1), (1, 0), (1, 1)]:
        bits, vals = ref[key]
        print(f"  {NAMES[key]:10s} nval={len(vals):3d}  "
              f"bits_sha1={hashlib.sha1(bytes(bits)).hexdigest()[:16]}  "
              f"vals_sha1={hashlib.sha1(bytes(vals)).hexdigest()[:16]}")


if __name__ == "__main__":
    main()
