#!/usr/bin/env bash
# tools/genmjpeg.sh -- generate MJPEG test fixtures for c/lib/video/mjpeg.c.
#
# For each case (name:width:height:nframes:lavfi-pattern:qlevel) this:
#   1. encodes an MJPEG-in-AVI clip with ffmpeg (-c:v mjpeg -huffman default,
#      which writes the ITU-T.81 Annex K.3 STANDARD Huffman tables -- the same
#      fact tools/gen_mjpeg_tables.py cross-checked against cjpeg when it built
#      c/lib/video/mjpeg_deftables.inc);
#   2. extracts every frame losslessly (`-c:v copy -f image2`) to individual
#      complete JPEGs;
#   3. decodes each extracted frame with `djpeg -nosmooth -dct int -pnm` --
#      the SAME oracle c/lib/image/jpeg.c is gated against (tests/unit/
#      jpeg_test.c) -- and writes the raw RGBA8 reference bytes;
#   4. builds TWO elementary MJPEG streams per case by concatenating the
#      per-frame JPEGs in order (concatenating whole SOI..EOI JPEGs is exactly
#      what a legal MJPEG elementary stream is -- see mjpeg_next_frame's
#      comment): <case>.mjpeg keeps every frame's DHT segment as ffmpeg wrote
#      it; <case>_nodht.mjpeg has that DHT segment SURGICALLY REMOVED from
#      every frame -- this is the AVI "MJPG" default-tables convention
#      (mjpeg.h's file comment), reproduced here rather than found in the
#      wild because no encoder on this machine has a "omit DHT" switch.
#
# Before removing a frame's DHT this script VERIFIES its bytes equal the four
# tables in c/lib/video/mjpeg_deftables.inc, byte for byte (parsed straight out
# of that generated file, not retyped) -- so a future regeneration of that file
# from a different reference pair would make this script fail loudly instead of
# silently building a fixture the decoder was never actually tested against.
#
# Needs: ffmpeg, djpeg (libjpeg/libjpeg-turbo), python3 -- all already required
# by tests/unit/jpeg_gen.py's djpeg oracle and by tools/gen_mjpeg_tables.py.
#
# Usage: tools/genmjpeg.sh <outdir>
#   Writes <outdir>/manifest.txt ("<case> <nframes> <w> <h>", one per case)
#   plus, per case: <case>.mjpeg, <case>_nodht.mjpeg, <case>_f<k>.ref (k=0..).
set -euo pipefail

OUT="${1:?usage: genmjpeg.sh <outdir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFTABLES="$HERE/c/lib/video/mjpeg_deftables.inc"

for bin in ffmpeg djpeg python3; do
    command -v "$bin" >/dev/null 2>&1 || { echo "genmjpeg.sh: need '$bin' on PATH" >&2; exit 1; }
done
[ -f "$DEFTABLES" ] || { echo "genmjpeg.sh: missing $DEFTABLES" >&2; exit 1; }

mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# name : width : height : nframes : lavfi pattern : ffmpeg -q:v level
# Deliberately mixed: c34/c18 are NOT a whole number of 4:2:0 MCUs (16x16) in
# either dimension, which is where an MCU-grid-vs-block-grid confusion in the
# framing walk would first show up as a decode error rather than a shear (this
# file reuses jpeg.c's decode wholesale, so there is no separate progressive
# scan-grid risk here -- the point is only to not accidentally test exclusively
# whole-MCU geometry). c64_hi/c64_lo hold the SAME content at two very
# different quantizer levels, to show the byte-exactness gate does not depend
# on picture quality/quantization -- only on entropy-decode + IDCT + colour
# conversion, which the two share.
CASES=(
    "c64_hi:64:48:4:testsrc2:2"
    "c64_lo:64:48:4:testsrc2:28"
    "c34:34:26:4:mandelbrot:12"
    "c96:96:64:3:smptebars:20"
    "c18:18:14:2:rgbtestsrc:6"
)

