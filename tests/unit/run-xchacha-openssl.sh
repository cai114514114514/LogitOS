#!/usr/bin/env bash
# XChaCha20-Poly1305 differential against openssl -- PARTIAL, and this file
# says exactly how.
#
# draft-irtf-cfrg-xchacha-03 is a CFRG draft that never became an RFC, so
# there is no RFC-numbered known-answer appendix, and OpenSSL does not
# implement the X variant at all (`openssl list -cipher-algorithms` on this
# host lists "ChaCha20-Poly1305" and "ChaCha20" but nothing named XChaCha).
# The task this file was written under suggested feeding our derived subkey
# and constructed 12-byte nonce to `openssl enc -chacha20-poly1305` to get a
# real AEAD differential for the RFC 8439 core half. THAT DOES NOT WORK ON
# THIS HOST: openssl 3.6.3's `enc` subcommand refuses EVERY AEAD cipher
# outright --
#
#   $ openssl enc -chacha20-poly1305 -K ... -iv ...
#   enc: AEAD ciphers not supported
#
# -- verified the same way for -aes-256-gcm, so this is not specific to
# ChaCha; `enc` simply has no -aad/-tag flags to drive an AEAD cipher with
# (see its own -help output). That is a fact about the CLI, checked, not
# assumed.
#
# What DOES work is `openssl enc -chacha20` -- the plain, non-authenticated
# stream cipher, which takes a 16-byte IV = 4-byte little-endian block
# counter || 12-byte RFC 8439 nonce (verified against the RFC 8439 section
# 2.4.2 "sunscreen" test vector below before trusting it for anything else).
# Since RFC 8439's AEAD ciphertext is exactly plaintext XOR keystream --
# independent of AAD, which only affects the tag -- this still gives a real,
# externally-anchored check of everything this file is actually responsible
# for: HChaCha20's subkey derivation and the nonce split, end to end, over
# many random trials, not just the one official vector. What it does NOT
# check is the Poly1305 tag -- but the RFC 8439 core (poly1305 + the AEAD
# framing) already has its own coverage elsewhere in this tree
# (tests/unit/crypto_vec_test.c's test_chacha, RFC 8439 vectors), and
# tests/unit/xchacha_kat.inc pins every tag against Wycheproof directly. This
# script's job is the part nothing else in the tree touches: is the subkey
# and the nonce split right, checked against an implementation that is not
# ours.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL="${OPENSSL:-openssl}"
NTRIAL="${NTRIAL:-16}"
CONTROLS=0
[ "${1:-}" = "--controls" ] && CONTROLS=1

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$BUILD"

# Sanity: the plain stream cipher must exist and take a 16-byte IV the way we
# assume, checked against RFC 8439 2.4.2 before anything else runs.
SUNKEY=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
SUNIV=01000000000000000000004a00000000     # counter=1 LE || nonce
SUNPT="4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a204966204920636f756c64206f6666657220796f75206f6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73637265656e20776f756c642062652069742e"
sunout=$(printf '%s' "$SUNPT" | xxd -r -p | "$OPENSSL" enc -e -chacha20 -K "$SUNKEY" -iv "$SUNIV" -nopad 2>/dev/null | xxd -p | tr -d '\n')
case "$sunout" in
    6e2e359a2568f98041ba0728dd0d6981*) ;;
    *) echo "SKIP: this openssl's 'enc -chacha20' does not behave the way this script assumes (got $sunout)"; exit 0 ;;
esac

build() {   # build [-Dcontrol]
    $CC -O2 -Wall -Wextra ${1:+"$1"} -o "$BUILD/xchacha_cli" \
        "$ROOT/tests/unit/xchacha_cli.c" \
        "$ROOT/c/crypto/aead/xchacha20poly1305.c" \
        "$ROOT/c/crypto/aead/chacha20poly1305.c" \
        -I"$ROOT/c/crypto" -I"$ROOT/c/crypto/aead" 2>"$TMP/build.log"
}

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

le32() { printf '%02x%02x%02x%02x' $(( $1 & 255 )) $(( ($1>>8) & 255 )) $(( ($1>>16) & 255 )) $(( ($1>>24) & 255 )); }

