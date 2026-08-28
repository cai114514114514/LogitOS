#!/usr/bin/env bash
# BLAKE2s differential against OpenSSL 3.5+, byte for byte, in the one
# dimension the reference KAT (tests/unit/blake2s_test.c) cannot reach:
# VARIABLE digest length. blake2s-kat.txt's 256 vectors are all outlen==32;
# this drives `openssl dgst -blake2s256` (unkeyed, fixed 32) and
# `openssl mac ... BLAKE2SMAC` (keyed, `size:` = any outlen 1..32) across
# message lengths that cross the 64-byte block boundary the same way the KAT
# does, but at outlen in {1, 7, 16, 20, 32} and key lengths in {0, 1, 16, 32}.
#
#   make test-blake2s-openssl            differential
#   bash run-blake2s-openssl.sh --controls   the negative control (below)
#
# WHY A DIFFERENTIAL AND NOT JUST MORE SELF-CHECKS: the parameter-block defect
# named in blake2s.c (BLAKE2S_PARAM_WORD, -DLOGIT_BLAKE2S_BAD_PARAM) is
# entirely self-consistent for keylen==0 && outlen==32 -- a round-trip test
# against our own blake2s_init/blake2s_final agrees with itself at every
# other (keylen, outlen) too, because both sides of a self-comparison inherit
# the same bug. Only a second, independently written implementation --
# openssl's -- can catch that the wrong constant is wrong. Same argument
# run-mlkem-openssl.sh makes for ML-KEM.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL="${OPENSSL:-openssl}"
CONTROLS=0
[ "${1:-}" = "--controls" ] && CONTROLS=1

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" list -digest-algorithms 2>/dev/null | grep -qi BLAKE2s256 || {
    echo "SKIP: this openssl has no BLAKE2s256 (needs a build with the"
    echo "      legacy/default provider's blake2 -- 3.0+ normally has it);"
    echo "      the KAT vectors in tests/unit/blake2s_test.c still gate the"
    echo "      code via 'make test-blake2s'."; exit 0; }
"$OPENSSL" list -mac-algorithms 2>/dev/null | grep -qi BLAKE2SMAC || {
    echo "SKIP: this openssl has BLAKE2s256 but no BLAKE2SMAC (needed for the"
    echo "      keyed / variable-outlen half of this differential)."; exit 0; }

mkdir -p "$BUILD"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

build() {   # build [-Dcontrol]
    $CC -O2 -Wall -Wextra ${1:+"$1"} -o "$BUILD/blake2s_cli" \
        "$ROOT/tests/unit/blake2s_cli.c" "$ROOT/c/crypto/hash/blake2s.c" \
        -I"$ROOT/c/crypto/hash" 2>"$TMP/build.log"
}

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

# Message lengths chosen to straddle BLAKE2S_BLOCKBYTES (64) the same way the
# KAT's 0..255 range does: 0, 1, mid-block, exactly one block, one-past-a-
# block, two blocks, two-blocks-plus-one.
MSGLENS=(0 1 30 64 65 128 129)

randhex() { [ "$1" = 0 ] && return; head -c "$1" /dev/urandom | xxd -p | tr -d '\n'; }

run_suite() {
    pass=0; fail=0
    local CLI="$BUILD/blake2s_cli"
    for mlen in "${MSGLENS[@]}"; do
        local msg; msg=$(randhex "$mlen")
        [ "$mlen" = 0 ] && msg=""

        # --- unkeyed, fixed 32-byte output, against `openssl dgst` ---
        local ours ossl
        ours=$("$CLI" "$msg" - 32)
        if [ -z "$msg" ]; then
            ossl=$(printf '' | "$OPENSSL" dgst -blake2s256 -r | cut -d' ' -f1)
        else
            ossl=$(printf '%s' "$msg" | xxd -r -p | "$OPENSSL" dgst -blake2s256 -r | cut -d' ' -f1)
        fi
        if [ "$ours" = "$ossl" ]; then ok; else bad "unkeyed outlen=32 msglen=$mlen: ours=$ours ossl=$ossl"; fi

        # --- keyed, variable output length, against `openssl mac BLAKE2SMAC` ---
        for keylen in 1 16 32; do
            local key; key=$(randhex "$keylen")
            for outlen in 1 7 16 20 32; do
                ours=$("$CLI" "$msg" "$key" "$outlen")
                if [ -z "$msg" ]; then
                    ossl=$(printf '' | "$OPENSSL" mac -macopt hexkey:"$key" -macopt size:"$outlen" BLAKE2SMAC 2>/dev/null | tr 'A-F' 'a-f')
                else
                    ossl=$(printf '%s' "$msg" | xxd -r -p | "$OPENSSL" mac -macopt hexkey:"$key" -macopt size:"$outlen" BLAKE2SMAC 2>/dev/null | tr 'A-F' 'a-f')
                fi
                if [ "$ours" = "$ossl" ]; then ok; else bad "keyed keylen=$keylen outlen=$outlen msglen=$mlen: ours=$ours ossl=$ossl"; fi
            done
        done
    done
}

if [ "$CONTROLS" = 0 ]; then
    build || { echo "FAIL: could not build blake2s_cli"; cat "$TMP/build.log"; exit 1; }
    run_suite
    echo "BLAKE2s vs $($OPENSSL version | cut -d' ' -f1-2): $pass passed, $fail failed" \
         "(${#MSGLENS[@]} msglens x (1 unkeyed + 3 keylens x 5 outlens))"
    [ "$fail" = 0 ] || exit 1
    exit 0
fi

# ------------------------------------------------------- negative control --
# LOGIT_BLAKE2S_BAD_PARAM hardcodes the parameter word for (keylen=0,
# outlen=32) -- so it MUST still agree with openssl on the unkeyed/32 cases
# and MUST disagree on every keyed or non-32 case. Watching only "some
# failures" would not distinguish this from a build that is broken outright;
# the assertion is the SHAPE of the failure, not just its presence.
build -DLOGIT_BLAKE2S_BAD_PARAM || { echo "FAIL: could not build control"; cat "$TMP/build.log"; exit 1; }
run_suite
want_pass=$(( ${#MSGLENS[@]} * 1 ))   # only the unkeyed/32 case per msglen
want_fail=$(( ${#MSGLENS[@]} * 3 * 5 ))
echo "control (LOGIT_BLAKE2S_BAD_PARAM): $pass passed, $fail failed" \
     "(want exactly $want_pass passed [unkeyed/32], $want_fail failed [every keyed/non-32 case])"
if [ "$pass" = "$want_pass" ] && [ "$fail" = "$want_fail" ]; then
    echo "negative control ok: the defect is invisible on unkeyed/32 and total on everything else"
    exit 0
else
    echo "FAIL: control did not fail in the SHAPE the parameter-block defect predicts"
    exit 1
fi
