#!/usr/bin/env bash
# tests/unit/vorbis_gen.sh -- build the Vorbis differential corpus with ffmpeg.
#
# READ tests/unit/vorbis_test.c BEFORE QUOTING ANY NUMBER THIS PRODUCES.
# Vorbis I defines no numeric conformance criterion for decoder output and Xiph
# publishes no conformance bitstream suite, so what this corpus supports is a
# DIFFERENTIAL -- how far two implementations are apart on the same bytes --
# and not a conformance claim. AAC and MP3 have published bounds and are
# measured against them; Vorbis has none and is not.
#
# Generated, never committed, for the reason audio_gen.sh gives.

set -eu

OUT="${1:?usage: vorbis_gen.sh <outdir>}"
FF="${FFMPEG:-ffmpeg}"
mkdir -p "$OUT"

STAMP="$OUT/.stamp-vorbis-v1"
if [ -f "$STAMP" ]; then exit 0; fi

src() {
    name="$1"; shift
    "$FF" -y -loglevel error "$@" -c:a pcm_s16le "$OUT/$name.wav"
}

# Tones for the steady state; noise and impulses to force the short block and
# the mode switch; a two-uncorrelated-channel case so the stereo COUPLING gets
# used (a correlated pair would leave the angle channel near zero and the
# inverse-coupling quadrant logic untested).
src sine    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2"
src sweep   -f lavfi -i "aevalsrc='sin(2*PI*(200+3000*t)*t)':s=44100:d=2"
src noise   -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.5"
src impulse -f lavfi -i "aevalsrc='if(eq(floor(t*8),t*8),0.9,0.0001*random(0))':s=44100:d=2"
src quiet   -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=2" -af "volume=-40dB"
src stereo  -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.5" \
            -f lavfi -i "anoisesrc=d=2:c=white:r=44100:a=0.5:seed=3" \
            -filter_complex "[0:a][1:a]amerge=inputs=2[a]" -map "[a]"
src tonal   -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
            -f lavfi -i "sine=frequency=441:sample_rate=44100:duration=2" \
            -filter_complex "[0:a][1:a]amerge=inputs=2[a]" -map "[a]"
src sr48    -f lavfi -i "anoisesrc=d=1:c=white:r=48000:a=0.4"
src sr32    -f lavfi -i "sine=frequency=440:sample_rate=32000:duration=1"
src sr22    -f lavfi -i "anoisesrc=d=1:c=pink:r=22050:a=0.4"
src sr16    -f lavfi -i "sine=frequency=800:sample_rate=16000:duration=1"
src sr8     -f lavfi -i "anoisesrc=d=1:c=pink:r=8000:a=0.4"

enc() {   # enc <name> <src> <extra args...>
    name="$1"; s="$2"; shift 2
    "$FF" -y -loglevel error -i "$OUT/$s.wav" -c:a libvorbis "$@" "$OUT/$name.ogg"
    "$FF" -y -loglevel error -i "$OUT/$name.ogg" -f f32le "$OUT/$name.f32"
}

# The quality setting decides the codebooks, the block sizes and how often the
# encoder switches, so the sweep over -q:a is coverage rather than decoration:
# q=-1 and q=10 pick different setup templates entirely.
enc sine    sine    -q:a 5
enc sweep   sweep   -q:a 6
enc noise   noise   -q:a 4
enc impulse impulse -q:a 8
enc quiet   quiet   -q:a 2
enc stereo  stereo  -q:a 4
enc tonal   tonal   -q:a 6
enc mono    sine    -ac 1 -q:a 3
enc lowq    noise   -q:a -1
enc highq   noise   -q:a 10
enc sr48    sr48    -q:a 5
enc sr32    sr32    -q:a 4
enc sr22    sr22    -q:a 3
enc sr16    sr16    -q:a 2
enc sr8     sr8     -q:a 1

# A bitrate-targeted encode as well: libvorbis uses a different rate-management
# path for -b:a than for -q:a.
enc cbr     noise   -b:a 128k

touch "$STAMP"
