#!/usr/bin/env bash
# Generate the H.265/HEVC host-test matrix: Main-profile 8-bit elementary
# streams from ffmpeg/libx265, plus reference YUV from ffmpeg's own HEVC
# decoder. Our decoder must match the reference byte-for-byte -- HEVC
# reconstruction is exactly specified integer arithmetic, so bit-exactness is
# achievable and required, exactly as for H.264 (tools/genvideo.sh).
#
# Usage: genvideo265.sh <outdir> [group]
#   group = "core"  (default) the I/P subset the decoder is gated on
#           "b"     the B-slice / reordering subset
#           "all"   both
# Produces <outdir>/<case>.h265 and <outdir>/<case>.ref.yuv for each case.
#
# NOTE ON DETERMINISM: libx265's default rate control is threaded and its
# lookahead is not reproducible across builds, so every case pins
# `pools=none:frame-threads=1:wpp=0` unless the case is specifically about
# wavefronts, and encodes at a fixed QP. Two runs on the same libx265 then
# produce the same bytes; two DIFFERENT libx265 versions still may not, which
# is exactly why tests/fixtures/video265/ carries a committed stream + its
# pinned CRC and `make test-h265` gates on that one even with no encoder
# installed.

set -eu
OUT="${1:?usage: genvideo265.sh <outdir> [core|b|all]}"
GROUP="${2:-core}"
mkdir -p "$OUT"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "genvideo265: ffmpeg not installed -- skipping generated matrix" >&2
    exit 3
fi
if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
    echo "genvideo265: this ffmpeg has no libx265 -- skipping generated matrix" >&2
    exit 3
fi

DET="pools=none:frame-threads=1:wpp=0:rc-lookahead=0"

# One case: $1 name, $2 lavfi input, $3 duration, $4... x265-params fragments
case_gen() {
    local name="$1" input="$2" dur="$3"; shift 3
    local params="$*"
    ffmpeg -v error -f lavfi -i "$input" -t "$dur" \
        -c:v libx265 -pix_fmt yuv420p -preset medium \
        -x265-params "$params" -f hevc "$OUT/$name.h265" -y
    ffmpeg -v error -i "$OUT/$name.h265" -f rawvideo -pix_fmt yuv420p \
        "$OUT/$name.ref.yuv" -y
    echo "  $name: $(stat -c%s "$OUT/$name.h265") bytes"
}

TS="testsrc2=rate=30"
MV="mandelbrot=rate=30"

if [ "$GROUP" = core ] || [ "$GROUP" = all ]; then
    # --- I only. Intra prediction (35 modes), the DST-VII 4x4, the DCT-II
    # 8/16/32, CABAC residual coding, the CTU/TU quadtree, deblocking, SAO.
    # No inter machinery at all, so a failure here is a reconstruction bug.
    case_gen i-only-160x120  "$TS:size=160x120" 0.5 "$DET:keyint=1:bframes=0:qp=27"
    # Not a multiple of the 64-luma CTB: the conformance-window crop path.
    case_gen i-only-322x242  "$TS:size=322x242" 0.4 "$DET:keyint=1:bframes=0:qp=27"
    # SAO off: separates a SAO bug from a deblocking bug in one measurement.
    case_gen i-nosao-160x120 "$TS:size=160x120" 0.4 "$DET:keyint=1:bframes=0:qp=30:sao=0"
    # Deblocking off too: pure reconstruction, no in-loop filter at all.
    case_gen i-raw-160x120   "$TS:size=160x120" 0.4 \
             "$DET:keyint=1:bframes=0:qp=30:sao=0:deblock=0:no-deblock=1"
    # Small CTBs: exercises the 16x16 CTB quadtree and many more CTU headers.
    case_gen i-ctb16-160x120 "$TS:size=160x120" 0.4 \
             "$DET:keyint=1:bframes=0:qp=27:ctu=16"

    # --- I/P. Merge/AMVP, the 8-tap luma and 4-tap chroma interpolation,
    # uni-prediction, the DPB and its sliding window.
    case_gen ip-320x240      "$TS:size=320x240" 1.0 "$DET:bframes=0:qp=27"
    case_gen ip-640x360      "$MV:size=640x360" 0.7 "$DET:bframes=0:qp=27"
    # Multiple references: L0 list construction from the short-term RPS.
    case_gen ip-refs4        "$MV:size=320x240" 1.0 "$DET:bframes=0:qp=27:ref=4"
    # Temporal MV prediction off: separates the collocated-MV path from AMVP.
    case_gen ip-notmvp       "$TS:size=320x240" 0.7 "$DET:bframes=0:qp=27:temporal-mvp=0"
    # AMP (asymmetric 2NxnU/nLx2N partitions) explicitly on.
    case_gen ip-amp          "$MV:size=320x240" 0.7 "$DET:bframes=0:qp=27:amp=1:rect=1"
    # Long GOP: RPS churn and POC wraparound pressure.
    case_gen ip-longgop      "$MV:size=160x120" 2.0 "$DET:bframes=0:qp=27:keyint=60"
    # Transform skip + sign data hiding, the two residual side channels.
    case_gen ip-tskip        "$TS:size=160x120" 0.7 \
             "$DET:bframes=0:qp=32:tskip=1:signhide=1"
    # Wavefront entropy sync: CABAC contexts saved after CTU 1 of each row and
    # restored at the start of the next, plus per-row entry points.
    case_gen ip-wpp          "$TS:size=320x240" 0.7 \
             "pools=none:frame-threads=1:rc-lookahead=0:bframes=0:qp=27:wpp=1"
    # NO multi-slice case, deliberately. x265 4.1 refuses --slices without
    # --wpp, and with both it emits a stream that ffmpeg's own decoder rejects
    # ("Overread slice header by 6 bits", "Skipping invalid undecodable NALU"),
    # while other geometries make it write a zero-byte file. Our decoder
    # returns H265_ERR_CORRUPT on the same stream, which is the correct answer
    # to a malformed one -- but a case both decoders refuse measures the
    # ENCODER, not us. Multiple slice segments per picture are implemented
    # (segment_address, per-slice CABAC re-init, entry points) and are
    # therefore UNVERIFIED; ip-wpp does cover entry-point offsets and CABAC
    # context save/restore, which is the machinery they share.
fi

if [ "$GROUP" = b ] || [ "$GROUP" = all ]; then
    # --- B slices. Bi-prediction, two reference lists, and decode order !=
    # output order, so this also gates the DPB's reorder logic.
    # x265 refuses rc-lookahead=0 together with bframes ("lookahead depth must
    # be greater than the max consecutive bframe count"), so the B group pins a
    # fixed lookahead instead of none; pools=none + frame-threads=1 still make
    # the encode reproducible.
    BDET="pools=none:frame-threads=1:wpp=0:rc-lookahead=8"
    case_gen b-320x240       "$TS:size=320x240" 1.0 "$BDET:bframes=3:b-adapt=0:qp=27"
    case_gen b-pyramid       "$MV:size=320x240" 1.0 \
             "$BDET:bframes=4:b-adapt=0:b-pyramid=1:qp=27"
    case_gen b-weighted      "$MV:size=320x240,fade=t=out:st=0:d=1" 1.0 \
             "$BDET:bframes=3:b-adapt=0:qp=27:weightb=1"
fi

echo "genvideo265: matrix ($GROUP) generated in $OUT"
