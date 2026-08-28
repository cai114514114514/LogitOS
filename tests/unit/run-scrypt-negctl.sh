#!/usr/bin/env bash
# scrypt (RFC 7914) negative control.
#
# THE DEFECT: SCRYPT_BREAK_INTERLEAVE (c/crypto/kdf/scrypt.c, scrypt_blockmix)
# writes scryptBlockMix's output blocks in the order they were PRODUCED
# instead of the RFC 7914 s4 step-3 INTERLEAVED order
# (Y[0],Y[2],...,Y[2r-2],Y[1],Y[3],...,Y[2r-1]). It is a real, specific,
# argued-for mistake -- not a generic corruption -- and one this task's own
# brief names as the way this algorithm is most often gotten silently wrong.
#
# WHY THIS CONTROL HAS TO BE WATCHED, NOT JUST RUN: at r=1 the interleaved
# and produced orders are THE SAME SEQUENCE (2r=2 blocks: (Y[0],Y[1]) either
# way). RFC 7914's own section 8/9/10 standalone vectors and its first full
# scrypt vector (test 1) all use r=1 -- so a gate built only from those would
# report this control as "did not fire" while looking exactly like a working
# control, which is rule 5 of this tree's CLAUDE.md in one sentence: "a
# control that cannot be watched failing is worse than no control, because it
# reads like one." This script does not accept a bare nonzero exit code as
# proof the defect was caught -- it greps for exactly the shape the RFC's own
# math predicts: r=1 vectors (core, blockmix, romix, full test 1) still OK,
# r=8 vectors (full tests 2 and 3) FAIL. A control satisfied by an unrelated
# crash, or by the wrong vectors reddening, does not count.
#
# WATCHED, 2026: baseline green (12/12, big vector skipped -- default). With
# -DSCRYPT_BREAK_INTERLEAVE: salsa20/8 core, in-place core, blockmix r=1,
# romix r=1,N=16, and full test 1 (N=16,r=1,p=1) all still read "ok"; full
# test 2 (N=1024,r=8,p=16) and test 3 (N=16384,r=8,p=1) both read "FAIL";
# overall exit code 1, "9 passed, 2 failed". Exactly the predicted shape.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
mkdir -p "$BUILD"
SRC=("$ROOT/tests/unit/scrypt_test.c" "$ROOT/c/crypto/kdf/scrypt.c" "$ROOT/c/crypto/hash/sha256.c")
INC=(-I"$ROOT/c/crypto" -I"$ROOT/c/crypto/kdf" -I"$ROOT/tests/unit")

build() { $CC -O2 -Wall -Wextra "${1:+-D$1}" -o "$BUILD/scrypt_test_negctl" "${SRC[@]}" "${INC[@]}"; }

build "" || { echo "FAIL: baseline scrypt_test does not even build"; exit 1; }
if ! "$BUILD/scrypt_test_negctl" >"$BUILD/scrypt_negctl_base.log" 2>&1; then
    echo "FAIL: the baseline is not green; a control on top of a broken baseline means nothing"
    cat "$BUILD/scrypt_negctl_base.log"; exit 1
fi

build "SCRYPT_BREAK_INTERLEAVE" || { echo "FAIL: could not build the SCRYPT_BREAK_INTERLEAVE control"; exit 1; }
"$BUILD/scrypt_test_negctl" >"$BUILD/scrypt_negctl_break.log" 2>&1
rc=$?

bad=0
must_pass=("ok   salsa20/8 core (RFC 7914 s8)" "ok   salsa20/8 core, in-place"
           "ok   scryptBlockMix r=1 (RFC 7914 s9)" "ok   scryptROMix r=1,N=16 (RFC 7914 s10)"
           "ok   scrypt test 1 (N=16,r=1,p=1)")
must_fail=("FAIL scrypt test 2 (N=1024,r=8,p=16)" "FAIL scrypt test 3 (N=16384,r=8,p=1)")

for line in "${must_pass[@]}"; do
    grep -qF "$line" "$BUILD/scrypt_negctl_break.log" || {
        echo "BAD CONTROL: expected still-passing '$line' (r=1 cannot see this defect) but it did not appear"
        bad=1; }
done
for line in "${must_fail[@]}"; do
    grep -qF "$line" "$BUILD/scrypt_negctl_break.log" || {
        echo "BAD CONTROL: expected '$line' but the interleave defect did not redden it -- control did not fire"
        bad=1; }
done
if [ "$rc" -eq 0 ]; then
    echo "BAD CONTROL: scrypt_test_negctl exited 0 with the defect compiled in"
    bad=1
fi

if [ "$bad" -eq 0 ]; then
    echo "negative control fired exactly as predicted (r=1 vectors blind to it, r=8 vectors catch it)"
    exit 0
else
    cat "$BUILD/scrypt_negctl_break.log"
    exit 1
fi
