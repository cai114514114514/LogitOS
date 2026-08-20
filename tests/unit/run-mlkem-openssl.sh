#!/usr/bin/env bash
# ML-KEM-768 differential against OpenSSL 3.5+, byte for byte.
#
# WHY A DIFFERENTIAL AND NOT A SELF-TEST. Three of the five defects this file's
# controls inject produce an implementation that is entirely self-consistent --
# it generates a key, encapsulates to it and decapsulates back correctly, every
# time -- and interoperates with nothing. A round-trip test cannot see any of
# them. Only a second, independently written implementation can, and openssl is
# that. This is the same argument c/crypto's 140,214 differential cases rest on.
#
# Five properties, which fail in different places:
#   A KEYGEN   our ek from seed (d,z) must BE openssl's ek from the same seed.
#              Exercises G, matrix expansion + rejection sampling, CBD, the NTT
#              and ByteEncode in one comparison.
#   B DECAPS   openssl encapsulates to our key; we must recover ITS secret.
#   C ENCAPS   we encapsulate to openssl's key; openssl must recover OURS.
#   D REJECT   a corrupted ciphertext must yield a pseudorandom secret rather
#              than an error, and openssl must yield THE SAME one. Two sides
#              agreeing on a value neither can predict is the only real evidence
#              the Fujisaki-Okamoto transform matches; it is also the property
#              whose absence would make this a decryption oracle.
#   E ALIVE    the rejected secret must differ from the true one, or "rejection"
#              did nothing and D would pass trivially.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL="${OPENSSL:-openssl}"
NTRIAL="${NTRIAL:-8}"
CONTROLS=0
[ "${1:-}" = "--controls" ] && CONTROLS=1

# A missing reference is not a regression in the code under test.
command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" list -kem-algorithms 2>/dev/null | grep -qi ML-KEM-768 || {
    echo "SKIP: this openssl has no ML-KEM-768 (needs 3.5+); the FIPS 203"
    echo "      known answers in tests/unit/mlkem_kat.inc still gate the code"
    echo "      via 'make test-mlkem'."; exit 0; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$BUILD"

build() {   # build [-Dcontrol]
    $CC -O2 -Wall -Wextra ${1:+"$1"} -o "$BUILD/mlkem_cli" \
        "$ROOT/tests/unit/mlkem_cli.c" "$ROOT/c/crypto/pq/mlkem.c" \
        "$ROOT/c/crypto/pq/keccak.c" -I"$ROOT/c/crypto/pq" 2>"$TMP/build.log"
}

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

