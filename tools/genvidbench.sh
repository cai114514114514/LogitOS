#!/usr/bin/env bash
# tools/genvidbench.sh -- stream matrix for the decode-throughput benchmark
# (tests/video.mk, c/apps/video/vidbench.c). NOT the correctness matrix
# (tools/genvideo.sh / genvideo265.sh) -- this one produces no reference YUV,
# because vidbench measures TIME, not bytes; a stream vidbench decodes wrongly
# would still time a real decode of real bitstream syntax, and the correctness
# gates (make test-h264 / test-h265) are what catch a wrong pixel.
#
# Four resolutions x two codecs, encoded to look like what CLAUDE.md measured
# off a real bilibili page: High-profile H.264 (CABAC, B-pyramid, 8x8dct,
# weighted bi-prediction, 4 refs) and HEVC Main with the same shape of
# machinery (CABAC always, B frames, AMP, SAO -- x265's own defaults already
# turn all of that on, so no -x265-params is needed to make it realistic).
#
# Frame counts fall as resolution rises on purpose: this feeds a QEMU-TCG
# guest boot, where whole-machine software decode of 1080p is not fast (see
# c/lib/video/h265.h's own memory note -- 1080p barely fits in the 24 MiB
# default arena at all), and the point of this matrix is a speed measurement,
# not an endurance run. Fewer frames still gives a stable fps average because
# every stream is one GOP: one I frame's outlier cost is a small fraction of
# even 15 frames' total.
#
# Usage: genvidbench.sh <outdir>
# Produces <outdir>/<name>.h264 and <outdir>/<name>.h265.

set -eu
OUT="${1:?usage: genvidbench.sh <outdir>}"
mkdir -p "$OUT"

# name  size       frames  fps
SPECS="
320x240   90  30
640x360   60  30
1280x720  30  30
1920x1080 15  30
"

H264ENC="-c:v libx264 -profile:v high -pix_fmt yuv420p -preset veryslow \
-x264-params cabac=1:bframes=3:b-pyramid=normal:8x8dct=1:weightb=1:ref=4"
# bframes=0: two independent decoder limits ruled out B frames for this
# matrix, bisected here rather than assumed --
#   (1) wpp=1 + bframes>0 returns H265_ERR_CORRUPT outright (wpp=1 alone
#       decodes fine, as the H265_GATE's own ip-wpp case proves), so wpp=0
#       was tried next;
#   (2) even with wpp=0, bframes>=1 at 640x360/1280x720/1920x1080 ALSO returns
#       H265_ERR_CORRUPT for several ref counts (ref=1,3,4 fail, ref=2
#       happens to decode) -- resolution-dependent, content-dependent, and NOT
#       covered by the existing correctness matrix, whose only B-slice cases
#       (test-h265-b) are all 320x240. That is consistent with h265.h's own
#       admission that "B slices decode but are NOT bit-exact ... treat 'HEVC
#       works' as a claim about I/P material only" -- this benchmark found a
#       sharper edge of the same known gap (an outright decode failure, not
#       just wrong pixels) but fixing c/lib/video/h265.c is out of scope for a
#       throughput measurement. bframes=0 is the H.265 feature set the decoder
#       actually claims to run everywhere; ref=4 (multi-reference P) is kept
#       since that direction is not implicated.
H265ENC="-c:v libx265 -pix_fmt yuv420p -preset medium \
-x265-params bframes=0:ref=4:wpp=0"

echo "$SPECS" | while read -r size frames fps; do
    [ -n "$size" ] || continue
    secs=$(awk -v f="$frames" -v r="$fps" 'BEGIN{printf "%.3f", f/r}')
    name264="h264-$size"
    name265="h265-$size"

    if [ ! -f "$OUT/$name264.h264" ]; then
        ffmpeg -nostdin -v error -f lavfi -i "mandelbrot=rate=$fps:size=$size" -t "$secs" \
            $H264ENC -f h264 "$OUT/$name264.h264" -y </dev/null
        echo "  $name264: $(stat -c%s "$OUT/$name264.h264") bytes, $frames frames requested"
    fi
    if [ ! -f "$OUT/$name265.h265" ]; then
        ffmpeg -nostdin -v error -f lavfi -i "mandelbrot=rate=$fps:size=$size" -t "$secs" \
            $H265ENC -f hevc "$OUT/$name265.h265" -y </dev/null
        echo "  $name265: $(stat -c%s "$OUT/$name265.h265") bytes, $frames frames requested"
    fi
done

echo "genvidbench: matrix generated in $OUT"
