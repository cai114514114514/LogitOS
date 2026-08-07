#!/usr/bin/env bash
# Build + run the c/kernel/mm host tests.
#
# These compile the REAL c/kernel/mm sources with -DMM_HOSTTEST (see
# c/kernel/mm/mmhost.h) over a simulated physical memory, so the refcounting,
# the copy-on-write clone and the fault-decision table are exercised as the
# kernel will run them -- with ASan and UBSan on top, which QEMU cannot give us.
#
# Usage:  sh tests/unit/mm_run.sh [build-dir]
# Makefile equivalent (not wired yet -- the Makefile is owned by another line):
#     test-mm:
#         @sh tests/unit/mm_run.sh $(BUILD)
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build}"
CC="${CC:-cc}"
mkdir -p "$OUT"

INC="-I$ROOT/tests/unit -I$ROOT/tests/unit/mmstub -I$ROOT/c/kernel/mm"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
FLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -DMM_HOSTTEST $SAN $INC"

MM="$ROOT/c/kernel/mm"
COMMON="$ROOT/tests/unit/mm_common.c"

fail=0
run_one() {
    name="$1"; shift
    [ -f "$ROOT/tests/unit/$name.c" ] || { echo "=== $name: not present, skipped ==="; return; }
    echo "=== $name ==="
    # shellcheck disable=SC2086
    $CC $FLAGS -o "$OUT/$name" "$ROOT/tests/unit/$name.c" "$COMMON" "$@"
    if ! "$OUT/$name"; then fail=1; fi
    echo
}

run_one mm_pmm_test   "$MM/pmm.c"
run_one mm_vmm_test   "$MM/pmm.c" "$MM/vmm.c" "$MM/fault.c" "$MM/vma.c"

if [ "$fail" -ne 0 ]; then
    echo "FAIL: mm host tests"
    exit 1
fi
echo "PASS: mm host tests"
