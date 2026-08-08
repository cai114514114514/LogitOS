#!/bin/bash
# tests/unit/gen_media.sh -- build the container fixture matrix with ffmpeg.
#
# The files this writes are COMMITTED under tests/fixtures/media, for the same
# reason tests/fixtures/video/sample.h264 is: the gate has to mean something on
# a machine with no encoder installed. Regenerate with:
#
#     bash tests/unit/gen_media.sh tests/fixtures/media
#
# and expect every pinned checksum in the suite to move, because a different
# ffmpeg writes a different file. That is the point of committing them.
#
# Exit 3 means "no ffmpeg" -- the caller skips the generated half and runs the
# committed fixtures only, exactly as tools/genvideo265.sh does for libx265.
#
# TWO CLASSES OF FIXTURE, AND WHY.
#
#   DEMUX-ONLY files use whatever the container needs to be interesting --
#   above all B frames, because B frames are what make MP4's ctts (composition
#   offsets) non-zero and decode order differ from display order, and a
#   demuxer that ignores ctts is correct on every file without them. The H.264
#   decoder here is BASELINE and refuses B slices, so these files are demuxed
#   and compared with ffmpeg but not decoded.
#
#   DECODABLE files are encoded inside the decoders' proven envelope -- H.264
#   baseline at preset veryslow, HEVC with the same deterministic x265
#   parameters tools/genvideo265.sh uses -- so the end-to-end claim
#   (container in, bit-exact pictures out) can be REQUIRED rather than hoped
#   for. Using a codec configuration the decoder next door was never proved
#   against would make a decoder bug look like a demuxer bug.
set -u
OUT=${1:-tests/fixtures/media}

command -v ffmpeg >/dev/null 2>&1 || exit 3
mkdir -p "$OUT"

# Deterministic, generated sources: no input file, same bytes on any machine
# with the same ffmpeg. testsrc2 moves (so P frames are not all skip).
V="-f lavfi -i testsrc2=size=64x48:rate=15:duration=2"
A="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2"

# The H.264 settings the decoder is proved bit-exact against (tools/genvideo.sh).
BASE="-c:v libx264 -profile:v baseline -preset veryslow -pix_fmt yuv420p -g 15"
# ...and the settings for the files that exist to exercise ctts.
BF="-c:v libx264 -profile:v main -preset veryslow -bf 2 -pix_fmt yuv420p -g 15"
MP3="-c:a libmp3lame -b:a 64k"

q() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# 1. The ordinary case: H.264 + MP3 in MP4, WITH B frames -> non-zero ctts,
#    and an edit list, because x264 writes one to hide the priming frames.
q $V $A $BF $MP3 -shortest -f mp4 "$OUT/h264-mp3.mp4"

# 2. Baseline, no B frames: pts == dts throughout. DECODABLE end to end.
q $V $A $BASE $MP3 -shortest -f mp4 "$OUT/h264-mp3-nobf.mp4"

# 3. FRAGMENTED MP4: no sample table at all, one moof per fragment, with B
#    frames so trun's composition offsets are exercised too. This is what a
#    streaming site delivers and it is a completely separate code path.
q $V $A $BF $MP3 -shortest \
      -movflags +frag_keyframe+empty_moov+default_base_moof -f mp4 "$OUT/frag.mp4"

# 4. Fragmented, one fragment per frame, baseline: hundreds of tiny moofs, each
#    with its own tfdt, and DECODABLE. Continuation of dts across fragments and
#    the tfdt path are different lines of code; both are wanted.
q $V $BASE -movflags +frag_every_frame+empty_moov -f mp4 "$OUT/frag-everyframe.mp4"

# 5. H.265 in MP4: hvcC instead of avcC, and a completely different
#    parameter-set layout (arrays of VPS/SPS/PPS rather than two counted
#    lists). Encoded with genvideo265.sh's deterministic parameters so it is
#    inside the HEVC decoder's proven envelope. DECODABLE.
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
    q $V -c:v libx265 -pix_fmt yuv420p -preset medium \
          -x265-params "pools=none:frame-threads=1:wpp=0:rc-lookahead=0:bframes=0:qp=27:log-level=none" \
          -tag:v hvc1 -f mp4 "$OUT/h265.mp4"
fi

# 6. AAC in MP4: the codec this system has NO decoder for. It is here so the
#    demuxer's handling of it is tested and so the gap is visible rather than
#    hidden -- the file must open, report aac with its esds DecoderSpecificInfo,
#    and be refused by the player with a message, not a crash.
q $A -c:a aac -b:a 64k -f mp4 "$OUT/aac.m4a"

# 7. QuickTime .mov with big-endian PCM ('twos'): a constant sample size, so
#    the sample "table" is 88,200 two-byte entries and the container is really
#    chunk-based. Also the version-1 sound sample description, which has extra
#    fields before the child boxes -- skipping the wrong number lands the
#    parser mid-box and it silently finds no extradata.
q $A -c:a pcm_s16be -f mov "$OUT/pcm.mov"

# 8. Matroska, H.264 + MP3, with B frames. EBML, clusters, SimpleBlocks, and
#    an audio track with a CodecDelay (LAME's 1105 samples of priming).
q $V $A $BF $MP3 -shortest -f matroska "$OUT/h264-mp3.mkv"

# 9. Matroska with FLAC and baseline video: CodecPrivate carries the FLAC
#    stream header rather than an avcC, and the video is DECODABLE.
q $V $A $BASE -c:a flac -shortest -f matroska "$OUT/h264-flac.mkv"

# 10. WebM: VP9 + Opus. NEITHER has a decoder here. Same reason as the AAC
#     file -- the container must open, and say so.
q $V $A -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 100k \
      -c:a libopus -b:a 48k -shortest -f webm "$OUT/vp9-opus.webm"

# 11. Audio-only Matroska (.mka): no video track at all, which is the case a
#     player's "find the video track" path has to survive.
q $A $MP3 -f matroska "$OUT/mp3.mka"

# 12. Laced Matroska. ffmpeg's muxer never writes lacing, so this one is built
#     by hand -- see tests/unit/gen_laced.py for why that is a feature: the
#     frame boundaries are known BY CONSTRUCTION rather than by asking the same
#     library we are checking.
python3 tests/unit/gen_laced.py "$OUT" >/dev/null

echo "media fixtures written to $OUT"
ls -l "$OUT" | tail -n +2
