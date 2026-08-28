#!/usr/bin/env bash
# BLAKE2b differential against OpenSSL 3.6.3, byte for byte.
#
# WHAT THIS COVERS THAT tests/unit/blake2b_test.c CANNOT: the official KAT
# (blake2b_kat.inc) only ever exercises a 64-byte KEYED output with a fixed
# 64-byte key. This script is the only place in the tree that checks
#   (a) UNKEYED, fixed 64-byte output, over many random messages
#       -- `openssl dgst -blake2b512`
#   (b) KEYED, variable output length 1..64 AND variable key length 1..64
#       -- `openssl mac -macopt size:N -macopt hexkey:K BLAKE2BMAC`
# `openssl mac ... BLAKE2BMAC` was confirmed (2026-08-28) to reproduce the
# FIRST row of the official KAT exactly (empty message, full 64-byte key,
# 64-byte output => 10ebb677...fc51568) before this script was trusted to
# check anything else -- so (b) is validated against (1) before it is used to
# validate what (1) cannot reach.
#
# NOT COVERED, and said plainly rather than silently skipped: unkeyed output
# at a length other than 64. OpenSSL's BLAKE2BMAC refuses a zero-length key
# ("invalid key length"), and `dgst -blake2b512` has no length option, so
# there is no OpenSSL entry point for that combination at all. See
# blake2b_test.c's header for the property check that covers it instead.
#
# --controls: build blake2b_cli with -DBLAKE2B_BUG_ROT63 (the same negative
# control blake2b_test.c uses -- see blake2b.c's header) and assert it
# DISAGREES with OpenSSL on essentially everything. A control that has not
# been built and watched fail is not a control (CLAUDE.md rule 1 / rule 5 of
# this task).
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL="${OPENSSL:-/opt/homebrew/opt/openssl@3/bin/openssl}"
command -v "$OPENSSL" >/dev/null 2>&1 || OPENSSL=openssl
NTRIAL="${NTRIAL:-40}"
CONTROLS=0
[ "${1:-}" = "--controls" ] && CONTROLS=1

# A missing reference is not a regression in the code under test.
command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" dgst -blake2b512 </dev/null >/dev/null 2>&1 || {
    echo "SKIP: this openssl build has no BLAKE2b512 digest; the RFC 7693 /"
    echo "      official-KAT checks in tests/unit/blake2b_test.c still gate"
    echo "      the code via 'make test-blake2b'."; exit 0; }
"$OPENSSL" mac -macopt size:32 -macopt hexkey:00 BLAKE2BMAC </dev/null >/dev/null 2>&1 || {
    echo "SKIP: this openssl build's BLAKE2BMAC does not accept -macopt size/hexkey;"
    echo "      the keyed/variable-outlen half of this differential needs it."; exit 0; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$BUILD"

build() {   # build [-Dflag]
    $CC -O2 -Wall -Wextra ${1:+"$1"} -o "$BUILD/blake2b_cli" \
        "$ROOT/tests/unit/blake2b_cli.c" "$ROOT/c/crypto/hash/blake2b.c" \
        -I"$ROOT/c/crypto/hash" 2>"$TMP/build.log" && return 0
    echo "BUILD FAILED:"; cat "$TMP/build.log"; return 1
}

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

hexkey() { head -c "$1" /dev/urandom | xxd -p | tr -d '\n'; }

run_suite() {
    pass=0; fail=0
    local CLI="$BUILD/blake2b_cli"

    # (a) unkeyed, fixed 64-byte output, random messages of varying length
    for i in $(seq 1 "$NTRIAL"); do
        # BSD head refuses `-c 0`, so keep this in [1,256] -- 0-length input
        # is exercised separately by tests/unit/blake2b_test.c's RFC/KAT
        # cases (several official vectors are the empty message).
        local len=$(( (i * 3 % 256) + 1 ))
        head -c "$len" /dev/urandom > "$TMP/msg"
        local ours ossl
        ours=$("$CLI" 64 < "$TMP/msg")
        ossl=$("$OPENSSL" dgst -blake2b512 -r < "$TMP/msg" | awk '{print $1}' | tr 'A-F' 'a-f')
        if [ "$ours" = "$ossl" ]; then ok; else bad "unkeyed len=$len: ours=$ours ossl=$ossl"; fi
    done

    # (b) keyed, variable output length 1..64, variable key length 1..64
    for outlen in 1 7 16 31 32 48 63 64; do
        for keylen in 1 8 32 63 64; do
            local k msg ours ossl
            k=$(hexkey "$keylen")
            head -c $(( ((outlen * 7 + keylen) % 199) + 1 )) /dev/urandom > "$TMP/msg"
            ours=$("$CLI" "$outlen" "$k" < "$TMP/msg")
            ossl=$("$OPENSSL" mac -macopt "size:$outlen" -macopt "hexkey:$k" BLAKE2BMAC < "$TMP/msg" 2>"$TMP/mac.err" | tr 'A-F' 'a-f')
            if [ "$ours" = "$ossl" ]; then ok; else bad "keyed outlen=$outlen keylen=$keylen: ours=$ours ossl=$ossl $(cat "$TMP/mac.err")"; fi
        done
    done
}

if [ "$CONTROLS" = 1 ]; then
    echo "== negative control: -DBLAKE2B_BUG_ROT63 must disagree with OpenSSL =="
    build -DBLAKE2B_BUG_ROT63 || exit 1
    run_suite
    total=$((pass + fail))
    echo "control: $pass/$total agreed with OpenSSL (want: agreement rare/zero)"
    # The control is watched FAILING, i.e. the broken build must NOT pass as
    # cleanly as the real one -- assert it disagrees on the clear majority.
    if [ "$fail" -lt $(( total * 9 / 10 )) ]; then
        echo "CONTROL DID NOT REDDEN: broken build agreed with OpenSSL $pass/$total times"
        echo "(expected almost all to disagree -- the control is not exercising the bug)"
        exit 1
    fi
    echo "control confirmed red: $fail/$total disagreements, as expected"
    exit 0
fi

build || exit 1
run_suite
echo "blake2b-openssl: $pass/$((pass + fail)) agreed with OpenSSL $($OPENSSL version)"
[ "$fail" -eq 0 ] || exit 1
