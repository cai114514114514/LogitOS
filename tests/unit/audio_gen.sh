#!/usr/bin/env bash
# tests/unit/audio_gen.sh -- build the audio test corpus with ffmpeg.
#
# Everything here is generated, never committed: the point of a differential
# test is that the reference and the input come from a tool that has no
# knowledge of our decoder, and re-generating them each run means a fixture
# cannot silently rot into agreement with a bug.
#
# For each source signal we emit:
#   <name>.wav          the original 16-bit PCM
#   <name>.mp3          encoded with libmp3lame, then
#   <name>.mp3.f32      ffmpeg's own float decode of that exact .mp3
#   <name>.flac         encoded losslessly, and
#   <name>.flac.s32     ffmpeg's decode of it, for a bit-exact comparison
#
# -write_xing 0 matters more than it looks. With a Xing/LAME header ffmpeg
# trims the encoder delay and the trailing padding when it decodes, so its
# output is a different length and a different alignment from every frame the
# file actually contains. A differential test needs both decoders to be
# decoding the same thing, so the header is suppressed and neither side trims.

set -eu

OUT="${1:?usage: audio_gen.sh <outdir>}"
FF="${FFMPEG:-ffmpeg}"
mkdir -p "$OUT"

# A stamp keeps re-runs cheap; delete the directory to force regeneration.
STAMP="$OUT/.stamp-v5"
if [ -f "$STAMP" ]; then exit 0; fi

gen() {   # gen <name> <ffmpeg input args...>
    name="$1"; shift
    "$FF" -y -loglevel error "$@" -c:a pcm_s16le "$OUT/$name.wav"
}

# --- source signals -------------------------------------------------------
# Sines and sweeps exercise the filter bank's steady state; noise exercises the
# Huffman escape paths and the short-block switch; a step function forces the
# window switch that a purely tonal signal never triggers.
gen sine440   -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2"
gen sweep     -f lavfi -i "aevalsrc='sin(2*PI*(200+3000*t)*t)':s=44100:d=2"
gen noise     -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.5"
gen stereo    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
              -f lavfi -i "sine=frequency=997:sample_rate=44100:duration=2" \
              -filter_complex "[0:a][1:a]amerge=inputs=2[a]" -map "[a]"
gen impulse   -f lavfi -i "aevalsrc='if(eq(floor(t*8),t*8),0.9,0.0001*random(0))':s=44100:d=2"
gen quiet     -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=2" -af "volume=-40dB"
gen sr32      -f lavfi -i "sine=frequency=440:sample_rate=32000:duration=1"
gen sr48      -f lavfi -i "anoisesrc=d=1:c=white:r=48000:a=0.4"
gen lsf22     -f lavfi -i "anoisesrc=d=1:c=pink:r=22050:a=0.4"
gen lsf24     -f lavfi -i "sine=frequency=800:sample_rate=24000:duration=1"
gen lsf16     -f lavfi -i "anoisesrc=d=1:c=white:r=16000:a=0.4"
gen mpeg25    -f lavfi -i "sine=frequency=300:sample_rate=11025:duration=1"
# The 8 kHz row is the one row of the band table whose short-block region0 is
# 72 lines rather than 36; nothing else reaches that branch.
gen sr8       -f lavfi -i "anoisesrc=d=1:c=pink:r=8000:a=0.4"
# Wideband stereo noise at a low bitrate: the case that keeps LAME in
# mid/side and switching blocks for most of the file.
gen msnoise   -f lavfi -i "anoisesrc=d=2:c=white:r=44100:a=0.5" \
              -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.5:seed=7" \
              -filter_complex "[0:a][1:a]amerge=inputs=2[a]" -map "[a]"

# --- MP3: one encode per case, plus ffmpeg's decode of that same file ------
enc_mp3() {   # enc_mp3 <name> <extra encoder args...>
    name="$1"; shift
    "$FF" -y -loglevel error -i "$OUT/$name.wav" -c:a libmp3lame "$@" \
          -write_xing 0 -id3v2_version 0 "$OUT/$name.mp3"
    "$FF" -y -loglevel error -i "$OUT/$name.mp3" -f f32le -acodec pcm_f32le \
          "$OUT/$name.mp3.f32"
}

