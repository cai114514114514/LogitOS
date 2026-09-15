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

# Homebrew's FFmpeg can omit libvorbis and expose only its native experimental
# Vorbis encoder. Do not use that as the fallback: FFmpeg 8.1's native encoder
# and decoder self-agree on stereo bytes whose second channel libvorbis decodes
# differently. A producer and oracle sharing that quirk is not a differential.
# Prefer FFmpeg+libvorbis when available; otherwise let libsndfile/libvorbis
# produce the Ogg bytes and retain FFmpeg as the independent PCM oracle.
if "$FF" -hide_banner -encoders 2>/dev/null | grep -q 'libvorbis'; then
    VORBIS_BACKEND=ffmpeg-libvorbis
else
    if ! python3 -c 'import numpy, soundfile; assert "VORBIS" in soundfile.available_subtypes("OGG")' 2>/dev/null; then
        echo "vorbis_gen: FFmpeg has no libvorbis and Python soundfile cannot encode Ogg/Vorbis" >&2
        exit 1
    fi
    VORBIS_BACKEND=libsndfile
    echo "vorbis_gen: FFmpeg has no libvorbis; libsndfile writes bytes, FFmpeg decodes the oracle" >&2
fi

STAMP="$OUT/.stamp-vorbis-v3"
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
src left    -f lavfi -i "aevalsrc='0.4*sin(2*PI*440*t)|0':s=44100:d=1"
src right   -f lavfi -i "aevalsrc='0|0.4*sin(2*PI*440*t)':s=44100:d=1"
src sr48    -f lavfi -i "anoisesrc=d=1:c=white:r=48000:a=0.4"
src sr32    -f lavfi -i "sine=frequency=440:sample_rate=32000:duration=1"
src sr22    -f lavfi -i "anoisesrc=d=1:c=pink:r=22050:a=0.4"
src sr16    -f lavfi -i "sine=frequency=800:sample_rate=16000:duration=1"
src sr8     -f lavfi -i "anoisesrc=d=1:c=pink:r=8000:a=0.4"

enc() {   # enc <name> <src> <quality 0..1> [channels]
    name="$1"; s="$2"; shift 2
    quality="$1"; channels="${2:-}"
    if [ "$VORBIS_BACKEND" = ffmpeg-libvorbis ]; then
        q=$(awk -v v="$quality" 'BEGIN { printf "%.3f", v * 11.0 - 1.0 }')
        if [ -n "$channels" ]; then
            "$FF" -y -loglevel error -i "$OUT/$s.wav" -c:a libvorbis \
                -q:a "$q" -ac "$channels" "$OUT/$name.ogg"
        else
            "$FF" -y -loglevel error -i "$OUT/$s.wav" -c:a libvorbis \
                -q:a "$q" "$OUT/$name.ogg"
        fi
    else
        if [ -z "$channels" ]; then channels=0; fi
        python3 tests/unit/vorbis_encode.py "$OUT/$s.wav" "$OUT/$name.ogg" \
            "$quality" "$channels"
    fi
    "$FF" -y -loglevel error -i "$OUT/$name.ogg" -f f32le "$OUT/$name.f32"
}

# The quality setting decides the codebooks, block sizes and mode switches, so
# this low-to-high sweep is coverage rather than decoration. The FFmpeg backend
# maps 0..1 onto libvorbis -q:a -1..10; libsndfile accepts 0..1 directly.
enc sine    sine    0.55
enc sweep   sweep   0.64
enc noise   noise   0.45
enc impulse impulse 0.82
enc quiet   quiet   0.27
enc stereo  stereo  0.45
enc tonal   tonal   0.64
enc left    left    0.55
enc right   right   0.55
enc mono    sine    0.36 1
enc lowq    noise   0.00
enc highq   noise   1.00
enc sr48    sr48    0.55
enc sr32    sr32    0.45
enc sr22    sr22    0.36
enc sr16    sr16    0.27
enc sr8     sr8     0.18

# The name is retained for corpus compatibility. libsndfile's public API does
# not expose Vorbis managed-bitrate mode, so on that fallback this is a
# mid-quality VBR encode rather than silently claiming CBR coverage.
enc cbr     noise   0.50

touch "$STAMP"
