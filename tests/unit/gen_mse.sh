#!/bin/bash
# tests/unit/gen_mse.sh -- build the MSE fixture: a DASH-shaped stream.
#
# What a real site delivers, and therefore what the MSE gate has to be driven
# with. Measured on bilibili with a headless Chrome: the DASH manifest offers
# separate video/mp4 and audio/mp4 representations, and the player fetches an
# INIT segment (ftyp + moov, no media) followed by numbered MEDIA segments
# (moof + mdat) and appends them to two SourceBuffers on one <video>. Nothing
# in this tree produced files of that shape before -- tests/fixtures/media has
# ONE fragmented mp4 and it is a single whole file.
#
#     bash tests/unit/gen_mse.sh tests/fixtures/mse
#
# Exit 3 means "no ffmpeg": the caller then runs the committed fixtures only,
# the same contract tests/unit/gen_media.sh has.
#
# THE CODEC CHOICE IS THE POINT, not an incidental. bilibili's manifest offers
# avc1.640033 -- H.264 HIGH profile, level 5.1 -- and Chrome takes AV1 instead
# only because Chrome can. The whole reason MediaSource.isTypeSupported() has
# to be honest here is that answering "no" to av01 and "yes" to avc1.64 is what
# makes a real site hand us the stream we can decode. So the fixture is
# encoded HIGH profile, with B frames and CABAC and the 8x8 transform on --
# i.e. the things High profile actually means -- because a fixture encoded
# baseline would prove the answer "yes to avc1.64" without ever testing it.
set -u
OUT=${1:-tests/fixtures/mse}

command -v ffmpeg >/dev/null 2>&1 || exit 3
rm -rf "$OUT"
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

q() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# Deterministic sources, no input file. Small on purpose: this is committed,
# and a from-scratch H.264 decoder under TCG pays for every macroblock.
V="-f lavfi -i testsrc2=size=128x96:rate=15:duration=4"
A="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=4"

# HIGH profile: CABAC, B frames, 8x8 transform. -bf 2 makes decode order differ
# from display order, which is exactly the case a decode-order PTS FIFO gets
# wrong -- see the note about h264_decode_pts in c/apps/browser/js_media.h.
HIGH="-c:v libx264 -profile:v high -level 4.0 -preset veryslow -bf 2 \
      -pix_fmt yuv420p -g 15 -keyint_min 15 -sc_threshold 0"
AAC="-c:a aac -b:a 64k -ar 44100 -ac 2"

# One second per segment, so a 4 s clip is 4 media segments per track: enough
# for "append segment 3 while segment 1 is playing" to be a real sequence and
# not a single append wearing a streaming costume.
q $V $A $HIGH $AAC -shortest \
  -f dash -seg_duration 1 -use_timeline 0 -use_template 1 \
  -init_seg_name 'init-$RepresentationID$.m4s' \
  -media_seg_name 'seg-$RepresentationID$-$Number$.m4s' \
  -single_file 0 "$TMP/x.mpd"

# ffmpeg names the video representation 0 and the audio 1 for this input order.
# Renamed to say what they are, because "stream0" is a fact about ffmpeg's
# argument parsing and "video" is a fact about the file.
cp "$TMP/init-0.m4s" "$OUT/init-video.mp4"
cp "$TMP/init-1.m4s" "$OUT/init-audio.mp4"
n=0
for f in "$TMP"/seg-0-*.m4s; do n=$((n+1)); cp "$f" "$OUT/video-$n.m4s"; done
vsegs=$n
n=0
for f in "$TMP"/seg-1-*.m4s; do n=$((n+1)); cp "$f" "$OUT/audio-$n.m4s"; done
asegs=$n

# The reference whole file: init + every media segment concatenated IS a valid
# fragmented MP4, and that identity is the load-bearing one for the appendBuffer
# path (see msebuf in js_media_src.c). Committing it lets the test assert that
# the incremental path and the whole-file path produce the SAME samples, which
# is a comparison rather than a claim.
cat "$OUT/init-video.mp4" "$OUT"/video-*.m4s > "$OUT/whole-video.mp4"
cat "$OUT/init-audio.mp4" "$OUT"/audio-*.m4s > "$OUT/whole-audio.mp4"

# THE OTHER REPRESENTATION IN THE MANIFEST. bilibili offers AV1 alongside H.264
# and Chrome takes it. We have no AV1 decoder, so this exists to be REFUSED --
# it is the input to the negative control (make test-mse-negctl), where a build
# whose isTypeSupported claims av01 accepts these bytes, appends them, and
# produces no picture at all. Without a real av01 fixture that control would be
# asserting on a string rather than on a consequence.
AV1TMP=$(mktemp -d)
if q -f lavfi -i testsrc2=size=128x96:rate=15:duration=2 \
     -c:v libsvtav1 -preset 12 -crf 50 -pix_fmt yuv420p -g 15 \
     -f dash -seg_duration 1 -use_timeline 0 -use_template 1 \
     -init_seg_name 'init-$RepresentationID$.m4s' \
     -media_seg_name 'seg-$RepresentationID$-$Number$.m4s' \
     -single_file 0 "$AV1TMP/a.mpd" 2>/dev/null; then
  cp "$AV1TMP/init-0.m4s" "$OUT/init-av1.mp4"
  n=0
  for f in "$AV1TMP"/seg-0-*.m4s; do n=$((n+1)); cp "$f" "$OUT/av1-$n.m4s"; done
  cat "$OUT/init-av1.mp4" "$OUT"/av1-*.m4s > "$OUT/whole-av1.mp4"
  av1codec=$(ffprobe -v error -select_streams v:0 \
             -show_entries stream=codec_name -of default=nw=1:nk=1 "$OUT/whole-av1.mp4")
  echo "gen_mse: av1 representation: $n segments ($av1codec)"
fi
rm -rf "$AV1TMP"

# The codec strings the manifest would carry, read back OUT OF THE FILES rather
# than written down here: the isTypeSupported table is tested against these, so
# a hand-typed string would be testing the test.
vcodec=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_tag_string,profile,level \
         -of default=nw=1:nk=1 "$OUT/whole-video.mp4" | tr '\n' ' ')
{
  echo "# generated by tests/unit/gen_mse.sh -- do not edit"
  echo "video_segments $vsegs"
  echo "audio_segments $asegs"
  echo "video_probe $vcodec"
  ffprobe -v error -show_entries stream=codec_name,profile,level,width,height,nb_frames \
          -of default=nw=1 "$OUT/whole-video.mp4" | sed 's/^/video_/'
  ffprobe -v error -show_entries stream=codec_name,profile,sample_rate,channels \
          -of default=nw=1 "$OUT/whole-audio.mp4" | sed 's/^/audio_/'
} > "$OUT/MANIFEST.txt"

echo "gen_mse: $vsegs video + $asegs audio segments in $OUT"
cat "$OUT/MANIFEST.txt"
