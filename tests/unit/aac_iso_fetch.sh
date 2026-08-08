#!/usr/bin/env bash
# tests/unit/aac_iso_fetch.sh -- fetch the OFFICIAL ISO/IEC 14496-4 AAC
# conformance bitstreams and reference waveforms.
#
# THIS IS THE REAL SUITE, NOT A DIFFERENTIAL.  ISO publishes the MPEG-4 audio
# conformance package under Publicly Available Standards, and it contains both
# the conformance bitstreams and the reference decoder's output waveform for
# each one. That turns "our AAC agrees with ffmpeg" into "our AAC meets the
# standard's own criterion against the standard's own reference", which is a
# different and much stronger claim -- and it is the claim the MP3 line could
# not make, because ISO's public download for 11172-4 returns an HTML error
# page (that line downloaded it and checked the bytes; this one downloads real
# streams and checks those).
#
# NOTHING FETCHED HERE IS COMMITTED.  The ISO licence permits use of the
# electronic inserts in their original form for the purposes of the standard;
# it does not permit redistribution, and the package is hundreds of megabytes
# besides. So this script is opt-in, the files land in $(BUILD), and
# `make test-aac-conformance` says clearly what to run when they are absent
# rather than silently passing.
#
# WHY ffmpeg APPEARS HERE AND WHAT IT IS NOT DOING.  The normative streams are
# carried in MP4 and in LATM/LOAS; this decoder reads ADTS and raw blocks with
# an AudioSpecificConfig, which is what a demuxer hands over. `ffmpeg -c:a
# copy` REPACKAGES the MP4 into ADTS without touching a single byte of AAC
# payload -- it is being used as the container demuxer this project does not
# have yet, not as a decoder. The samples that reach our decoder are ISO's
# bits, and the reference they are scored against is ISO's waveform. ffmpeg's
# own AAC decoder is not involved at any point.
#
#   ./tests/unit/aac_iso_fetch.sh build/isoaac [N]
#
# N limits how many cases to fetch (default 40); pass 0 for all of them.

set -eu

OUT="${1:?usage: aac_iso_fetch.sh <outdir> [count]}"
LIMIT="${2:-40}"
BASE="https://standards.iso.org/ittf/PubliclyAvailableStandards/ISO_IEC_14496-4_2004_Conformance_Testing/audio_conformance/mpeg4audio-conformance"
FF="${FFMPEG:-ffmpeg}"

mkdir -p "$OUT"
STAMP="$OUT/.stamp-iso-$LIMIT"
if [ -f "$STAMP" ]; then exit 0; fi

# The AAC-LC cases are the ones named al*. Everything else in the package is a
# profile this decoder does not claim: am* is Main (backward prediction), ap*
# is LTP, as* is scalable, er_* is the error-resilient syntax, and each is
# refused rather than decoded.
echo "aac_iso_fetch: listing the ISO conformance package..."
curl -fsS --max-time 180 "$BASE/compressedMp4/" -o "$OUT/.mp4list.html"
grep -oE 'href="[^"]*\.mp4"' "$OUT/.mp4list.html" |
    sed 's|.*/||; s|"||' | grep '^al' | sort > "$OUT/.cases.txt"

total=$(wc -l < "$OUT/.cases.txt")
echo "aac_iso_fetch: $total AAC-LC cases published"

# The reference waveform for a case is not always <case>.wav: several carry a
# suffix describing the encoder settings (al08_48_s06.wav for al08_48.mp4), so
# the listing is fetched once and matched by prefix instead of guessed.
curl -fsS --max-time 180 "$BASE/referencesWav/" -o "$OUT/.wavlist.html"
grep -oE 'href="[^"]*\.wav"' "$OUT/.wavlist.html" |
    sed 's|.*/||; s|"||' | sort > "$OUT/.wavs.txt"

# EXACT names only. Several multichannel cases publish their reference one
# channel per file (al08_48_s06.wav for al08_48), and matching those by prefix
# means comparing a six-channel decode against one channel of it -- which is
# not a conformance failure, it is a harness that measured the wrong thing.
# Those cases are skipped and counted, not silently mis-scored.
ref_for() {   # ref_for <case>  -> prints the published wav name, or nothing
    grep -E "^$1\.wav$" "$OUT/.wavs.txt" | head -1
}

# Spread the selection across the whole list rather than taking the first N:
# the names sort by test number then by sampling rate, so the head of the list
# is one test at twelve rates and tells you far less than twelve tests do.
if [ "$LIMIT" -gt 0 ] && [ "$total" -gt "$LIMIT" ]; then
    step=$(( (total + LIMIT - 1) / LIMIT ))
    awk -v s="$step" 'NR % s == 1' "$OUT/.cases.txt" > "$OUT/.sel.txt"
else
    cp "$OUT/.cases.txt" "$OUT/.sel.txt"
fi

n=0
ok=0
while read -r f; do
    base="${f%.mp4}"
    n=$((n + 1))
    if [ -f "$OUT/$base.adts" ] && [ -f "$OUT/$base.wav" ]; then ok=$((ok+1)); continue; fi
    if ! curl -fsS --max-time 180 "$BASE/compressedMp4/$f" -o "$OUT/$base.mp4"; then
        echo "  skip $base (bitstream not fetchable)"; continue
    fi
    ref="$(ref_for "$base")"
    if [ -z "$ref" ]; then
        echo "  skip $base (reference published only per-channel)"
        rm -f "$OUT/$base.mp4"; continue
    fi
    if ! curl -fsS --max-time 180 "$BASE/referencesWav/$ref" -o "$OUT/$base.wav"; then
        echo "  skip $base (reference waveform $ref not fetchable)"
        rm -f "$OUT/$base.mp4"; continue
    fi
    # Repackage only. -c:a copy does not re-encode; -bsf:a aac_adtstoasc is the
    # other direction and is deliberately NOT used.
    if ! "$FF" -y -loglevel error -i "$OUT/$base.mp4" -c:a copy -f adts "$OUT/$base.adts"; then
        echo "  skip $base (could not be repackaged to ADTS)"; rm -f "$OUT/$base.mp4"; continue
    fi
    rm -f "$OUT/$base.mp4"
    ok=$((ok + 1))
done < "$OUT/.sel.txt"

echo "aac_iso_fetch: $ok of $n cases ready in $OUT"
touch "$STAMP"
