#!/usr/bin/env bash
# ML-DSA (FIPS 204, all three parameter sets) differential against OpenSSL
# 3.6+, byte for byte.
#
# WHY A DIFFERENTIAL AND NOT JUST THE KAT (tests/unit/mldsa_test.c). The KAT
# already caught real bugs during development of THIS file's own controls
# (MLDSA_CTL_WEAK_ZBOUND, which drops the rejection-loop's ||r0||_inf check,
# fails 4 of the 6 ACVP sigGen vectors while every OTHER check -- keyGen,
# sigVer, and this file's own round trip -- stays green). But the KAT is a
# FIXED, small set of (key, message) pairs; a defect that only manifests for
# some fraction of inputs (exactly the shape of a dropped rejection check --
# see mldsa.c's header) can survive a small KAT by chance the way it cannot
# survive hundreds of trials against an independently-written verifier. This
# is the same argument run-mlkem-openssl.sh's header makes, and the same
# "agrees with a SECOND implementation" standard mini-libc's gate is held to.
#
# Five properties:
#   A KEYGEN   our pk from seed xi must BE openssl's pk from the same seed
#              (genpkey -pkeyopt hexseed:). Exercises ExpandA, ExpandS, the
#              NTT and every pk/sk encode function.
#   B SIGN     our DETERMINISTIC signature (rnd=0^32) over openssl's key must
#              BE openssl's own deterministic signature for the same
#              (key, message, context) -- not just "verifies", IDENTICAL
#              BYTES. This is the one property that exercises the rejection
#              loop end to end against a second implementation; see the
#              header note above on why that matters.
#   C VERIFY   openssl must accept OUR signature, and we must accept
#              OPENSSL's signature, over the SAME key.
#   D REJECT   a one-bit-flipped signature must be refused by BOTH sides.
#   E CROSS    a signature made under one CONTEXT must be refused when
#              verified under a different one, by both sides -- context is
#              part of the signed message representative (FIPS 204 Algorithm
#              2), not a side channel, and a verifier that ignored it would
#              still pass every vector above.
#
# THE OPENSSL PKEYOPT TRAP THIS SCRIPT ALREADY FELL INTO ONCE, keep it in the
# comment so nobody re-discovers it by an hour of confused diffing:
# `-pkeyopt context-string:6162` sends the LITERAL FOUR ASCII BYTES "6162",
# not the two bytes 0x61 0x62 -- `hexcontext-string:` is the hex form. Using
# the wrong one produces two signatures that are each internally consistent
# (this file's own verify accepts its own signature; openssl's accepts its
# own) and disagree with each other for a reason that has nothing to do with
# either implementation being wrong. Exactly rule 5's shape: a control (or
# here, a comparison) that fails for the wrong reason.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL="${OPENSSL:-openssl}"
NTRIAL="${NTRIAL:-4}"
CONTROLS=0
[ "${1:-}" = "--controls" ] && CONTROLS=1

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" list -signature-algorithms 2>/dev/null | grep -qi ML-DSA-44 || {
    echo "SKIP: this openssl has no ML-DSA (needs 3.5+ built with the ML-DSA"
    echo "      provider); the FIPS 204 / ACVP known answers in"
    echo "      tests/unit/mldsa_kat.inc still gate the code via 'make test-mldsa'."
    exit 0; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$BUILD"

MLDSA_SRC="$ROOT/c/crypto/pq/mldsa.c $ROOT/c/crypto/pq/keccak.c"

build() {   # build [-Dcontrol]
    $CC -O2 -Wall -Wextra ${1:+"$1"} -o "$BUILD/mldsa_cli" \
        "$ROOT/tests/unit/mldsa_cli.c" $MLDSA_SRC -I"$ROOT/c/crypto/pq" 2>"$TMP/build.log"
}

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

# openssl's ML-DSA parameter set names, one per set this file gates.
ossl_name() { case "$1" in 44) echo ML-DSA-44;; 65) echo ML-DSA-65;; 87) echo ML-DSA-87;; esac; }

