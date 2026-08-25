#!/usr/bin/env bash
# Generate the MPEG-1/MPEG-2 host-test matrix: elementary streams with
# ffmpeg's own mpeg1video/mpeg2video encoders, plus the reference YUV from
# ffmpeg's decoder, plus the four streams FFmpeg's encoder CANNOT write (see
# tools/genmpeg12_interlaced.py for why field pictures, 16x8 and dual prime
# have to be assembled by hand).
#
# THE REFERENCE IS DECODED WITH `-idct simple`, AND THAT IS THE WHOLE GATE.
# 13818-2 specifies the IDCT by an accuracy requirement, not an algorithm, so
# two conforming decoders differ by +-1 per sample and P/B prediction feeds
# that difference forward until the next I picture. Bit-exactness only means
# something once the transform is pinned on both sides -- ours to that exact
# integer transform (c/lib/video/mpeg12_idct.c), the reference by this flag.
# Dropping the flag here would compare against whichever SIMD transform the
# host CPU happens to select, which is not the same thing on two machines.
#
# `-threads 1` for the same reason: the corpus has to be a fixed target.
#
# Each case exists to exercise ONE thing that can be got wrong independently;
# where a feature only appears in combination (an interlaced picture that also
# uses B frames, say) there is a case for the combination too.
#
# Usage: genmpeg12.sh <outdir>
# Produces <outdir>/<case>.m2v and <outdir>/<case>.ref.yuv

set -eu
OUT="${1:?usage: genmpeg12.sh <outdir>}"
mkdir -p "$OUT"

HERE="$(cd "$(dirname "$0")" && pwd)"

# $1 case name, $2 elementary-stream format (mpeg1video|mpeg2video),
# $3 lavfi input, rest: encoder arguments.
case_gen() {
    local name="$1" fmt="$2" input="$3"; shift 3
    if [ ! -f "$OUT/$name.m2v" ]; then
        ffmpeg -v error -f lavfi -i "$input" -t 0.6 -threads 1 -idct simple \
               "$@" -f "$fmt" "$OUT/$name.m2v" -y
    fi
    if [ ! -f "$OUT/$name.ref.yuv" ] || \
       [ "$OUT/$name.m2v" -nt "$OUT/$name.ref.yuv" ]; then
        ffmpeg -v error -threads 1 -idct simple -i "$OUT/$name.m2v" \
               -f rawvideo -pix_fmt yuv420p "$OUT/$name.ref.yuv" -y
    fi
    echo "  $name: $(stat -c%s "$OUT/$name.m2v") bytes"
}

M2="-c:v mpeg2video -pix_fmt yuv420p"
M1="-c:v mpeg1video -pix_fmt yuv420p"
TS="testsrc2=rate=25"
MB="mandelbrot=rate=25"

# --- MPEG-2, progressive frame pictures -------------------------------------
# I only: intra DC prediction, both DC-size tables, the zigzag scan and the
# IDCT, with no inter machinery at all. If this fails nothing else can pass.
case_gen m2-i-352x288        mpeg2video "$TS:size=352x288"  $M2 -g 1  -qscale:v 3
case_gen m2-ip-352x288       mpeg2video "$TS:size=352x288"  $M2 -g 15 -bf 0 -qscale:v 3
case_gen m2-ibbp-352x288     mpeg2video "$TS:size=352x288"  $M2 -g 15 -bf 2 -qscale:v 3
# GOP length changes how often the predictors restart and how far the
# reference chain runs; a 2-frame GOP and a 30-frame one fail differently.
case_gen m2-ibbp-g4          mpeg2video "$TS:size=352x288"  $M2 -g 4  -bf 1 -qscale:v 3
case_gen m2-ibbp-g30         mpeg2video "$MB:size=352x288"  $M2 -g 30 -bf 2 -qscale:v 3
# Not a multiple of 16 in EITHER dimension: the coded picture is bigger than
# the displayed one and motion compensation reads the padding.
case_gen m2-i-322x242        mpeg2video "$TS:size=322x242"  $M2 -g 1  -qscale:v 3
case_gen m2-ibbp-322x242     mpeg2video "$TS:size=322x242"  $M2 -g 15 -bf 2 -qscale:v 3
# Syntax switches that change how a block is READ rather than what is in it.
case_gen m2-intravlc-352x288 mpeg2video "$MB:size=352x288"  $M2 -g 15 -bf 2 -qscale:v 3 -intra_vlc 1
case_gen m2-altscan-352x288  mpeg2video "$MB:size=352x288"  $M2 -g 15 -bf 2 -qscale:v 3 -alternate_scan 1
# (the encoder refuses non_linear_quant with the default qmax of 31, so the
#  ceiling is lowered here rather than the flag quietly dropped)
case_gen m2-nonlinq-352x288  mpeg2video "$MB:size=352x288"  $M2 -g 15 -bf 2 -qscale:v 6 -qmax 28 -non_linear_quant 1
# Custom quantiser matrices: the sequence header's download path, in zigzag
# order, and a matrix that is nothing like the default so a mix-up shows.
case_gen m2-cqm-352x288      mpeg2video "$TS:size=352x288"  $M2 -g 15 -bf 2 -qscale:v 3 \
    -intra_matrix "8,17,18,19,21,23,25,27,17,18,19,21,23,25,27,28,20,21,22,23,24,26,28,30,21,22,23,24,26,28,30,32,22,23,24,26,28,30,32,35,23,24,26,28,30,32,35,38,25,26,28,30,32,35,38,41,27,28,30,32,35,38,41,45" \
    -inter_matrix "16,18,20,22,24,26,28,30,18,20,22,24,26,28,30,32,20,22,24,26,28,30,32,34,22,24,26,28,30,32,34,36,24,26,28,30,32,34,36,38,26,28,30,32,34,36,38,40,28,30,32,34,36,38,40,42,30,32,34,36,38,40,42,44"
