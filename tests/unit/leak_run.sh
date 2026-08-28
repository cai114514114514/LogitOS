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
     $ROOT/tests/unit/mmstub/mm_hoststub.c \
     $MM/kheap.c $MM/pmm.c $MM/vmm.c $MM/fault.c $MM/vma.c \
     $MM/rmap.c $MM/reclaim.c $MM/swap.c $MM/pcache.c $MM/shm.c $MM/oom.c"
# pcache.c: fault.c and vma.c call pcache_get/pcache_file_put/pcache_report
# since the file-backed page cache landed, so this list stopped linking the
# moment that happened. Nothing said so -- no suite reaches test-leak.
#
# shm.c and oom.c are the same story told twice more, and mm_run.sh's list now
# carries both for the same reasons. shm.c is the plain case: fault.c's shared
# page and vma.c's segment-backed areas call shm_frame/shm_pages/shm_ref/
# shm_put/shm_report directly, so it is a link error and not a coverage choice.
#
# oom.c is the one with a second cause underneath it. kheap.c calls
# oom_kheap_fail(), pmm.c calls oom_alloc_fail() and fault.c calls
# oom_fault_retry(), and all three are declared __attribute__((weak))
# specifically so THIS script would keep linking without oom.c --
# tests/unit/oom_run.sh:9 says so in as many words. That is the ELF idiom, and
# it does not hold on the documented development host: Mach-O needs
# `weak_import` for a nullable undefined symbol, so on macOS/Apple Silicon each
# weak reference is a hard link error, and this suite reported
#
#     Undefined symbols for architecture arm64:
#       "_kheap_cpu_index" "_oom_alloc_fail" "_oom_fault_retry"
#       "_oom_kheap_fail" "_shm_frame" "_shm_pages" "_shm_put" "_shm_ref"
#       "_shm_report" "_tlb_flush_all"
#     FAIL: leak_kheap_test did not build
#
# -- ten symbols, and the two negative controls this file exists to watch fail
# had therefore never been built here at all. Linking the real oom.c is
# behaviour-neutral for what is measured: leak_kheap_test registers no task
# through oom_test_add(), so gather() returns 0 and oom_kill() returns
# OOM_NO_VICTIM, which is exactly the path the absent hook took.
#
# mm_hoststub.c covers the two symbols that are NOT in c/kernel/mm at all --
# tlb_flush_all (c/kernel/cpu/tlb.c) and kheap_cpu_index (c/kernel/cpu/
# percpu.c), both weak for the same reason and both undefined here for it.

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
#
# THE WAIVER BELOW IS AIMED AT ONE WARNING, IN THE CONTROL BUILDS ONLY, and it
# is aimed at the CONTROL rather than at the code under test. -DKHEAP_NO_COALESCE
# compiles out the only caller of kheap.c's blk_prev() (kheap.c:309, inside
# `#ifndef KHEAP_NO_COALESCE`), so the helper is necessarily unused in that
# build. GCC does not warn about an unused `static inline`; clang does, so on
# the documented development host the control did not compile:
#
#     c/kernel/mm/kheap.c:143:30: error: unused function 'blk_prev'
#         [-Werror,-Wunused-function]
#     FAIL: the negative control did not build
#
# That is the apparatus penalising a control for being a control -- and "did not
# build" is the one outcome this script treats as proving nothing at all, so
# with -Werror on, the KHEAP_NO_COALESCE half of the pair could never be watched
# to fail on this host. The waiver is one warning class and applies to the two
# negctl builds; the shipped build above is compiled -Werror, unchanged, so the
# allocator itself buys no exemption from this line. It does NOT touch the
# runtime requirement, which is still that the control FAILS.
negctl() {
    flag="$1"; why="$2"
    echo "=== NEGATIVE CONTROL ($flag: $why) ==="
    # shellcheck disable=SC2086
    if ! $CC $FLAGS -Wno-error=unused-function "$flag" -o "$OUT/leak_negctl" $SRC; then
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
