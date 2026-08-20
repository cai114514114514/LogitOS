#!/usr/bin/env bash
# Build + run the c/kernel/mm host tests.
#
# These compile the REAL c/kernel/mm sources with -DMM_HOSTTEST (see
# c/kernel/mm/mmhost.h) over a simulated physical memory, so the refcounting,
# the copy-on-write clone, the fault-decision table, the reverse map and the
# reclaim/swap path are exercised as the kernel will run them -- with ASan and
# UBSan on top, which QEMU cannot give us.
#
# The last two suites are also run as NEGATIVE CONTROLS: the same test, built
# with one load-bearing check removed, and REQUIRED to fail. A guardrail nobody
# has watched fail is not a known-working guardrail.
#
# Usage:  sh tests/unit/mm_run.sh [build-dir]
# Makefile:  test-mm
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

# One source set for every suite. pmm.c now calls into the reverse map and the
# reclaim trigger, so they are no longer separable -- which is the point: the
# thing under test is the whole of c/kernel/mm, wired the way the kernel wires
# it.
# pcache.c joins for the same reason the comment above gives: fault.c's file
# case and vma.c's file-backed VMAs call straight into it, so leaving it out is
# not "one fewer thing under test", it is a link error -- which is exactly how
# it was found. The thing under test is the whole of c/kernel/mm, wired the way
# the kernel wires it.
# shm.c joins for the third time the same argument has been made in this file:
# fault.c's shared case and vma.c's segment-backed areas call straight into it,
# so leaving it out is not "one fewer thing under test", it is a link error.
MMSRC="$MM/pmm.c $MM/vmm.c $MM/fault.c $MM/vma.c $MM/rmap.c $MM/reclaim.c $MM/swap.c $MM/pcache.c $MM/shm.c"

fail=0
run_one() {
    name="$1"; shift
    [ -f "$ROOT/tests/unit/$name.c" ] || { echo "=== $name: not present, skipped ==="; return; }
    echo "=== $name ==="
    # shellcheck disable=SC2086
    $CC $FLAGS "$@" -o "$OUT/$name" "$ROOT/tests/unit/$name.c" "$COMMON" $MMSRC
    if ! "$OUT/$name"; then fail=1; fi
    echo
}

# A build with one check removed, which MUST fail. `name` is the suite, `tag`
# the -D that breaks it, and `why` what it proves when it fails.
run_negative() {
    name="$1"; tag="$2"; why="$3"
    [ -f "$ROOT/tests/unit/$name.c" ] || return
    echo "=== NEGATIVE CONTROL: $name with -D$tag ==="
    echo "    proves: $why"
    # A control that does not COMPILE proves nothing at all, and would otherwise
    # look identical to a control that failed for the right reason.
    # shellcheck disable=SC2086
    if ! $CC $FLAGS "-D$tag" -o "$OUT/${name}_$tag" \
             "$ROOT/tests/unit/$name.c" "$COMMON" $MMSRC; then
        echo "    FAIL: the -D$tag build does not compile, so it is not a control"
        fail=1
        return
    fi
    if "$OUT/${name}_$tag" >"$OUT/${name}_$tag.log" 2>&1; then
        echo "    FAIL: the build with -D$tag PASSED. The check it removes is not"
        echo "          load-bearing, or the test does not actually reach it."
        tail -20 "$OUT/${name}_$tag.log" | sed 's/^/          /'
        fail=1
    else
        echo "    OK: it failed, as required:"
        grep -a "FAIL:" "$OUT/${name}_$tag.log" | head -4 | sed 's/^/          /'
    fi
    echo
}

run_one mm_pmm_test
run_one mm_vmm_test
run_one mm_rmap_test
run_one mm_reclaim_test
run_one mm_pcache_test
run_one mm_forkfile_test
run_one mm_shm_test

run_negative mm_reclaim_test RECLAIM_NO_PIN_CHECK \
    "a pinned frame IS evicted without the pin check -- so the check is what protects a page the kernel holds a pointer into"
run_negative mm_reclaim_test RECLAIM_NO_ZERO_CHECK \
    "a page with data IS dropped instead of swapped without the zero check -- and comes back as zeroes, silently"
run_negative mm_forkfile_test VMM_FORK_REASSEMBLE \
    "a forked child's file PTE is taken apart and put back together (frame from the top, flags from the low twelve bits) instead of installed whole -- the child's copy of a NO-EXECUTE page comes back EXECUTABLE, and the program still runs, which is why only a bit-for-bit assertion catches it"
run_negative mm_shm_test SHM_FORK_COPY \
    "fork privatises a MAP_SHARED page instead of handing the child the same memory -- which is exactly what adding shared memory and not touching vmm_clone_user looks like. Nothing else breaks: the child gets the right bytes, the right permissions and a balanced refcount, rmap_audit stays clean, and both address spaces are well formed. The two processes simply stop hearing from each other on the first write, with no error anywhere. Only an assertion on FRAME IDENTITY across the fork can see it"

run_negative mm_pcache_test PCACHE_PER_OPEN \
    "keyed per-open instead of per-inode, two opens of the same file stop sharing a page and a page dropped by reclaim comes back in a DIFFERENT frame than a second handle still expects -- the exact bug the (dev,ino) key exists to prevent"

if [ "$fail" -ne 0 ]; then
    echo "FAIL: mm host tests"
    exit 1
fi
echo "PASS: mm host tests"
