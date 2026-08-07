#!/usr/bin/env bash
# Generate the H.264 host-test matrix: baseline-profile elementary streams with
# ffmpeg/libx264, plus reference YUV from ffmpeg's own decoder. Our decoder must
# match the reference byte-for-byte (the H.264 reconstruction path is exactly
# specified integer arithmetic, so bit-exactness is achievable and required).
#
# Usage: genvideo.sh <outdir>
# Produces <outdir>/<case>.h264 and <outdir>/<case>.ref.yuv for each case.

set -eu
OUT="${1:?usage: genvideo.sh <outdir>}"
mkdir -p "$OUT"

# One case: $1 name, $2 lavfi input, $3... x264 encode args
case_gen() {
    local name="$1" input="$2"; shift 2
    ffmpeg -v error -f lavfi -i "$input" -t 2 "$@" -f h264 "$OUT/$name.h264" -y
    ffmpeg -v error -i "$OUT/$name.h264" -f rawvideo -pix_fmt yuv420p "$OUT/$name.ref.yuv" -y
    echo "  $name: $(stat -c%s "$OUT/$name.h264") bytes"
}

ENC="-c:v libx264 -profile:v baseline -pix_fmt yuv420p -preset veryslow"
TS="testsrc2=rate=30"
MV="mandelbrot=rate=30"

# I-frames only: exercises intra pred / CAVLC / IDCT with zero inter machinery.
case_gen i-only-160x120    "$TS:size=160x120" $ENC -g 1
case_gen i-only-322x242    "$TS:size=322x242" $ENC -g 1   # frame_cropping path

# I/P, single reference, the bread and butter.
case_gen ip-320x240        "$TS:size=320x240" $ENC
case_gen ip-640x360        "$MV:size=640x360" $ENC

# Multiple slices per frame: MB raster restarts mid-frame.
case_gen slices4-320x240   "$TS:size=320x240" $ENC -slices 4

# Multiple reference frames: L0 list construction + ref reordering.
case_gen refs4-320x240     "$MV:size=320x240" $ENC -x264-params refs=4

# Deblock-stress: sharp content, high QP edges, and deblock a/b offsets.
# NOTE: "deblock=0,0" does NOT disable the filter -- those are the alpha/beta
# offsets and 0,0 is the default, so that spelling produced a stream byte-
# identical to ip-320x240 and this slot tested nothing. Disabling it takes
# no-deblock, and a P stream with the loop filter off is worth having: it
# separates a reconstruction bug from a deblocking bug in one measurement.
case_gen deblock-320x240   "$TS:size=320x240" $ENC -x264-params deblock=3,3
case_gen nodeblock-320x240 "$TS:size=320x240" $ENC -x264-params no-deblock=1

# Long GOP: sliding-window DPB eviction + frame_num wraparound paths.
case_gen longgop-160x120   "$MV:size=160x120" $ENC -g 60

# Weighted P prediction (PPS flag; baseline allows it for P slices).
# NOTE: "-x264-params weightp=1" on steady content was a no-op against preset
# veryslow -- it produced the same reconstruction as refs4-320x240 (only the
# options string x264 writes into an SEI differed), so weighted prediction was
# never actually exercised. Weighting is what an encoder reaches for on a
# global luma ramp, so fade the source: that is what puts non-default
# luma_weight/offset in the slice header.
case_gen wpred-320x240     "$MV:size=320x240,fade=t=out:st=0:d=2" $ENC \
                           -x264-params weightp=2

echo "genvideo: matrix generated in $OUT"
