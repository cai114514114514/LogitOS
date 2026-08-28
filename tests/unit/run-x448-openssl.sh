#!/usr/bin/env bash
# X448 differential against OpenSSL 3.6+, byte for byte.
#
# WHY A DIFFERENTIAL AND NOT JUST THE RFC KAT. tests/unit/x448_test.c already
# pins two RFC 7748 vectors, the iterated self-composition test, and 498
# Wycheproof cases -- but every one of those is a FIXED input. This script
# generates fresh random scalars every run, on the OTHER side (openssl's
# RNG, not ours), so it is checking against inputs nobody hand-picked. Three
# properties, in the same "keygen / derive both directions" shape
# run-mlkem-openssl.sh uses for ML-KEM:
#
#   A KEYGEN   x448_base(openssl's priv) must equal openssl's own pub for the
#              same key. Exercises our base-point ladder against openssl's,
#              openssl's random priv as the only input.
#   B DERIVE   our x448(a_priv, b_pub) must equal openssl's derived shared
#              secret for that same (a,b) pair.
#   C DERIVE   the same check with a and b swapped -- catches an asymmetric
#              bug that only shows up when OUR side is the "b" position, the
#              kind of thing a single-direction check would miss entirely.
#
# A missing/incapable openssl is a SKIP (exit 0), not a failure -- a missing
# reference is not a regression in the code under test, the same rule
# test-wpt and test-mlkem-openssl apply to an absent corpus/capability.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL="${OPENSSL:-openssl}"
NTRIAL="${NTRIAL:-8}"
CONTROLS=0
[ "${1:-}" = "--controls" ] && CONTROLS=1

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" genpkey -algorithm X448 -out /dev/null 2>/dev/null || {
    echo "SKIP: this openssl cannot do X448 (needs 3.0+)"; exit 0; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$BUILD"

build() {   # build [-Dcontrol]
    $CC -O2 -Wall -Wextra ${1:+"$1"} -o "$BUILD/x448_cli" \
        "$ROOT/tests/unit/x448_cli.c" "$ROOT/c/crypto/pubkey/x448.c" \
        "$ROOT/c/crypto/pubkey/field448.c" -I"$ROOT/c/crypto/pubkey" \
        2>"$TMP/build.log"
}

# openssl's `pkey -text` prints priv/pub as colon-separated hex over several
# indented lines; this strips everything but hex digits from the block
# between the given label and the next label (or EOF).
extract_hex() {   # extract_hex <pemfile> <priv|pub>
    "$OPENSSL" pkey -in "$1" -noout -text 2>/dev/null | \
        awk -v want="$2:" '
            $0 ~ want { on=1; next }
            /^[a-z]+:/ { on=0 }
            on { print }
        ' | tr -d ' :\n'
}

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

run_suite() {
    pass=0; fail=0
    local CLI="$BUILD/x448_cli"
    for i in $(seq 1 "$NTRIAL"); do
        "$OPENSSL" genpkey -algorithm X448 -out "$TMP/a.pem" 2>/dev/null
        "$OPENSSL" genpkey -algorithm X448 -out "$TMP/b.pem" 2>/dev/null

        local apriv apub bpriv bpub
        apriv=$(extract_hex "$TMP/a.pem" priv); apub=$(extract_hex "$TMP/a.pem" pub)
        bpriv=$(extract_hex "$TMP/b.pem" priv); bpub=$(extract_hex "$TMP/b.pem" pub)
        if [ ${#apriv} -ne 112 ] || [ ${#apub} -ne 112 ] || \
           [ ${#bpriv} -ne 112 ] || [ ${#bpub} -ne 112 ]; then
            bad "A could not parse openssl's pkey -text output (trial $i)"; continue
        fi

        # A. keygen: our base-point ladder from openssl's priv == openssl's own pub
        local our_apub
        our_apub=$("$CLI" base "$apriv")
        if [ "$our_apub" = "$apub" ]; then ok; else bad "A keygen (trial $i)"; fi

        # B/C. derive both directions against openssl's -derive
        local ossl_shared our_ab our_ba
        ossl_shared=$("$OPENSSL" pkeyutl -derive -inkey "$TMP/a.pem" \
            -peerkey "$TMP/b.pem" 2>/dev/null | xxd -p | tr -d '\n')
        our_ab=$("$CLI" shared "$apriv" "$bpub")
        our_ba=$("$CLI" shared "$bpriv" "$apub")
        if [ "$our_ab" = "$ossl_shared" ]; then ok; else bad "B derive a->b (trial $i)"; fi
        if [ "$our_ba" = "$ossl_shared" ]; then ok; else bad "C derive b->a (trial $i)"; fi
    done
}

if [ "$CONTROLS" = 0 ]; then
    build || { echo "FAIL: could not build x448_cli"; cat "$TMP/build.log"; exit 1; }
    run_suite
    echo "X448 vs $($OPENSSL version | cut -d' ' -f1-2): $pass passed, $fail failed" \
         "($NTRIAL trials x 3 checks)"
    [ "$fail" -eq 0 ]
    exit $?
fi

# ------------------------------------------------------------- the controls --
echo "== X448 negative controls: each MUST fail =="
build || { echo "FAIL: baseline build"; cat "$TMP/build.log"; exit 1; }
NTRIAL=3 run_suite
base_fail=$fail
echo "  baseline (no control): $pass passed, $fail failed"
[ "$base_fail" -eq 0 ] || { echo "FAIL: the baseline is not green; controls mean nothing"; exit 1; }

ctl_bad=0
for CTL in LOGIT_X448_CTL_BAD_A24 LOGIT_X448_CTL_BAD_CLAMP; do
    if ! build "-D$CTL"; then
        echo "  $CTL: BUILD FAILED (a control must compile)"; cat "$TMP/build.log"
        ctl_bad=$((ctl_bad+1)); continue
    fi
    NTRIAL=3 run_suite
    if [ "$fail" -gt 0 ]; then
        echo "  $CTL: correctly RED ($fail of $((pass+fail)) checks failed)"
    else
        echo "  $CTL: PASSED -- the suite cannot see this defect"
        ctl_bad=$((ctl_bad+1))
    fi
done
build   # leave the honest binary behind
if [ "$ctl_bad" -eq 0 ]; then
    echo "all 2 controls fired"; exit 0
else
    echo "$ctl_bad control(s) did not fire"; exit 1
fi
