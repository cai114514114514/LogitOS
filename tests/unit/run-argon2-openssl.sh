#!/usr/bin/env bash
# Argon2 (RFC 9106) differential against OpenSSL 3.6.3's EVP_KDF
# ARGON2D/ARGON2I/ARGON2ID, across RANDOMISED (type, t_cost, m_cost, lanes).
#
# WHY RANDOMISED PARAMETERS AND NOT JUST THE RFC VECTOR. The RFC 9106 5.1/5.2/
# 5.3 vectors (checked by test-argon2, byte for byte, tag AND per-pass block
# state against the PHC reference implementation's own KAT) are always
# p=4 lanes. argon2_index_alpha's same-lane/other-lane branches, and the
# ref_lane computation itself, only diverge from a p=1 run when p > 1 -- and
# p=1 is exactly the case a from-scratch implementation is most likely to
# have gotten right by accident (there is only one lane, so "which lane"
# bugs cannot manifest). This script runs p in {1,2,3,5}, several (t,m)
# pairs, and all three types, so a lane-indexing bug that happens not to
# matter at p=4 has somewhere else to show up.
#
# --controls: builds argon2_cli with -DARGON2_NEGCTL_ALWAYS_DATA_DEP (see
# argon2.c's top comment and argon2_test.c) and INVERTS the expected verdict
# for Argon2i/Argon2id only -- Argon2d must still agree with openssl (the
# flag does not touch it), Argon2i/Argon2id must NOT. Exits 0 only if that
# exact pattern holds across every trial, so a control that happened to still
# agree with openssl for the wrong reason cannot pass silently.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL="${OPENSSL:-/opt/homebrew/opt/openssl@3/bin/openssl}"
command -v "$OPENSSL" >/dev/null || OPENSSL=openssl
CONTROLS=0
[ "${1:-}" = "--controls" ] && CONTROLS=1

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" list -kdf-algorithms 2>/dev/null | grep -qi ARGON2ID || {
    echo "SKIP: this openssl has no Argon2 KDFs; the RFC 9106 known answers"
    echo "      in tests/unit/argon2_test.c still gate the code via"
    echo "      'make test-argon2'."; exit 0; }

mkdir -p "$BUILD"
CLI="$BUILD/argon2_cli"
FLAG=""
[ "$CONTROLS" = 1 ] && FLAG="-DARGON2_NEGCTL_ALWAYS_DATA_DEP"
$CC -O2 -Wall -Wextra $FLAG -o "$CLI" \
    "$ROOT/tests/unit/argon2_cli.c" "$ROOT/c/crypto/kdf/argon2.c" \
    -I"$ROOT/c/crypto" -I"$ROOT/c/crypto/kdf" || { echo "FAIL: build"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

# (t_cost, m_cost_kib, lanes) -- m_cost chosen small so the whole suite runs
# in well under a second per case; the memory-hardness ARGUMENT does not
# depend on m being large, only the correctness of the addressing arithmetic
# does, and that is what this differential is checking.
#
# EVERY case below holds segment_length (= m_cost/(4*lanes)) AT OR ABOVE 8,
# and that floor is not arbitrary: it is what --controls needs to be a
# control anyone can watch fail rather than one that sometimes cannot. The
# very first block this file computes in any run has reference_area_size ==
# 1 (RFC 9106 3.4's "all but the previous" case, at the smallest possible
# index) -- there is exactly one candidate, so THAT ONE BLOCK is identical
# whether the pseudo-random index came from the data-independent stream or
# the data-dependent one, no matter which addressing mode is in force. A
# too-small segment_length (this script's first draft used m=8 lanes=1, t=1,
# segment_length=2) leaves so few subsequent blocks, with such small
# reference windows at every one of them, that this coincidence was measured
# to survive all the way to the FINAL TAG on 45 of 60 trials (75%) for
# Argon2id -- the injected defect produced a byte-identical answer to the
# correct one BY CHANCE. That is not a rare corner; it would have made
# --controls flaky-green far more often than red. Watching it happen is the
# reason this comment exists; segment_length=8 was measured to make it not
# reachable at all across 420 trials (7 cases x 3 types x 20 repeats).
CASES=(
    "1 32 1"
    "2 32 1"
    "1 64 2"
    "3 64 2"
    "2 96 3"
    "1 160 5"
    "4 128 4"
)
TYPES="d i id"
REPEATS="${ARGON2_REPEATS:-4}"

trial=0
for c in "${CASES[@]}"; do
    read -r T M P <<< "$c"
    for TYPE in $TYPES; do
      for _rep in $(seq 1 "$REPEATS"); do
        trial=$((trial+1))
        PW=$(printf '%02x' $((trial % 251)))$(head -c 15 /dev/urandom | xxd -p | tr -d '\n')
        SALT=$(head -c 16 /dev/urandom | xxd -p | tr -d '\n')
        SEC=""
        AD=""
        if [ $((trial % 2)) -eq 0 ]; then SEC=$(head -c 8 /dev/urandom | xxd -p | tr -d '\n'); fi
        if [ $((trial % 3)) -eq 0 ]; then AD=$(head -c 12 /dev/urandom | xxd -p | tr -d '\n'); fi

        OSSL_TYPE=$(echo "ARGON2$TYPE" | tr '[:lower:]' '[:upper:]')
        ossl_out=$("$OPENSSL" kdf -keylen 32 -kdfopt hexpass:"$PW" -kdfopt hexsalt:"$SALT" \
            -kdfopt hexsecret:"$SEC" -kdfopt hexad:"$AD" \
            -kdfopt iter:"$T" -kdfopt threads:1 -kdfopt lanes:"$P" -kdfopt memcost:"$M" \
            "$OSSL_TYPE" 2>/dev/null | tr -d ':\n' | tr 'A-F' 'a-f')

        our_out=$("$CLI" "$TYPE" "$T" "$M" "$P" 32 "$PW" "$SALT" "$SEC" "$AD" 2>/dev/null)

        label="type=$TYPE t=$T m=$M p=$P trial=$trial"
        if [ -z "$ossl_out" ]; then bad "$label: openssl produced nothing"; continue; fi

        agree=0
        [ "$our_out" = "$ossl_out" ] && agree=1

        if [ "$CONTROLS" = 1 ] && [ "$TYPE" != "d" ]; then
            # Argon2i/Argon2id MUST disagree under the injected defect.
            if [ "$agree" = 0 ]; then ok; else bad "$label: control should have diverged from openssl and did not"; fi
        else
            # Every other case (including Argon2d under --controls) must agree.
            if [ "$agree" = 1 ]; then ok; else bad "$label: our=$our_out ossl=$ossl_out"; fi
        fi
      done
    done
done

suffix=""; [ "$CONTROLS" = 1 ] && suffix="-controls"
echo "argon2-openssl${suffix}: $pass ok, $fail failed ($trial trials, types: $TYPES)"
[ "$fail" -eq 0 ] && exit 0 || exit 1