: > "$OUT/manifest.txt"

for case in "${CASES[@]}"; do
    IFS=':' read -r name w h nframes pattern q <<<"$case"
    avi="$WORK/$name.avi"
    fdir="$WORK/${name}_frames"
    mkdir -p "$fdir"

    ffmpeg -y -v error -f lavfi -i "${pattern}=size=${w}x${h}:rate=8" \
        -frames:v "$nframes" -c:v mjpeg -huffman default -pix_fmt yuvj420p \
        -q:v "$q" -f avi "$avi"

    ffmpeg -y -v error -i "$avi" -c:v copy -f image2 "$fdir/f%03d.jpg"

    got=$(ls "$fdir"/f*.jpg | wc -l)
    if [ "$got" -ne "$nframes" ]; then
        echo "genmjpeg.sh: $name: ffmpeg extracted $got frames, wanted $nframes" >&2
        exit 1
    fi

    echo "$name $nframes $w $h" >> "$OUT/manifest.txt"
    echo "genmjpeg.sh: $name  ${w}x${h}  ${nframes} frames  q=$q  pattern=$pattern"
done

# The byte-munging (marker walk, DHT verification/removal, PNM->RGBA, stream
# concatenation) is one python3 pass over everything ffmpeg just produced.
python3 - "$OUT" "$WORK" "$DEFTABLES" "${CASES[@]}" <<'PY'
import re, subprocess, sys

outdir, work, deftables = sys.argv[1], sys.argv[2], sys.argv[3]
cases = sys.argv[4:]

# --- parse the four checked-in default tables straight out of the .inc file,
# so this script cross-checks against what the DECODER actually splices in,
# never a second hand-typed copy of Annex K.3. ---
src = open(deftables).read()
def parse_arr(name, expect_n):
    m = re.search(name + r"\[\d+\]\s*=\s*\{([^}]*)\}", src)
    if not m:
        sys.exit(f"genmjpeg.sh: {deftables} has no array {name}")
    vals = [int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()]
    if len(vals) != expect_n:
        sys.exit(f"genmjpeg.sh: {name} has {len(vals)} entries, want {expect_n}")
    return vals

TABLES = {
    (0, 0): (parse_arr("mjpeg_std_dc_luma_bits", 17)[1:], parse_arr("mjpeg_std_dc_luma_vals", 12)),
    (0, 1): (parse_arr("mjpeg_std_dc_chroma_bits", 17)[1:], parse_arr("mjpeg_std_dc_chroma_vals", 12)),
    (1, 0): (parse_arr("mjpeg_std_ac_luma_bits", 17)[1:], parse_arr("mjpeg_std_ac_luma_vals", 162)),
    (1, 1): (parse_arr("mjpeg_std_ac_chroma_bits", 17)[1:], parse_arr("mjpeg_std_ac_chroma_vals", 162)),
}


def walk_markers(data):
    """Yield (marker_byte, seg_start, seg_len_incl_marker) for every marker
    segment up to and including SOS; SOS itself is yielded with seg_len
    covering just its header (the entropy data after it is not walked -- this
    function only needs the header segments)."""
    i = 2
    n = len(data)
    out = []
    while i + 1 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        if m == 0x01 or (0xD0 <= m <= 0xD7):
            i += 2
            continue
        if m == 0xD9:
            break
        seglen = (data[i + 2] << 8) | data[i + 3]
        out.append((m, i, 2 + seglen))
        if m == 0xDA:
            break
        i += 2 + seglen
    return out