run_suite() {
    pass=0; fail=0
    local CLI="$BUILD/mlkem_cli"
    for i in $(seq 1 "$NTRIAL"); do
        local d z ossl_ek our_ek ct ss_ossl dk ss_ours m out our_ct our_ss ss_back
        d=$(head -c 32 /dev/urandom | xxd -p | tr -d '\n')
        z=$(head -c 32 /dev/urandom | xxd -p | tr -d '\n')

        # A. keygen, byte for byte
        "$OPENSSL" genpkey -algorithm ML-KEM-768 -pkeyopt hexseed:"${d}${z}" \
            -out "$TMP/k.pem" 2>/dev/null
        "$OPENSSL" pkey -in "$TMP/k.pem" -pubout -outform DER -out "$TMP/pub.der" 2>/dev/null
        ossl_ek=$(tail -c 1184 "$TMP/pub.der" | xxd -p | tr -d '\n')
        our_ek=$("$CLI" keygen "$d" "$z" ek)
        if [ "$our_ek" = "$ossl_ek" ]; then ok; else bad "A keygen (trial $i)"; fi

        # B. openssl encapsulates to that key; we decapsulate
        "$OPENSSL" pkeyutl -encap -inkey "$TMP/k.pem" -pubin \
            -out "$TMP/ct.bin" -secret "$TMP/ss.bin" 2>/dev/null
        if [ -s "$TMP/ct.bin" ]; then
            ct=$(xxd -p "$TMP/ct.bin" | tr -d '\n')
            ss_ossl=$(xxd -p "$TMP/ss.bin" | tr -d '\n')
            dk=$("$CLI" keygen "$d" "$z" dk)
            ss_ours=$("$CLI" decaps "$dk" "$ct")
            if [ "$ss_ours" = "$ss_ossl" ]; then ok; else bad "B decaps (trial $i)"; fi
        else bad "B openssl encap produced nothing (trial $i)"; dk=$("$CLI" keygen "$d" "$z" dk); fi

        # C. we encapsulate to openssl's key; openssl decapsulates
        m=$(head -c 32 /dev/urandom | xxd -p | tr -d '\n')
        out=$("$CLI" encaps "$ossl_ek" "$m")
        our_ct=$(printf '%s\n' "$out" | sed -n 1p)
        our_ss=$(printf '%s\n' "$out" | sed -n 2p)
        printf '%s\n' "$our_ct" | xxd -r -p > "$TMP/ourct.bin"
        "$OPENSSL" pkeyutl -decap -inkey "$TMP/k.pem" -in "$TMP/ourct.bin" \
            -secret "$TMP/back.bin" 2>/dev/null
        ss_back=$(xxd -p "$TMP/back.bin" 2>/dev/null | tr -d '\n')
        if [ -n "$ss_back" ] && [ "$our_ss" = "$ss_back" ]; then ok; else bad "C encaps (trial $i)"; fi

        # D + E. implicit rejection, in c1 (the u part) and c2 (the v part).
        # c1 is bytes 0..959, c2 is 960..1087 -- the two decode differently, so
        # both are corrupted rather than trusting one to stand for the other.
        for off in 5 1000; do
            local bad_ct ours_rej ossl_rej
            bad_ct=$(python3 -c "
import sys
ct=bytearray.fromhex(sys.argv[1]); ct[int(sys.argv[2])]^=0x40; print(ct.hex())" "$our_ct" "$off")
            printf '%s\n' "$bad_ct" | xxd -r -p > "$TMP/badct.bin"
            ours_rej=$("$CLI" decaps "$dk" "$bad_ct")
            "$OPENSSL" pkeyutl -decap -inkey "$TMP/k.pem" -in "$TMP/badct.bin" \
                -secret "$TMP/rej.bin" 2>/dev/null
            ossl_rej=$(xxd -p "$TMP/rej.bin" 2>/dev/null | tr -d '\n')
            if [ -z "$ossl_rej" ]; then
                bad "D openssl refused a corrupted ct -- expected implicit rejection"; continue; fi
            if [ "$ours_rej" != "$ossl_rej" ]; then
                bad "D implicit rejection differs (off=$off, trial $i)"; continue; fi
            ok
            if [ "$ours_rej" = "$our_ss" ]; then
                bad "E rejection returned the REAL secret (off=$off)"; else ok; fi
        done
    done
}

if [ "$CONTROLS" = 0 ]; then
    build || { echo "FAIL: could not build mlkem_cli"; cat "$TMP/build.log"; exit 1; }
    run_suite
    echo "ML-KEM-768 vs $($OPENSSL version | cut -d' ' -f1-2): $pass passed, $fail failed" \
         "($NTRIAL trials x 7 checks)"
    [ "$fail" -eq 0 ]
    exit $?
fi

# ------------------------------------------------------------- the controls --
echo "== ML-KEM negative controls: each MUST fail =="
build || { echo "FAIL: baseline build"; cat "$TMP/build.log"; exit 1; }
NTRIAL=2 run_suite
base_fail=$fail
echo "  baseline (no control): $pass passed, $fail failed"
[ "$base_fail" -eq 0 ] || { echo "FAIL: the baseline is not green; controls mean nothing"; exit 1; }

ctl_bad=0
for CTL in MLKEM_CTL_NO_DOMAIN_BYTE MLKEM_CTL_NIBBLE_SWAP MLKEM_CTL_SKIP_D2 \
           MLKEM_CTL_NO_TRANSPOSE MLKEM_CTL_NO_IMPLICIT_REJECT; do
    if ! build "-D$CTL"; then
        echo "  $CTL: BUILD FAILED (a control must compile)"; cat "$TMP/build.log"
        ctl_bad=$((ctl_bad+1)); continue
    fi
    NTRIAL=2 run_suite
    if [ "$fail" -gt 0 ]; then
        echo "  $CTL: correctly RED ($fail of $((pass+fail)) checks failed)"
    else
        echo "  $CTL: PASSED -- the suite cannot see this defect"
        ctl_bad=$((ctl_bad+1))
    fi
done
build   # leave the honest binary behind
if [ "$ctl_bad" -eq 0 ]; then
    echo "all 5 controls fired"; exit 0
else
    echo "$ctl_bad control(s) did not fire"; exit 1
fi
