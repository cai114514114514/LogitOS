#!/usr/bin/env bash
# tests/unit/aac_gen.sh -- build the AAC differential corpus with ffmpeg.
#
# Kept separate from tests/unit/audio_gen.sh, which belongs to the WAV/FLAC/MP3
# corpus and carries its own stamp: adding cases to that file would force every
# existing target to regenerate its whole corpus, and the two are checked
# against different criteria anyway.
#
# Everything here is generated, never committed, for the reason audio_gen.sh
# gives: a fixture cannot silently rot into agreement with a bug if it is
# rebuilt from a tool that knows nothing about our decoder. (tests/fixtures/
# audio/ separately holds ONE committed sample per format so the on-device
# harness works without ffmpeg.)
#
# -aac_pns 0 IS LOAD-BEARING AND IS NOT A WAY OF AVOIDING A HARD CASE.
# Perceptual noise substitution transmits a band's ENERGY and lets the decoder
# fill it with noise of its own making. Two conformant decoders therefore
# produce completely different samples in a PNS band, on purpose. Comparing
# them sample-for-sample measures nothing at all -- with PNS left on (which is
# ffmpeg's default) the pink-noise case disagrees by 0.1 out of 0.25 full
# scale, and that number is not a decoder bug, it is two different noises. So
# the differential corpus is generated without PNS, and a separate pns.aac IS
# generated with it, decoded, and checked for the things that ARE well defined
# there: that it decodes, that it produces the right number of frames, and
# that it is deterministic run to run.

set -eu

OUT="${1:?usage: aac_gen.sh <outdir>}"
FF="${FFMPEG:-ffmpeg}"
mkdir -p "$OUT"

STAMP="$OUT/.stamp-aac-v1"
if [ -f "$STAMP" ]; then exit 0; fi

src() {   # src <name> <ffmpeg input args...>
    name="$1"; shift
    "$FF" -y -loglevel error "$@" -c:a pcm_s16le "$OUT/$name.wav"
}

# --- source signals -------------------------------------------------------
# Tones exercise the filter bank's steady state and the long window; noise and
# impulses force the block switch to EIGHT_SHORT and the window grouping;
# a quiet signal drives the scalefactors to one end of their range.
src sine    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2"
src sweep   -f lavfi -i "aevalsrc='sin(2*PI*(200+3000*t)*t)':s=44100:d=2"
src noise   -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.5"
src impulse -f lavfi -i "aevalsrc='if(eq(floor(t*8),t*8),0.9,0.0001*random(0))':s=44100:d=2"
src quiet   -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=2" -af "volume=-40dB"
src loud    -f lavfi -i "anoisesrc=d=2:c=white:r=44100:a=0.95"
src stereo  -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
            -f lavfi -i "sine=frequency=997:sample_rate=44100:duration=2" \
            -filter_complex "[0:a][1:a]amerge=inputs=2[a]" -map "[a]"
src wide    -f lavfi -i "anoisesrc=d=2:c=white:r=44100:a=0.5" \
            -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.5:seed=7" \
            -filter_complex "[0:a][1:a]amerge=inputs=2[a]" -map "[a]"
src sr48    -f lavfi -i "anoisesrc=d=1:c=white:r=48000:a=0.4"
src sr32    -f lavfi -i "sine=frequency=440:sample_rate=32000:duration=1"
src sr24    -f lavfi -i "anoisesrc=d=1:c=pink:r=24000:a=0.4"
src sr22    -f lavfi -i "sine=frequency=800:sample_rate=22050:duration=1"
src sr16    -f lavfi -i "anoisesrc=d=1:c=white:r=16000:a=0.4"
src sr11    -f lavfi -i "sine=frequency=300:sample_rate=11025:duration=1"
src sr8     -f lavfi -i "anoisesrc=d=1:c=pink:r=8000:a=0.4"
src sr96    -f lavfi -i "anoisesrc=d=1:c=white:r=96000:a=0.4"
src sr88    -f lavfi -i "sine=frequency=1000:sample_rate=88200:duration=1"
src sr64    -f lavfi -i "anoisesrc=d=1:c=pink:r=64000:a=0.4"
src sr12    -f lavfi -i "sine=frequency=400:sample_rate=12000:duration=1"
src sr7     -f lavfi -i "anoisesrc=d=1:c=white:r=7350:a=0.4"