def strip_dht_verified(data, name):
    """Return (data_without_dht, removed_any). Every DHT segment found must
    decode to exactly the four tables in TABLES, in order dc_luma, dc_chroma,
    ac_luma, ac_chroma (that is the order c/lib/video/mjpeg.c's
    build_default_dht writes them in, and the order ffmpeg -huffman default
    happens to emit) -- anything else aborts rather than building a fixture
    that silently tests nothing."""
    segs = walk_markers(data)
    dht = [(off, ln) for (m, off, ln) in segs if m == 0xC4]
    if not dht:
        sys.exit(f"genmjpeg.sh: {name}: frame has no DHT segment to verify/strip")

    order = [(0, 0), (0, 1), (1, 0), (1, 1)]
    want = bytearray()
    for tc_th in order:
        bits, vals = TABLES[tc_th]
        want.append((tc_th[0] << 4) | tc_th[1])
        want.extend(bits)
        want.extend(vals)

    got = bytearray()
    for off, ln in dht:
        got.extend(data[off + 4: off + ln])   # skip FF C4 + 2-byte length
    if bytes(got) != bytes(want):
        sys.exit(f"genmjpeg.sh: {name}: DHT bytes do not match "
                  f"mjpeg_deftables.inc -- ffmpeg -huffman default drifted "
                  f"from Annex K.3, or wrote the four tables in an unexpected "
                  f"order. Refusing to build a nodht fixture from it.")

    out = bytearray()
    prev = 0
    for off, ln in dht:
        out += data[prev:off]
        prev = off + ln
    out += data[prev:]
    return bytes(out)


def read_pnm(data):
    assert data[:2] in (b"P6", b"P5"), "djpeg did not emit P6/P5"
    gray = data[:2] == b"P5"
    fields, i, n = [], 2, len(data)
    while len(fields) < 3:
        while i < n and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < n and data[i:i + 1] != b"\n":
                i += 1
            continue
        start = i
        while i < n and not data[i:i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    w, h, maxval = fields
    assert maxval == 255
    i += 1
    px = data[i:]
    rgba = bytearray()
    if gray:
        for k in range(w * h):
            g = px[k]
            rgba += bytes([g, g, g, 255])
    else:
        for k in range(w * h):
            rgba += bytes([px[k * 3], px[k * 3 + 1], px[k * 3 + 2], 255])
    return w, h, bytes(rgba)


def djpeg_ref(jpg_path):
    out = subprocess.run(["djpeg", "-nosmooth", "-dct", "int", "-pnm", jpg_path],
                          capture_output=True, check=True).stdout
    return read_pnm(out)


import os
for case in cases:
    name, w, h, nframes, pattern, q = case.split(":")
    w, h, nframes = int(w), int(h), int(nframes)
    fdir = os.path.join(work, f"{name}_frames")
    frames = sorted(os.listdir(fdir))
    if len(frames) != nframes:
        sys.exit(f"genmjpeg.sh: {name}: {len(frames)} frame files, manifest says {nframes}")

    with_dht = bytearray()
    without_dht = bytearray()
    for k, fn in enumerate(frames):
        path = os.path.join(fdir, fn)
        data = open(path, "rb").read()

        rw, rh, ref = djpeg_ref(path)
        if (rw, rh) != (w, h):
            sys.exit(f"genmjpeg.sh: {name} frame {k}: djpeg reports {rw}x{rh}, want {w}x{h}")
        open(os.path.join(outdir, f"{name}_f{k}.ref"), "wb").write(ref)

        with_dht += data
        without_dht += strip_dht_verified(data, f"{name} frame {k}")

    open(os.path.join(outdir, f"{name}.mjpeg"), "wb").write(bytes(with_dht))
    open(os.path.join(outdir, f"{name}_nodht.mjpeg"), "wb").write(bytes(without_dht))
    print(f"genmjpeg.sh: {name}: wrote {nframes} refs + .mjpeg ({len(with_dht)} B) "
          f"+ _nodht.mjpeg ({len(without_dht)} B, -{len(with_dht) - len(without_dht)} B of DHT)")

print(f"genmjpeg.sh: done -- {outdir}/manifest.txt + {len(cases)} cases")
PY
