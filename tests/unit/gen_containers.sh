#!/bin/bash
# tests/unit/gen_containers.sh -- build the AVI/MPEG-TS/MPEG-PS/FLV fixture
# matrix with ffmpeg. Same shape and same reasoning as tests/unit/gen_media.sh
# (read that file's header first) -- deterministic generated sources
# (lavfi testsrc2 + sine), no dependency on any other committed fixture, so
# regenerating needs only ffmpeg:
#
#     bash tests/unit/gen_containers.sh tests/fixtures/media
#
# Exit 3 means "no ffmpeg" -- callers skip the generated half exactly as
# gen_media.sh's callers do.
#
# One file per format, H.264 baseline video (the settings the decoder next
# door is proven bit-exact against, tools/genvideo.sh) + the audio codec each
# container's demuxer here actually decodes: MP3 for AVI/TS/PS (avi.c's
# strf 0x0055, pes.h's stream_type 0x03/0x04), AAC for FLV (flv.c only
# indexes SoundFormat 10 -- see flv.c's own header comment). No B frames:
# none of these four formats' demuxers here read composition offsets (AVI and
# FLV have none to read; TS/PS carry PTS only), so a B-frame source would
# test nothing about them that the MP4 ctts fixture does not already cover.
set -u
OUT=${1:-tests/fixtures/media}

command -v ffmpeg >/dev/null 2>&1 || exit 3
mkdir -p "$OUT"

V="-f lavfi -i testsrc2=size=64x48:rate=15:duration=2"
A="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2"
BASE="-c:v libx264 -profile:v baseline -preset veryslow -pix_fmt yuv420p -g 15"
MP3="-c:a libmp3lame -b:a 64k"
AAC="-c:a aac -b:a 128k"

q() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# 1. AVI: H.264 + MP3 -- avi.c's strf audio format-tag path (0x0055).
q $V $A $BASE $MP3 -shortest "$OUT/containers-h264-mp3.avi"

# 2. MPEG-TS: H.264 + MP3 -- ts.c's PMT stream_type dispatch (0x1B/0x03).
q $V $A $BASE $MP3 -shortest -f mpegts "$OUT/containers-h264-mp3.ts"

# 3. MPEG-PS (.mpg): H.264 + MP3 -- ps.c's PES stream_id dispatch, PSM when
# ffmpeg's muxer writes one. -f mpeg is ffmpeg's generic MPEG-PS muxer; it
# does not require MPEG-1/2 video the way -f vob might expect, so H.264 muxes
# cleanly into it (verified: ps.c opens and decodes the result).
q $V $A $BASE $MP3 -shortest -f mpeg "$OUT/containers-h264-mp3.mpg"

# 4. FLV: H.264 + AAC -- flv.c only indexes AAC audio (SoundFormat 10); an
# MP3-in-FLV file opens with zero audio samples, which would test nothing.
q $V $A $BASE $AAC -shortest -f flv "$OUT/containers-h264-aac.flv"

echo "wrote $OUT/containers-h264-mp3.{avi,ts,mpg} and $OUT/containers-h264-aac.flv"
