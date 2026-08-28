#!/usr/bin/env bash
# X448 KAT/Wycheproof negative controls -- each MUST redden tests/unit/x448_test.c.
# Companion to run-x448-openssl.sh --controls, which watches the SAME two
# defects against a live openssl instead of the compiled-in KAT/Wycheproof
# corpus. Both exist because a self-consistent bug (neither of these two is,
# but the shape is worth guarding regardless -- see CLAUDE.md's account of
# ML-KEM's three self-consistent negative controls) would pass a KAT-only
# check and a KAT-only check runs without a live openssl, so it is the one
# that should gate CI by default.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
mkdir -p "$BUILD"

SRC=("$ROOT/tests/unit/x448_test.c" "$ROOT/c/crypto/pubkey/x448.c" "$ROOT/c/crypto/pubkey/field448.c")
INC=(-I"$ROOT/c/crypto/pubkey" -I"$ROOT/tests/unit")

build_and_run() {   # build_and_run [-Dcontrol]
    $CC -O2 -Wall -Wextra ${1:+"$1"} -o "$BUILD/x448_test_ctl" "${SRC[@]}" "${INC[@]}" || return 2
    "$BUILD/x448_test_ctl"
}

echo "== X448 KAT/Wycheproof negative controls: each MUST fail =="
if ! build_and_run >/tmp/x448_negctl_base.$$ 2>&1; then
    echo "FAIL: baseline (no control) is not green:"; cat /tmp/x448_negctl_base.$$; rm -f /tmp/x448_negctl_base.$$
    exit 1
fi
echo "  baseline: $(tail -1 /tmp/x448_negctl_base.$$)"
rm -f /tmp/x448_negctl_base.$$

ctl_bad=0
for CTL in LOGIT_X448_CTL_BAD_A24 LOGIT_X448_CTL_BAD_CLAMP; do
    if build_and_run "-D$CTL" >/tmp/x448_negctl_ctl.$$ 2>&1; then
        echo "  $CTL: PASSED -- the suite cannot see this defect"
        cat /tmp/x448_negctl_ctl.$$
        ctl_bad=$((ctl_bad+1))
    else
        echo "  $CTL: correctly RED ($(tail -1 /tmp/x448_negctl_ctl.$$))"
    fi
    rm -f /tmp/x448_negctl_ctl.$$
done

build_and_run >/dev/null 2>&1   # leave the honest binary behind
if [ "$ctl_bad" -eq 0 ]; then
    echo "both controls fired"; exit 0
else
    echo "$ctl_bad control(s) did not fire"; exit 1
fi
