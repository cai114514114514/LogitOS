#!/usr/bin/env bash
# Build + run the out-of-memory killer's host test (c/kernel/mm/oom.c).
#
# WHY THIS IS ITS OWN SCRIPT AND NOT THREE LINES IN mm_run.sh, which is where it
# belongs: mm_run.sh and leak_run.sh compile the SAME source list and a comment
# in both requires them to stay identical, and on the day this was written
# leak_run.sh had an in-flight edit from another line. Adding oom.c to one and
# not the other breaks a suite that is about something else entirely. The
# killer's three hooks in fault.c/pmm.c/kheap.c are declared WEAK for the same
# reason, so those two scripts keep linking and keep testing exactly what they
# tested before. When the tree is quiet, this file's body is `run_one
# mm_oom_test` + one `run_negative` in mm_run.sh and this script goes away.
#
# Usage:  sh tests/unit/oom_run.sh [build-dir]
# Makefile:  test-oom   (tests/mem.mk)
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
# The same set mm_run.sh links, plus oom.c. The whole of c/kernel/mm wired the
# way the kernel wires it -- vmm.c and rmap.c are not optional here, they are
# what produces the resident-set numbers the policy is judged on.
MMSRC="$MM/pmm.c $MM/vmm.c $MM/fault.c $MM/vma.c $MM/rmap.c $MM/reclaim.c \
       $MM/pcache.c $MM/shm.c $MM/oom.c"

# swap.c IS in the set -- reclaim.c calls it -- but it is compiled separately,
# with ONE warning demoted, and this is a quarantine rather than a preference.
# Measured 2026-08-20, before this line changed anything:
#
#     $ sh tests/unit/mm_run.sh build
#     === mm_pmm_test ===
#     c/kernel/mm/swap.c:101:12: error: 'dev_read' defined but not used
#
# So `make test-mm` is red in this tree right now, for a reason that has nothing
# to do with the memory-management code it is testing: the block line has an
# in-flight edit in swap.c that leaves a static function unreferenced under the
# submit/poll rewrite. That file is explicitly not this line's to touch.
#
# The waiver is aimed at exactly one warning in exactly one translation unit, so
# every other file -- including all of oom.c -- is still compiled -Werror. The
# alternative, dropping -Werror from FLAGS, would have quietly bought this
# line's own code the same exemption. Delete these four lines and put $MM/swap.c
# back in MMSRC the day that edit settles.
SWAPOBJ="$OUT/oom_swap.o"
# shellcheck disable=SC2086
$CC $FLAGS -Wno-error=unused-function -c -o "$SWAPOBJ" "$MM/swap.c" 2>/dev/null \
    || { echo "FAIL: swap.c does not compile even with the waiver"; exit 1; }

fail=0

echo "=== mm_oom_test ==="
# shellcheck disable=SC2086
$CC $FLAGS -o "$OUT/mm_oom_test" "$ROOT/tests/unit/mm_oom_test.c" "$COMMON" $MMSRC "$SWAPOBJ"
if ! "$OUT/mm_oom_test"; then fail=1; fi
echo

# ---------------------------------------------------------------------------
# THE NEGATIVE CONTROL.
#
# -DOOM_KILL_NEWEST replaces the selection with "the highest pid" -- a policy
# somebody could genuinely propose (the newest process is the one that just
# asked for memory, so blame it) and one that is wrong on this machine for a
# reason the control makes visible: the newest process is usually the small one
# the user just started, and the hog was started first.
#
# THE REQUIREMENT IS NOT "IT FAILS". It is that it fails on the WHICH-PROCESS
# assertions and passes every survival assertion -- init still protected, one
# kill at a time, no victim when nobody holds anything, the dead reaped before
# the living. A control that reddens everything would be indistinguishable from
# a build that does not compile, and would prove only that the test runs.
echo "=== NEGATIVE CONTROL: mm_oom_test with -DOOM_KILL_NEWEST ==="
# shellcheck disable=SC2086
if ! $CC $FLAGS -DOOM_KILL_NEWEST -o "$OUT/mm_oom_negctl" \
        "$ROOT/tests/unit/mm_oom_test.c" "$COMMON" $MMSRC "$SWAPOBJ"; then
    echo "    FAIL: the -DOOM_KILL_NEWEST build does not compile, so it is not a control"
    exit 1
fi
if "$OUT/mm_oom_negctl" >"$OUT/mm_oom_negctl.log" 2>&1; then
    echo "    FAIL: the wrong-victim build PASSED. The policy is not load-bearing,"
    echo "          or the cases do not distinguish it from the right one."
    tail -20 "$OUT/mm_oom_negctl.log" | sed 's/^/          /'
    fail=1
else
    nfail=$(grep -ac "FAIL:" "$OUT/mm_oom_negctl.log" || true)
    echo "    OK: it failed. $nfail assertions reddened:"
    grep -a "FAIL:" "$OUT/mm_oom_negctl.log" | sed 's/^/          /'
    # THE COUNT IS PINNED, measured 2026-08-20 on this machine: 12 of 84.
    # Not decoration -- "it failed" is satisfied by a control that fails for a
    # reason nobody chose (a crash, a link error, one assertion). Pinning the
    # number means a change that stops distinguishing the two policies shows up
    # as 11, and a change that makes the wrong policy break something it should
    # not shows up as 13.
    if [ "$nfail" -ne 12 ]; then
        echo "    FAIL: the control reddened $nfail assertions, expected exactly 12."
        echo "          Either the policy stopped being load-bearing (fewer) or the"
        echo "          wrong victim now breaks something that is not about victims"
        echo "          (more). Both need a person, not a re-pin."
        fail=1
    fi
    # THE CONTROL'S OWN CONTROL: the survival properties must have SURVIVED it.
    # Each of these strings is an assertion label from a refusal case; if the
    # wrong policy reddens one of them, then that case was measuring the policy
    # and not the refusal, and the suite's split is wrong.
    for must in "the console shell was not chosen" \
                "init was never even marked" \
                "and nothing was marked" \
                "the second shortage marked NOBODY new" \
                "NOBODY was killed"; do
        if grep -aq "FAIL:.*$must" "$OUT/mm_oom_negctl.log"; then
            echo "    FAIL: the control also broke a SURVIVAL property: $must"
            fail=1
        fi
    done
fi
echo

if [ "$fail" -ne 0 ]; then
    echo "FAIL: oom host tests"
    exit 1
fi
echo "PASS: oom host tests"