# Quantiser extremes: qscale 1 fills the escape codes, qscale 25 empties the
# picture into skipped macroblocks. Both are edges of the same path.
case_gen m2-q1-352x288       mpeg2video "$MB:size=352x288"  $M2 -g 15 -bf 2 -qscale:v 1
case_gen m2-q25-352x288      mpeg2video "$MB:size=352x288"  $M2 -g 15 -bf 2 -qscale:v 25
case_gen m2-640x360          mpeg2video "$MB:size=640x360"  $M2 -g 15 -bf 2 -qscale:v 4

# --- MPEG-2, interlaced FRAME pictures --------------------------------------
# +ildct is field DCT (a luma block holds one field's lines), +ilme is field
# motion estimation (two 16x8 field predictions per macroblock). Both are
# frame pictures; field PICTURES are the hand-written half below.
ILV="interlace"
case_gen m2-ildct-352x288    mpeg2video "$TS:size=352x288"  -vf "$ILV" $M2 -g 15 -bf 0 -qscale:v 3 -flags +ildct+ilme -top 1
case_gen m2-ildct-bf-352x288 mpeg2video "$TS:size=352x288"  -vf "$ILV" $M2 -g 15 -bf 2 -qscale:v 3 -flags +ildct+ilme -top 1
case_gen m2-ildct-alt-352x288 mpeg2video "$MB:size=352x288" -vf "$ILV" $M2 -g 15 -bf 2 -qscale:v 3 -flags +ildct+ilme -top 1 -alternate_scan 1

# --- MPEG-1 -----------------------------------------------------------------
# The regression half: every MPEG-2 feature above is absent here, and what is
# left is the six places the two standards differ (see mpeg12_slice.c).
case_gen m1-i-352x288        mpeg1video "$TS:size=352x288"  $M1 -g 1  -qscale:v 3
case_gen m1-ip-352x288       mpeg1video "$TS:size=352x288"  $M1 -g 15 -bf 0 -qscale:v 3
case_gen m1-ibbp-352x288     mpeg1video "$TS:size=352x288"  $M1 -g 15 -bf 2 -qscale:v 3
case_gen m1-ibbp-322x242     mpeg1video "$TS:size=322x242"  $M1 -g 15 -bf 2 -qscale:v 3
case_gen m1-g30-352x288      mpeg1video "$MB:size=352x288"  $M1 -g 30 -bf 2 -qscale:v 3
# qscale 1 on noisy content is what reaches MPEG-1's SECOND escape, the
# eight-more-bits form for |level| > 127. tests/mpeg12.mk asserts the census
# counts it rather than assuming this case gets there.
case_gen m1-q1-352x288       mpeg1video "$MB:size=352x288"  $M1 -g 15 -bf 2 -qscale:v 1
case_gen m1-cqm-352x288      mpeg1video "$TS:size=352x288"  $M1 -g 15 -bf 2 -qscale:v 3 \
    -intra_matrix "8,17,18,19,21,23,25,27,17,18,19,21,23,25,27,28,20,21,22,23,24,26,28,30,21,22,23,24,26,28,30,32,22,23,24,26,28,30,32,35,23,24,26,28,30,32,35,38,25,26,28,30,32,35,38,41,27,28,30,32,35,38,41,45"

# --- the escape codes, deliberately ----------------------------------------
# Random noise at qscale 1 through an all-8s quantiser matrix is the only
# thing in this corpus that reaches MPEG-1's SECOND escape in quantity (the
# ordinary cases reach it 0-3 times in 15 frames, which is not coverage, it is
# luck). It also puts ~400,000 escape codes through the coefficient path in
# ten frames, MPEG-1's 8-bit form and MPEG-2's 12-bit form respectively.
FLAT8="8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8"
NOISE="nullsrc=s=352x288:r=25,geq=random(1)*255:random(2)*255:random(3)*255"
case_gen m1-esc2-352x288     mpeg1video "$NOISE" $M1 -g 5 -bf 1 -qscale:v 1 \
    -intra_matrix "$FLAT8" -inter_matrix "$FLAT8"
case_gen m2-esc-352x288      mpeg2video "$NOISE" $M2 -g 5 -bf 1 -qscale:v 1 \
    -intra_matrix "$FLAT8" -inter_matrix "$FLAT8"

# --- the streams FFmpeg's encoder cannot write ------------------------------
python3 "$HERE/genmpeg12_interlaced.py" "$OUT"
for f in "$OUT"/field-intra.m2v "$OUT"/field-16x8.m2v "$OUT"/field-dmv.m2v \
         "$OUT"/frame-dmv.m2v "$OUT"/frame-field.m2v; do
    ref="${f%.m2v}.ref.yuv"
    if [ ! -f "$ref" ] || [ "$f" -nt "$ref" ]; then
        ffmpeg -v error -threads 1 -idct simple -i "$f" \
               -f rawvideo -pix_fmt yuv420p "$ref" -y
    fi
done
