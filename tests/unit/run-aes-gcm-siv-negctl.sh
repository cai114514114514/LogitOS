#!/usr/bin/env bash
# Negative control for AES-GCM-SIV: -DSIV_CTL_NO_TAG_MASK skips the
# "clear the MSB of the tag's last byte" step RFC 8452 section 4 requires
# before that value is used both as the tag and (with the bit set instead of
# cleared) as the initial CTR counter block. See aes_gcm_siv.c's comment
# beside the #ifndef for the argued failure mode: skipping the clear makes
# roughly half of all messages encrypt "correctly" by chance (whichever ones
# had that bit already 0) and the rest silently wrong -- which is exactly why
# a round-trip self-test cannot see it (both seal and open skip the SAME
# clear, so encrypt-then-decrypt-with-our-own-code stays internally
# consistent either way) and only a comparison against an independently
# produced answer -- RFC 8452's own vectors -- can. This is the same
# "control that cannot be watched failing is worse than no control" argument
# CLAUDE.md names (rule 5); this script exists so that argument has
# something to point at for this file, and it is watched failing every time
# it runs, not just once by hand.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
mkdir -p "$BUILD"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

SRC=(
    "$ROOT/tests/unit/aes_gcm_siv_test.c"
    "$ROOT/c/crypto/aead/aes_gcm_siv.c"
    "$ROOT/c/crypto/aead/aes_dispatch.c"
    "$ROOT/c/crypto/aead/aes_ni.c"
    "$ROOT/c/crypto/aead/aesgcm.c"
    "$ROOT/c/kernel/cpu/cpufeat.c"
)
INC=(-I"$ROOT/c/crypto" -I"$ROOT/c/crypto/aead" -I"$ROOT/c/kernel/cpu" -I"$ROOT/tests/unit")

echo "== baseline (must be green) =="
$CC -O2 -Wall -Wextra "${INC[@]}" -o "$BUILD/aes_gcm_siv_negctl_base" "${SRC[@]}" 2>"$TMP/base.log" \
    || { echo "FAIL: baseline build failed"; cat "$TMP/base.log"; exit 1; }
if ! "$BUILD/aes_gcm_siv_negctl_base" >"$TMP/base.out" 2>&1; then
    echo "FAIL: the baseline is not green; a control means nothing here"
    cat "$TMP/base.out"; exit 1
fi
tail -1 "$TMP/base.out"

echo "== -DSIV_CTL_NO_TAG_MASK (must be RED) =="
$CC -O2 -Wall -Wextra -DSIV_CTL_NO_TAG_MASK "${INC[@]}" -o "$BUILD/aes_gcm_siv_negctl_ctl" "${SRC[@]}" 2>"$TMP/ctl.log" \
    || { echo "FAIL: control build failed (a control must compile)"; cat "$TMP/ctl.log"; exit 1; }
if "$BUILD/aes_gcm_siv_negctl_ctl" >"$TMP/ctl.out" 2>&1; then
    echo "  SIV_CTL_NO_TAG_MASK: PASSED -- the suite cannot see this defect"
    tail -1 "$TMP/ctl.out"
    exit 1
else
    echo "  SIV_CTL_NO_TAG_MASK: correctly RED"
    tail -1 "$TMP/ctl.out"
fi

echo "control fired"
exit 0
