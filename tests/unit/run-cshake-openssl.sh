#!/usr/bin/env bash
# cSHAKE128/256 + KMAC128/256/XOF differential against OpenSSL 3.6.3, over
# random byte-aligned inputs, plus the two claims the NIST PDF samples in
# tests/unit/cshake_test.c cannot cover on their own:
#
#   1. KMACXOF128/256 -- none of NIST's 6 published KMAC samples is a XOF
#      sample (every one carries a nonzero right_encode(L) trailer; see
#      cshake_test.c's header). OpenSSL exposes KMAC-128/-256 as MACs with a
#      settable `xof` param (`openssl list -mac-algorithms`), which is the
#      only official-adjacent oracle for KMACXOF this workflow found. It IS
#      OpenSSL's own KMAC, not a third-party reimplementation -- named here
#      rather than left implicit.
#   2. cSHAKE(X, L, "", "") == SHAKE(X, L) against OpenSSL's SHAKE128/256
#      (`openssl dgst -shake128/-shake256 -xoflen`), independently of this
#      tree's own shake128/256 (c/crypto/pq/keccak.c) -- cshake_test.c checks
#      the same identity against THAT, which shares code with the thing under
#      test; this is the version with no shared code path at all.
#
# WHY A DIFFERENTIAL AND NOT ONLY THE FIXED KAT: the fixed samples in
# cshake_test.c are ~10 points in an enormous input space, all with fairly
# regular lengths (4 or 200 bytes). This script's random lengths (0..300
# bytes, key/custom/message independently) are what would catch an off-by-one
# in the bytepad zero-padding count that happens to land on a rate boundary
# for every NIST sample length but not for an arbitrary one.
#
# ACVP NOTE: NIST's own ACVP KMAC-128/256 JSON vectors were fetched during
# development (usnistgov/ACVP-Server, KMAC-128-1.0 and KMAC-256-1.0,
# verified 200) and inspected -- 798/800 and 799/800 of their key/message/mac
# lengths are literal BIT lengths not a multiple of 8 (e.g. a 3343-bit key
# hex-padded to 3344 bits), because ACVP tests KMAC's bit-string generality.
# Neither this implementation nor OpenSSL's KMAC accepts a non-byte-aligned
# key or message (OpenSSL's EVP_MAC takes byte buffers only), so that corpus
# could not be used here without inventing bit-level absorption nothing
# downstream needs -- said here rather than silently dropping the source.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
# Prefer a homebrew openssl@3 if the caller did not pin one: macOS ships
# LibreSSL as /usr/bin/openssl, which has neither KMAC nor -xoflen, and would
# otherwise produce a SKIP that reads as "no KMAC anywhere" rather than "the
# wrong openssl is first in PATH".
if [ -z "${OPENSSL:-}" ]; then
    if [ -x /opt/homebrew/opt/openssl@3/bin/openssl ]; then OPENSSL=/opt/homebrew/opt/openssl@3/bin/openssl
    elif [ -x /usr/local/opt/openssl@3/bin/openssl ]; then OPENSSL=/usr/local/opt/openssl@3/bin/openssl
    else OPENSSL=openssl
    fi
fi
NTRIAL="${NTRIAL:-40}"
CONTROLS=0
[ "${1:-}" = "--controls" ] && CONTROLS=1

# A missing reference is not a regression in the code under test -- skip
# loudly, per CLAUDE.md rule 5, rather than fail for an unrelated reason.
command -v "$OPENSSL" >/dev/null 2>&1 || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" list -mac-algorithms 2>/dev/null | grep -qi 'KMAC-128' || {
    echo "SKIP: this openssl has no KMAC-128 MAC; the NIST PDF vectors in"
    echo "      tests/unit/cshake_kmac_vectors.inc still gate the code via"
    echo "      'make test-cshake'."; exit 0; }
command -v xxd >/dev/null 2>&1 || { echo "SKIP: no xxd"; exit 0; }

mkdir -p "$BUILD"
CLI="$BUILD/cshake_kmac_cli"
build() {   # build [-Dflag]
    $CC -O2 -Wall -Wextra ${1:+"$1"} -o "$CLI" \
        "$ROOT/tests/unit/cshake_kmac_cli.c" \
        "$ROOT/c/crypto/hash/cshake.c" "$ROOT/c/crypto/hash/kmac.c" \
        "$ROOT/c/crypto/pq/keccak.c" \
        -I"$ROOT/c/crypto/hash" -I"$ROOT/c/crypto/pq"
}
build || { echo "FAIL: build"; exit 1; }