run_suite() {
    pass=0; fail=0
    local CLI="$BUILD/mldsa_cli"
    for SET in 44 65 87; do
        local ONAME; ONAME=$(ossl_name "$SET")
        for i in $(seq 1 "$NTRIAL"); do
            local xi our_pk ossl_pk msg msglen ctx ctx2 our_sig ossl_sig ourv osslv
            xi=$(head -c 32 /dev/urandom | xxd -p | tr -d '\n')

            # A. keygen, byte for byte
            "$OPENSSL" genpkey -algorithm "$ONAME" -pkeyopt hexseed:"$xi" -out "$TMP/k.pem" 2>/dev/null
            "$OPENSSL" pkey -in "$TMP/k.pem" -pubout -outform DER -out "$TMP/pub.der" 2>/dev/null
            local pklen; case "$SET" in 44) pklen=1312;; 65) pklen=1952;; 87) pklen=2592;; esac
            ossl_pk=$(tail -c "$pklen" "$TMP/pub.der" | xxd -p | tr -d '\n')
            our_pk=$("$CLI" keygen "$SET" "$xi" pk)
            if [ "$our_pk" = "$ossl_pk" ]; then ok; else bad "A keygen $ONAME (trial $i)"; fi

            # message + two distinct contexts, sized so the random byte count
            # varies across trials without ever exceeding pkeyutl's argv limits
            msglen=$(( (i * 37) % 200 + 1 ))
            msg=$(head -c "$msglen" /dev/urandom | xxd -p | tr -d '\n')
            ctx=$(head -c 16 /dev/urandom | xxd -p | tr -d '\n')
            ctx2=$(head -c 16 /dev/urandom | xxd -p | tr -d '\n')
            printf '%s' "$msg" | xxd -r -p > "$TMP/msg.bin"

            # B. our deterministic signature must be BYTE-IDENTICAL to openssl's
            "$OPENSSL" pkeyutl -sign -inkey "$TMP/k.pem" -rawin -in "$TMP/msg.bin" \
                -pkeyopt deterministic:1 -pkeyopt hexcontext-string:"$ctx" \
                -out "$TMP/ossl.sig" 2>"$TMP/sign.log"
            if [ ! -s "$TMP/ossl.sig" ]; then
                bad "B openssl produced no signature $ONAME (trial $i)"; cat "$TMP/sign.log"
            else
                ossl_sig=$(xxd -p "$TMP/ossl.sig" | tr -d '\n')
                our_sig=$("$CLI" sign "$SET" "$("$CLI" keygen "$SET" "$xi" sk)" "$msg" "$ctx")
                if [ "$our_sig" = "$ossl_sig" ]; then ok; else bad "B sign $ONAME (trial $i)"; fi
            fi

            # C. cross-verify: openssl accepts ours, we accept openssl's
            printf '%s' "$our_sig" | xxd -r -p > "$TMP/our.sig"
            "$OPENSSL" pkeyutl -verify -pubin -inkey <("$OPENSSL" pkey -in "$TMP/k.pem" -pubout) \
                -rawin -in "$TMP/msg.bin" -sigfile "$TMP/our.sig" -pkeyopt hexcontext-string:"$ctx" \
                >"$TMP/overify.log" 2>&1
            if grep -q "Signature Verified Successfully" "$TMP/overify.log"; then ok
            else bad "C openssl refused OUR signature $ONAME (trial $i)"; fi

            osslv=$("$CLI" verify "$SET" "$our_pk" "$msg" "$ctx" "$ossl_sig")
            if [ "$osslv" = "VALID" ]; then ok; else bad "C we refused OPENSSL's signature $ONAME (trial $i)"; fi

            # D. one flipped bit is refused by both
            local bad_sig; bad_sig=$(python3 -c "
import sys
b=bytearray.fromhex(sys.argv[1]); b[len(b)//2]^=0x40; print(b.hex())" "$ossl_sig")
            printf '%s' "$bad_sig" | xxd -r -p > "$TMP/bad.sig"
            "$OPENSSL" pkeyutl -verify -pubin -inkey <("$OPENSSL" pkey -in "$TMP/k.pem" -pubout) \
                -rawin -in "$TMP/msg.bin" -sigfile "$TMP/bad.sig" -pkeyopt hexcontext-string:"$ctx" \
                >"$TMP/overify2.log" 2>&1
            if grep -q "Signature Verified Successfully" "$TMP/overify2.log"; then
                bad "D openssl ACCEPTED a corrupted signature $ONAME (trial $i)"; else ok; fi
            ourv=$("$CLI" verify "$SET" "$our_pk" "$msg" "$ctx" "$bad_sig")
            if [ "$ourv" = "INVALID" ]; then ok; else bad "D we accepted a corrupted signature $ONAME (trial $i)"; fi

            # E. wrong context is refused by both (the "hexcontext-string:"
            # trap above is exactly why this checks openssl's OWN verify
            # against openssl's OWN signature under a DIFFERENT context,
            # rather than trusting a cross-implementation mismatch to mean
            # what it looks like it means)
            "$OPENSSL" pkeyutl -verify -pubin -inkey <("$OPENSSL" pkey -in "$TMP/k.pem" -pubout) \
                -rawin -in "$TMP/msg.bin" -sigfile "$TMP/ossl.sig" -pkeyopt hexcontext-string:"$ctx2" \
                >"$TMP/overify3.log" 2>&1
            if grep -q "Signature Verified Successfully" "$TMP/overify3.log"; then
                bad "E openssl accepted its own signature under the WRONG context $ONAME (trial $i)"; else ok; fi
            ourv=$("$CLI" verify "$SET" "$our_pk" "$msg" "$ctx2" "$our_sig")
            if [ "$ourv" = "INVALID" ]; then ok; else bad "E we accepted our own signature under the WRONG context $ONAME (trial $i)"; fi
        done
    done
}

if [ "$CONTROLS" = 0 ]; then
    build || { echo "FAIL: could not build mldsa_cli"; cat "$TMP/build.log"; exit 1; }
    run_suite
    echo "ML-DSA vs $($OPENSSL version | cut -d' ' -f1-2): $pass passed, $fail failed" \
         "(3 sets x $NTRIAL trials x 7 checks)"
    [ "$fail" -eq 0 ]
    exit $?
fi

# ------------------------------------------------------------- the controls --
echo "== ML-DSA negative controls: each MUST fail =="
build || { echo "FAIL: baseline build"; cat "$TMP/build.log"; exit 1; }
NTRIAL=2 run_suite
base_fail=$fail
echo "  baseline (no control): $pass passed, $fail failed"
[ "$base_fail" -eq 0 ] || { echo "FAIL: the baseline is not green; controls mean nothing"; exit 1; }

ctl_bad=0
for CTL in MLDSA_CTL_NO_TRANSPOSE MLDSA_CTL_NO_KL_DOMAIN MLDSA_CTL_WEAK_ZBOUND; do
    if ! build "-D$CTL"; then
        echo "  $CTL: BUILD FAILED (a control must compile)"; cat "$TMP/build.log"
        ctl_bad=$((ctl_bad+1)); continue
    fi
    NTRIAL=2 run_suite
    if [ "$fail" -gt 0 ]; then
        echo "  $CTL: correctly RED ($fail of $((pass+fail)) checks failed)"
    else
        echo "  $CTL: PASSED -- the differential cannot see this defect"
        ctl_bad=$((ctl_bad+1))
    fi
done
build   # leave the honest binary behind
if [ "$ctl_bad" -eq 0 ]; then
    echo "all 3 controls fired"; exit 0
else
    echo "$ctl_bad control(s) did not fire"; exit 1
fi