enc_mp3 sine440 -b:a 128k
enc_mp3 sweep   -b:a 192k
enc_mp3 noise   -b:a 128k
enc_mp3 stereo  -b:a 128k -joint_stereo 1
enc_mp3 impulse -b:a 320k
enc_mp3 quiet   -b:a 64k
enc_mp3 sr32    -b:a 128k
enc_mp3 sr48    -b:a 256k
enc_mp3 lsf22   -b:a 64k
enc_mp3 lsf24   -b:a 80k
enc_mp3 lsf16   -b:a 48k
enc_mp3 mpeg25  -b:a 32k
enc_mp3 sr8     -b:a 24k
enc_mp3 msnoise -b:a 96k -joint_stereo 1

# Mono, and a VBR file, and a dual-channel (non-joint) stereo file.
"$FF" -y -loglevel error -i "$OUT/sine440.wav" -ac 1 -c:a libmp3lame -b:a 96k \
      -write_xing 0 -id3v2_version 0 "$OUT/mono.mp3"
"$FF" -y -loglevel error -i "$OUT/mono.mp3" -f f32le "$OUT/mono.mp3.f32"
"$FF" -y -loglevel error -i "$OUT/noise.wav" -c:a libmp3lame -q:a 2 \
      -write_xing 0 -id3v2_version 0 "$OUT/vbr.mp3"
"$FF" -y -loglevel error -i "$OUT/vbr.mp3" -f f32le "$OUT/vbr.mp3.f32"
"$FF" -y -loglevel error -i "$OUT/stereo.wav" -c:a libmp3lame -b:a 128k \
      -joint_stereo 0 -write_xing 0 -id3v2_version 0 "$OUT/dual.mp3"
"$FF" -y -loglevel error -i "$OUT/dual.mp3" -f f32le "$OUT/dual.mp3.f32"

# --- FLAC: lossless, so the reference is the encoder's own input ----------
enc_flac() {   # enc_flac <name> <extra args...>
    name="$1"; shift
    "$FF" -y -loglevel error -i "$OUT/$name.wav" -c:a flac "$@" "$OUT/$name.flac"
    "$FF" -y -loglevel error -i "$OUT/$name.flac" -f s32le "$OUT/$name.flac.s32"
}

enc_flac sine440 -compression_level 5
enc_flac noise   -compression_level 12
enc_flac stereo  -compression_level 8
enc_flac impulse -compression_level 0
enc_flac quiet   -compression_level 12
enc_flac sr48    -compression_level 5

# All three stereo decorrelations, forced. Left over to "auto" the encoder
# picks whichever is cheapest for the content, which on these signals means two
# of the three paths are never decoded by any test.
for m in mid_side left_side right_side indep; do
    "$FF" -y -loglevel error -i "$OUT/stereo.wav" -c:a flac -ch_mode "$m" \
          "$OUT/f_$m.flac"
    "$FF" -y -loglevel error -i "$OUT/f_$m.flac" -f s32le "$OUT/f_$m.flac.s32"
done

# 24-bit and 8-bit FLAC, and a mono one: different bit depths take different
# paths through the wasted-bits and stereo-decorrelation code.
"$FF" -y -loglevel error -i "$OUT/noise.wav" -c:a flac -sample_fmt s32 \
      -bits_per_raw_sample 24 "$OUT/f24.flac"
"$FF" -y -loglevel error -i "$OUT/f24.flac" -f s32le "$OUT/f24.flac.s32"
"$FF" -y -loglevel error -i "$OUT/sine440.wav" -ac 1 -c:a flac "$OUT/fmono.flac"
"$FF" -y -loglevel error -i "$OUT/fmono.flac" -f s32le "$OUT/fmono.flac.s32"

# --- WAV variants ---------------------------------------------------------
"$FF" -y -loglevel error -i "$OUT/sine440.wav" -c:a pcm_u8      "$OUT/w8.wav"
"$FF" -y -loglevel error -i "$OUT/sine440.wav" -c:a pcm_s24le   "$OUT/w24.wav"
"$FF" -y -loglevel error -i "$OUT/sine440.wav" -c:a pcm_s32le   "$OUT/w32.wav"
"$FF" -y -loglevel error -i "$OUT/sine440.wav" -c:a pcm_f32le   "$OUT/wf32.wav"

touch "$STAMP"