# --- encode, then decode that exact file with ffmpeg for the reference ----
enc() {   # enc <name> <src> <extra encoder args...>
    name="$1"; s="$2"; shift 2
    "$FF" -y -loglevel error -i "$OUT/$s.wav" -c:a aac -aac_pns 0 "$@" \
          -f adts "$OUT/$name.aac"
    "$FF" -y -loglevel error -i "$OUT/$name.aac" -f f32le "$OUT/$name.f32"
}

enc sine    sine    -b:a 128k
enc sweep   sweep   -b:a 192k
enc noise   noise   -b:a 128k
enc impulse impulse -b:a 320k
enc quiet   quiet   -b:a 64k
# The escape codebook (11) only appears when a coefficient quantises above 15,
# which needs both loud content and enough bits to code it.
enc loud    loud    -b:a 320k
enc stereo  stereo  -b:a 128k
enc wide    wide    -b:a 96k
enc mono    sine    -ac 1 -b:a 96k
enc lowrate wide    -b:a 32k
enc sr48    sr48    -b:a 256k
enc sr32    sr32    -b:a 128k
enc sr24    sr24    -b:a 80k
enc sr22    sr22    -b:a 64k
enc sr16    sr16    -b:a 48k
enc sr11    sr11    -b:a 32k
enc sr8     sr8     -b:a 24k
enc sr96    sr96    -b:a 256k
enc sr88    sr88    -b:a 256k
enc sr64    sr64    -b:a 192k
enc sr12    sr12    -b:a 32k
enc sr7     sr7     -b:a 20k

# The rate-control loops choose different codebooks and different block
# switching for the same input, so each one reaches syntax the others do not.
# (This ffmpeg build offers twoloop and fast; the older `anmr` coder is gone.)
enc coder_fast    wide -b:a 128k -aac_coder fast
enc coder_twoloop wide -b:a 128k -aac_coder twoloop

# Forcing M/S on and TNS off, and the reverse, so neither tool's default hides
# a path. -aac_pce writes a program_config_element and sets
# channel_configuration to 0, which is the only way that element and the
# "geometry comes from the PCE, not the header" path get exercised.
enc forced_ms  wide -b:a 128k -aac_ms 1 -aac_tns 0
enc no_is      wide -b:a 128k -aac_is 0
enc with_pce   stereo -b:a 128k -aac_pce 1

# 5.1: SCE + CPE + CPE + LFE in one raw_data_block, which is the only way the
# LFE element and a four-element block get exercised at all.
"$FF" -y -loglevel error -f lavfi -i "anoisesrc=d=1:c=pink:r=48000:a=0.4" \
      -af "pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0" -c:a pcm_s16le "$OUT/s51.wav"
"$FF" -y -loglevel error -i "$OUT/s51.wav" -c:a aac -aac_pns 0 -b:a 384k \
      -f adts "$OUT/mc51.aac"
"$FF" -y -loglevel error -i "$OUT/mc51.aac" -f f32le "$OUT/mc51.f32"

# WITH PNS, deliberately. Not sample-compared -- see the header -- but decoded,
# so the PNS path is exercised rather than merely written.
"$FF" -y -loglevel error -i "$OUT/noise.wav" -c:a aac -aac_pns 1 -b:a 96k \
      -f adts "$OUT/pns.aac"
"$FF" -y -loglevel error -i "$OUT/pns.aac" -f f32le "$OUT/pns.f32"

touch "$STAMP"
