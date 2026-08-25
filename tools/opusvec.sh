#!/bin/sh
# tools/opusvec.sh -- fetch and verify everything the Opus gate measures against.
#
# THREE ARTEFACTS, AND THEY ARE NOT INTERCHANGEABLE.  Getting this wrong is the
# single easiest way to produce an Opus decoder that passes a test suite and
# does not decode Opus, so each one is named with what it is FOR:
#
#   1. THE OFFICIAL TEST VECTORS (opus_testvectors.tar.gz, opus-codec.org).
#      Twelve .bit/.dec pairs. These are the conformance corpus RFC 6716
#      section 6 points at -- not a corpus this author generated, and not
#      ffmpeg's output. The .dec files are the reference decoder's own output
#      and are what opus_compare scores against.
#
#   2. THE REFERENCE IMPLEMENTATION, extracted FROM RFC 6716 ITSELF.  The RFC
#      embeds its entire reference decoder as base64 in Appendix A and prints
#      the SHA-1 of the resulting tarball in its own text (section A.1). That
#      is the normative source: the RFC says so in as many words -- "although
#      that implementation is expected to remain conformant with the standard,
#      it is the code in this RFC that shall remain normative". So the source
#      of the tables in c/lib/audio/opus_tables.h is not a copy of libopus
#      from somewhere with a plausible version number; it is the standard,
#      and the standard signs it.
#
#      NOTE THE HASH IS SHA-1 AND THAT IS NOT A CHOICE.  SHA-1 is the hash the
#      RFC printed in 2012 and it is the only one that can be checked against
#      the document. A SHA-256 recorded here would only be checkable against
#      this file, i.e. against nothing. The SHA-256s below are for the two
#      DOWNLOADS, where the point is "did the network give me the same bytes
#      as last time", a question SHA-1 answers less well.
#
#   3. opus_compare AND opus_demo, built from (2).  opus_compare is the
#      conformance metric itself (see the top of c/lib/audio/opus.c for why a
#      byte-comparison is the wrong bar for this one codec); opus_demo is what
#      produces a .dec from a .bit, and is therefore the second oracle for any
#      stream we generate ourselves with ffmpeg.
#
# WHY IT DOES NOT LIVE IN /tmp.  It did, for one afternoon, and that is the
# reason this script exists: /tmp is the WSL side's and is wiped, so a corpus
# fetched there makes a gate that passes today and reports "no vectors" next
# week with nothing in the tree explaining why. Everything lands under
# build/.opus/, which is gitignored (39 MB of corpus does not belong in git)
# but is on the same disk as the checkout.
#
# THE CORPUS IS OPTIONAL AND THE CAPABILITY IS NOT -- the rule tests/wpt.mk
# already argues. If the network is down, tests/opus.mk says so and exits 0,
# because a missing download is not a regression in the decoder under test.
# It is `make opus-vectors` that fetches; no test target downloads anything.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/.opus
VEC=$OUT/vectors
REF=$OUT/ref

VEC_URL=https://opus-codec.org/static/testvectors/opus_testvectors.tar.gz
VEC_SHA256=94ac78ca4f74c4e43bc9fe4ec1ad0aa36f38ab90f45b0727c40dd1e96096e767

RFC_URL=https://www.rfc-editor.org/rfc/rfc6716.txt
RFC_SHA256=41caac5240a4a22661efd0031d5b7aee48f3c0bde3b2cdcee8165932e485f98c

# Printed by RFC 6716 section A.1 itself, line "The SHA1 hash of the
# opus-rfc6716.tar.gz file is 86a9...". Checked, not assumed.
REF_SHA1=86a927223e73d2476646a1b933fcd3fffb6ecc8c

say() { echo "[opusvec] $*"; }

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "[opusvec] missing tool: $1" >&2; exit 1; }
}
need curl
need tar
need sha256sum
need sha1sum
need base64

mkdir -p "$OUT"

# ---------------------------------------------------------------- 1. vectors
if [ -f "$VEC/testvector12.dec" ]; then
    say "vectors already present: $VEC"
else
    say "fetching $VEC_URL (39 MB)"
    curl -fL --retry 3 -o "$OUT/opus_testvectors.tar.gz" "$VEC_URL"
    got=$(sha256sum "$OUT/opus_testvectors.tar.gz" | cut -d' ' -f1)
    if [ "$got" != "$VEC_SHA256" ]; then
        echo "[opusvec] test vector sha256 MISMATCH" >&2
        echo "  want $VEC_SHA256" >&2
        echo "  got  $got" >&2
        exit 1
    fi
    say "sha256 ok"
    rm -rf "$VEC"
    mkdir -p "$VEC"
    # The tarball holds an opus_testvectors/ directory; flatten it so the
    # path in tests/opus.mk does not depend on the packaging.
    tar xzf "$OUT/opus_testvectors.tar.gz" -C "$VEC" --strip-components=1
    say "extracted $(ls "$VEC" | wc -l) files to $VEC"
fi

# ------------------------------------------------- 2. reference, from the RFC
if [ -f "$REF/opus_compare" ] && [ -f "$REF/opus_demo" ]; then
    say "reference already built: $REF"
else
    if [ ! -f "$OUT/rfc6716.txt" ]; then
        say "fetching $RFC_URL"
        curl -fL --retry 3 -o "$OUT/rfc6716.txt" "$RFC_URL"
    fi
    got=$(sha256sum "$OUT/rfc6716.txt" | cut -d' ' -f1)
    if [ "$got" != "$RFC_SHA256" ]; then
        echo "[opusvec] rfc6716.txt sha256 MISMATCH (want $RFC_SHA256 got $got)" >&2
        exit 1
    fi

    # The extraction command is the RFC's own, quoted from its section A.1:
    #   cat rfc6716.txt | grep '^\ \ \ ###' | sed -e 's/...###//' | base64 --decode
    say "extracting the reference source embedded in the RFC"
    grep '^   ###' "$OUT/rfc6716.txt" | sed -e 's/...###//' | base64 -d \
        > "$OUT/opus-rfc6716.tar.gz"

    got=$(sha1sum "$OUT/opus-rfc6716.tar.gz" | cut -d' ' -f1)
    if [ "$got" != "$REF_SHA1" ]; then
        echo "[opusvec] reference tarball sha1 MISMATCH -- this is the hash the" >&2
        echo "          RFC prints for itself, so a mismatch means the extraction" >&2
        echo "          is wrong, not that upstream moved." >&2
        echo "  want $REF_SHA1" >&2
        echo "  got  $got" >&2
        exit 1
    fi
    say "sha1 ok ($REF_SHA1) -- this is the hash RFC 6716 prints for itself"

    rm -rf "$REF"
    mkdir -p "$REF"
    tar xzf "$OUT/opus-rfc6716.tar.gz" -C "$REF" --strip-components=1

    say "building reference opus_demo + opus_compare"
    # -O2 only. The reference's own Makefile defaults to the FLOAT build, which
    # is the one whose .dec output the shipped vectors were produced with; do
    # not "improve" this to FIXED_POINT, because then opus_compare would be
    # scoring our decoder against a reference that disagrees with the corpus.
    ( cd "$REF" && make -s CFLAGS="-O2 -w" >/dev/null )
    [ -x "$REF/opus_compare" ] || { echo "[opusvec] opus_compare did not build" >&2; exit 1; }
fi

# --------------------------------------------------------------- 3. inventory
say "ready:"
say "  vectors     $VEC ($(ls "$VEC"/*.bit 2>/dev/null | wc -l) .bit files)"
say "  opus_demo   $REF/opus_demo"
say "  opus_compare $REF/opus_compare"
