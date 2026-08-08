#!/usr/bin/env bash
# Generate the H.264 host-test matrix: elementary streams with ffmpeg/libx264,
# plus reference YUV from ffmpeg's own decoder. Our decoder must match the
# reference byte-for-byte (the H.264 reconstruction path is exactly specified
# integer arithmetic, so bit-exactness is achievable and required).
#
# The matrix covers three profiles, and the split matters:
#   Baseline  CAVLC, I/P only. Now the REGRESSION half -- it is what proves a
#             change made for CABAC or B slices did not break the path that
#             already worked.
#   Main      adds CABAC and B slices.
#   High      adds the 8x8 transform, the 8x8 intra modes and scaling matrices.
#             This is what every video on the open web actually is: bilibili
#             and YouTube both hand out profile_idc 100, CABAC, B pyramids,
#             8x8dct and implicit weighted bi-prediction.
#
# Each case exists to exercise ONE thing that can be got wrong independently;
# where a feature only shows up in combination (a B slice that also uses the
# 8x8 transform, say) there is a case for the combination too.
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

# ===================================================================== Main ==
# CABAC and B slices. Both are all-or-nothing: a decoder either tracks the
# arithmetic coder exactly or loses the slice, so these cases fail loudly.
MAIN="-c:v libx264 -profile:v main -pix_fmt yuv420p -preset veryslow"

# CABAC with no B slices: isolates the arithmetic decoder, the context tables
# and the residual coding from everything B slices add.
case_gen cabac-i-176x144   "$TS:size=176x144" $MAIN -g 1 \
                           -x264-params cabac=1:bframes=0
case_gen cabac-ip-320x240  "$TS:size=320x240" $MAIN \
                           -x264-params cabac=1:bframes=0
# P_8x8 and its sub-macroblock partitions, under CABAC: sub_mb_type, four
# reference indices and up to sixteen motion vector differences per macroblock.
case_gen cabac-p8x8-176x144 "$TS:size=176x144" $MAIN \
                           -x264-params cabac=1:bframes=0:partitions=p8x8,p4x4,i4x4

# B slices under BOTH entropy coders, so a failure separates "B slices" from
# "CABAC". The CAVLC one is the more useful of the two when something breaks.
case_gen cavlc-b-176x144   "$TS:size=176x144" $MAIN \
                           -x264-params cabac=0:bframes=2:b-pyramid=none
case_gen cabac-b-176x144   "$TS:size=176x144" $MAIN \
                           -x264-params cabac=1:bframes=2:b-pyramid=none
# Spatial and temporal direct are different derivations that share nothing but
# their output shape; x264 defaults to spatial, so temporal needs asking for.
case_gen b-spatial-320x240 "$MV:size=320x240" $MAIN \
                           -x264-params cabac=1:bframes=3:direct=spatial
case_gen b-temporal-320x240 "$MV:size=320x240" $MAIN \
                           -x264-params cabac=1:bframes=3:direct=temporal
# B pyramid: B pictures that are themselves references. This is where the
# output (bumping) process earns its keep -- decode order and display order
# stop being a simple lag and the DPB has to sort by picture order count.
case_gen b-pyramid-320x240 "$MV:size=320x240" $MAIN \
                           -x264-params cabac=1:bframes=4:b-pyramid=normal
# Implicit weighted bi-prediction (weighted_bipred_idc 2) derives its weights
# from the picture order counts rather than the slice header. bilibili's
# encoder turns this on, so it is not a corner.
case_gen b-implicit-320x240 "$MV:size=320x240,fade=t=out:st=0:d=2" $MAIN \
                           -x264-params cabac=1:bframes=3:weightb=1
case_gen b-explicit-320x240 "$MV:size=320x240,fade=t=out:st=0:d=2" $MAIN \
                           -x264-params cabac=0:bframes=3:weightb=1

# ===================================================================== High ==
HIGH="-c:v libx264 -profile:v high -pix_fmt yuv420p -preset veryslow"

# The 8x8 transform and the 8x8 intra modes, with each entropy coder: CAVLC
# codes an 8x8 block as four INTERLEAVED 4x4 blocks and CABAC as one block of
# 64 coefficients, so they are genuinely different code paths into the same
# transform.
case_gen high-i8x8-176x144 "$TS:size=176x144" $HIGH -g 1 \
                           -x264-params cabac=1:bframes=0:8x8dct=1
case_gen high-cavlc8-176x144 "$TS:size=176x144" $HIGH -g 1 \
                           -x264-params cabac=0:bframes=0:8x8dct=1
case_gen high-p8x8dct-320x240 "$TS:size=320x240" $HIGH \
                           -x264-params cabac=1:bframes=0:8x8dct=1

# Everything at once, which is what a real web video is.
case_gen high-full-320x240 "$MV:size=320x240" $HIGH \
                           -x264-params cabac=1:bframes=3:b-pyramid=normal:8x8dct=1:weightb=1:ref=4
case_gen high-full-640x360 "$TS:size=640x360" $HIGH \
                           -x264-params cabac=1:bframes=3:b-pyramid=normal:8x8dct=1:weightb=1:ref=4
# Multiple slices per frame on top of High: the macroblock raster restarts
# mid-picture, which resets the CABAC engine and every neighbour availability
# answer along the slice boundary.
case_gen high-slices-320x240 "$TS:size=320x240" $HIGH -slices 4 \
                           -x264-params cabac=1:bframes=2:8x8dct=1

# Scaling matrices. x264 only emits them for its --tune psnr/ssim-independent
# "cqm" settings; the JVT default matrices are the ones a real encoder reaches
# for, and they change the dequantiser for every coefficient in the stream.
case_gen high-cqm-jvt-320x240 "$TS:size=320x240" $HIGH \
                           -x264-params cabac=1:bframes=2:8x8dct=1:cqm=jvt
case_gen high-cqm-flat-320x240 "$TS:size=320x240" $HIGH \
                           -x264-params cabac=1:bframes=2:8x8dct=1:cqm=flat

echo "genvideo: matrix generated in $OUT"