randhex() {   # randhex <max_bytes> [min_bytes] -- min..max_bytes bytes of hex
    local maxb="$1" minb="${2:-0}" n
    n=$((minb + RANDOM % (maxb - minb + 1)))
    [ "$n" -eq 0 ] && return
    head -c "$n" /dev/urandom | xxd -p -c 999 | tr -d '\n'
}

hex_to_bin() { [ -n "$1" ] && xxd -r -p <<<"$1" || printf ''; }   # "" -> empty stdout, no trailing NUL

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

run_suite() {
    pass=0; fail=0
    for i in $(seq 1 "$NTRIAL"); do
        local key msg cust outlen ours theirs keyforopenssl

        # OpenSSL's KMAC provider refuses a key under 4 bytes ("invalid key
        # length", providers/implementations/macs/kmac_prov.c) -- a floor
        # this implementation does not share (SP 800-185 recommends, does
        # not require, a key at least as long as the security strength) and
        # this differential therefore cannot exercise below 4 bytes; that
        # sub-4-byte range is exactly what tests/unit/cshake_test.c's own
        # NIST-sample keys (32/39 bytes) never touch either, so it is
        # uncovered rather than double-covered. min 4, matching OpenSSL.
        key=$(randhex 48 4)
        msg=$(randhex 300)
        cust=$(randhex 40)
        outlen=$(( (RANDOM % 96) + 1 ))
        keyforopenssl="$key"

        for alg in 128 256; do
            ours=$("$CLI" "kmac$alg" "$outlen" "$keyforopenssl" "$msg" "$cust")
            theirs=$(hex_to_bin "$msg" | "$OPENSSL" mac \
                -macopt "hexkey:$keyforopenssl" -macopt "size:$outlen" -macopt xof:0 \
                -macopt "hexcustom:$cust" "KMAC-$alg" 2>/dev/null | tr 'A-F' 'a-f')
            if [ "$ours" = "$theirs" ]; then ok; else bad "kmac$alg xof=0 outlen=$outlen key=$keyforopenssl msg=$msg cust=$cust: ours=$ours theirs=$theirs"; fi
        done

        for alg in 128 256; do
            ours=$("$CLI" "kmacxof$alg" "$outlen" "$keyforopenssl" "$msg" "$cust")
            theirs=$(hex_to_bin "$msg" | "$OPENSSL" mac \
                -macopt "hexkey:$keyforopenssl" -macopt "size:$outlen" -macopt xof:1 \
                -macopt "hexcustom:$cust" "KMAC-$alg" 2>/dev/null | tr 'A-F' 'a-f')
            if [ "$ours" = "$theirs" ]; then ok; else bad "kmacxof$alg outlen=$outlen key=$keyforopenssl msg=$msg cust=$cust: ours=$ours theirs=$theirs"; fi
        done

        for alg in 128 256; do
            ours=$("$CLI" "cshake$alg" "$outlen" "" "$msg" "")
            theirs=$(hex_to_bin "$msg" | "$OPENSSL" dgst "-shake$alg" -xoflen "$outlen" 2>/dev/null | \
                sed -E 's/^.*= //' | tr 'A-F' 'a-f')
            if [ "$ours" = "$theirs" ]; then ok; else bad "cshake$alg==shake$alg outlen=$outlen msg=$msg: ours=$ours theirs=$theirs"; fi
        done
    done
}

if [ "$CONTROLS" -eq 1 ]; then
    echo "== negative control: -DKMAC_NEGCTL_XOF_TRAILER against the SAME openssl oracle =="
    build -DKMAC_NEGCTL_XOF_TRAILER || { echo "FAIL: control build"; exit 1; }
    run_suite
    echo "control: $pass ok, $fail FAIL (of $((NTRIAL*6)))"
    # The bug forces every KMAC call to use KMACXOF's trailer. Against THIS
    # oracle that means: every kmac128/kmac256 case (2 of 6 per trial) must
    # now disagree with openssl; every kmacxof128/256 and cshake case (4 of 6)
    # must still agree -- exactly 2*NTRIAL failures, not everything and not
    # nothing. A control that turned everything red would not distinguish
    # "the trailer selection is wrong" from "the build is broken".
    if [ "$fail" -eq "$((NTRIAL*2))" ]; then
        echo "PASS: control failed in the exact expected shape (kmac128+kmac256 only, $fail failures)"
        build   # rebuild clean so a stray artifact doesn't leak into a later run
        exit 0
    else
        echo "FAIL: control did not fail in the expected shape (got $fail failures, wanted exactly $((NTRIAL*2)))"
        build
        exit 1
    fi
fi

run_suite
echo "$pass ok, $fail FAIL (of $((NTRIAL*6)))"
[ "$fail" -eq 0 ]