run_suite() {
    pass=0; fail=0
    local CLI="$BUILD/xchacha_cli"
    for i in $(seq 1 "$NTRIAL"); do
        local key nonce24 nonce16 nonce8 inner subkey pl pt aad
        key=$(head -c 32 /dev/urandom | xxd -p | tr -d '\n')
        nonce24=$(head -c 24 /dev/urandom | xxd -p | tr -d '\n')
        nonce16=${nonce24:0:32}
        nonce8=${nonce24:32:16}
        inner="00000000${nonce8}"                       # the correct split

        subkey=$("$CLI" hchacha "$key" "$nonce16")

        # keystream block 0 and block 1, ours vs openssl (16-byte IV = LE
        # counter || 12-byte inner nonce).
        for ctr in 0 1; do
            local iv ours theirs
            iv="$(le32 "$ctr")${inner}"
            ours=$("$CLI" stream "$subkey" "$inner" "$ctr" 64)
            theirs=$(printf '%0.s00' $(seq 1 64) | xxd -r -p | \
                     "$OPENSSL" enc -e -chacha20 -K "$subkey" -iv "$iv" -nopad 2>/dev/null | xxd -p | tr -d '\n')
            if [ "$ours" = "$theirs" ]; then ok; else bad "stream ctr=$ctr disagree (trial $i)"; fi
        done

        # a full seal, ciphertext bytes only (tag is not openssl-checkable
        # here -- see header; xchacha_kat.inc covers the tag).
        pl=$(( (RANDOM % 96) ))
        pt=$(head -c "$pl" /dev/urandom | xxd -p | tr -d '\n')
        aad=$(head -c 8 /dev/urandom | xxd -p | tr -d '\n')
        local sealout our_ct
        sealout=$("$CLI" seal "$key" "$nonce24" "$aad" "$pt")
        our_ct=$(printf '%s\n' "$sealout" | sed -n 1p)
        local theirs_ct
        if [ "$pl" -eq 0 ]; then theirs_ct=""; else
            theirs_ct=$(printf '%s' "$pt" | xxd -r -p | \
                "$OPENSSL" enc -e -chacha20 -K "$subkey" -iv "$(le32 1)${inner}" -nopad 2>/dev/null | xxd -p | tr -d '\n')
        fi
        if [ "$our_ct" = "$theirs_ct" ]; then ok; else bad "seal ciphertext disagree (trial $i, len=$pl)"; fi
    done
}

if [ "$CONTROLS" = 0 ]; then
    build || { echo "FAIL: could not build xchacha_cli"; cat "$TMP/build.log"; exit 1; }
    run_suite
    echo "XChaCha20-Poly1305 vs $($OPENSSL version | cut -d' ' -f1-2) (stream-only, see header): $pass passed, $fail failed" \
         "($NTRIAL trials x 3 checks)"
    [ "$fail" -eq 0 ]
    exit $?
fi

# ------------------------------------------------------------- the controls --
echo "== XChaCha20-Poly1305 negative controls: each MUST fail =="
build || { echo "FAIL: baseline build"; cat "$TMP/build.log"; exit 1; }
NTRIAL=4 run_suite
base_fail=$fail
echo "  baseline (no control): $pass passed, $fail failed"
[ "$base_fail" -eq 0 ] || { echo "FAIL: the baseline is not green; controls mean nothing"; exit 1; }

# XCHACHA_BUG_ADD_BACK is DELIBERATELY NOT in this list, and watching it fail
# to fire (below, before this fix) is exactly why: this script derives
# openssl's reference IV/key material from the SAME "hchacha" CLI call the
# thing under test uses, so a subkey-derivation bug makes both sides wrong
# in the identical way and they agree with each other throughout -- the same
# shape as run-mlkem-openssl.sh's "three of the five defects produce an
# implementation that is entirely self-consistent" note. That control is
# real, watched failing, and lives in test-xchacha (xchacha20poly1305_test.c)
# instead, against the standalone HChaCha20 vector this script never touches.
ctl_bad=0
for CTL in XCHACHA_BUG_NONCE_APPEND; do
    if ! build "-D$CTL"; then
        echo "  $CTL: BUILD FAILED (a control must compile)"; cat "$TMP/build.log"
        ctl_bad=$((ctl_bad+1)); continue
    fi
    NTRIAL=4 run_suite
    if [ "$fail" -gt 0 ]; then
        echo "  $CTL: correctly RED ($fail of $((pass+fail)) checks failed)"
    else
        echo "  $CTL: PASSED -- the differential cannot see this defect"
        ctl_bad=$((ctl_bad+1))
    fi
done
build   # leave the honest binary behind
if [ "$ctl_bad" -eq 0 ]; then
    # ONE control runs here, not two. The word "both" stood on this line while
    # the loop above carried a single -D, and the other control's verdict line
    # was simply absent from the output -- a summary claiming a control fired
    # in a harness that never ran it is the exact shape CLAUDE.md rule 5 names.
    # XCHACHA_BUG_ADD_BACK is invisible to THIS differential by construction
    # (see the comment at the top of the loop) and is watched failing in
    # test-xchacha instead: 65 passed, 739 failed.
    echo "the differential's control fired (XCHACHA_BUG_NONCE_APPEND);"
    echo "XCHACHA_BUG_ADD_BACK is invisible here by construction and is"
    echo "watched failing by test-xchacha -- see this file's own comment."
    exit 0
else
    echo "$ctl_bad control(s) did not fire"; exit 1
fi
