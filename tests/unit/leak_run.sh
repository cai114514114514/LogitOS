#!/usr/bin/env bash
# Build + run the kernel-heap leak test, TWICE: once as shipped, and once with
# the fix compiled out.
#
# The second run is the point. A leak assertion that has never been watched to
# fail is not known to be capable of failing -- it may be measuring the wrong
# counter, or asserting something that is true either way. So the same binary
# is rebuilt with -DKHEAP_NO_SPLIT, which restores exactly the whole-block reuse
# c/kernel/mm/kheap.c had before (see split_block()), and the script requires it
# to FAIL. If it passes, this suite is not testing anything and says so.
#
# Both builds run under ASan + UBSan, which is why the allocator is compiled for
# the host at all: a heap bug in QEMU is a freeze somewhere else, later.
#
# Usage:  sh tests/unit/leak_run.sh [build-dir]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build}"
CC="${CC:-cc}"
mkdir -p "$OUT"

INC="-I$ROOT/tests/unit -I$ROOT/tests/unit/mmstub -I$ROOT/c/kernel/mm"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
FLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -DMM_HOSTTEST $SAN $INC"

MM="$ROOT/c/kernel/mm"

# pmm.c is no longer a leaf. It calls rmap_init() at the end of pmm_init and
# reclaim_on_alloc() on every allocation, so the frame allocator now drags in
# the reverse map and the reclaim path, which in turn need swap/vmm/vma/fault.
# That is deliberate -- reclaim has to be reachable from the one place every
# frame in the system is handed out -- but it means this suite stopped linking
# the moment it landed, and a suite that does not build is a suite that cannot
# fail. Same source set as tests/unit/mm_run.sh, for the same reason: the thing
# under test is c/kernel/mm wired the way the kernel wires it.
SRC="$ROOT/tests/unit/leak_kheap_test.c $ROOT/tests/unit/mm_common.c \
     $MM/kheap.c $MM/pmm.c $MM/vmm.c $MM/fault.c $MM/vma.c \
     $MM/rmap.c $MM/reclaim.c $MM/swap.c"

fail=0

echo "=== leak_kheap_test (as shipped: blocks are split) ==="
# shellcheck disable=SC2086
if ! $CC $FLAGS -o "$OUT/leak_kheap_test" $SRC; then
    echo "FAIL: leak_kheap_test did not build"; exit 1
fi
if ! "$OUT/leak_kheap_test"; then
    echo "FAIL: the kernel heap leaks arena on repeated open/close cycles"
    fail=1
fi
echo

# Two negative controls, because the fix has two halves and each half alone is
# insufficient in a DIFFERENT way -- which is the single most surprising thing
# this work found:
#   KHEAP_NO_SPLIT     whole-block reuse, the allocator as it was. Over-
#                      allocates catastrophically; live_bytes balloons.
#   KHEAP_NO_COALESCE  split but never merge. Makes the arena leak WORSE than
#                      the original (measured at ~2.7 MB per open/close cycle),
#                      because the heap grinds itself into pieces too small for
#                      the next window surface while staying 90% free.
negctl() {
    flag="$1"; why="$2"
    echo "=== NEGATIVE CONTROL ($flag: $why) ==="
    # shellcheck disable=SC2086
    if ! $CC $FLAGS "$flag" -o "$OUT/leak_negctl" $SRC; then
        echo "FAIL: the negative control did not build"; return 1
    fi
    if "$OUT/leak_negctl" > "$OUT/leak_negctl.log" 2>&1; then
        echo "FAIL: the negative control PASSED -- with $flag the heap must"
        echo "      misbehave, so this suite is not measuring what it claims."
        tail -20 "$OUT/leak_negctl.log"
        return 1
    fi
    echo "PASS: with $flag the test fails, as it must:"
    grep -a "arena after warm-up\|  FAIL:" "$OUT/leak_negctl.log" | head -6
    return 0
}

negctl -DKHEAP_NO_SPLIT    "whole-block reuse, the allocator as it was" || fail=1
echo
negctl -DKHEAP_NO_COALESCE "split but never merge -- worse than the original" || fail=1
echo

if [ "$fail" -ne 0 ]; then
    echo "FAIL: kheap leak tests"
    exit 1
fi
echo "PASS: kheap leak tests (fix asserted, and asserted to be assertable)"
