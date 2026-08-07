#!/usr/bin/env bash
# ============================================================================
# run-kbench.sh -- what the kernel's own primitives cost, on the machine.
#
# WHAT THIS IS FOR
# ----------------
# tools/perf/perfbench.py measures WORKLOADS (boot, a shell round trip, a page
# load) from the host. tools/perf's sampling profiler answers "which function".
# This answers the third question neither of them can: what does ONE syscall
# entry, ONE context switch, ONE frame allocation cost, and how much of the
# machine is the big kernel lock eating?
#
# The measurement lives in the kernel (c/kernel/sched/kbench.c) because that is
# the only place that can time a 200 ns operation ten thousand times. This
# script boots it, gives it a real workload to account for, and asserts on the
# numbers it prints.
#
# METHOD, so the numbers can be argued with
# -----------------------------------------
#   - Every duration is measured BY THE GUEST with rdtsc, converted using the
#     frequency time.c calibrated. The host is shared with other agents' QEMU
#     processes, so nothing here is timed from the host.
#   - Every microbenchmark line carries min / median / max over 7 repetitions
#     inside one boot. A single number would be weather.
#   - `--repeat N` boots N times and this script reports the median ACROSS
#     boots for each asserted metric, with the full spread printed.
#   - The path counters (BKL wait/hold, kernel entries by class, syscalls by
#     number) are armed from sched_init() and disarmed when the report prints,
#     so their window is "boot through desktop live plus this shell workload" --
#     the busiest, most contended window the machine has.
#
# THE ASSERTIONS ARE A NEGATIVE CONTROL
# -------------------------------------
# Each threshold below is set so that it FAILS against the kernel as it was
# before this line's changes, and passes after. Measured values, both sides:
#
#   metric                              before      after     threshold
#   SYS_GET_TIME mean in handler        5626 ns     404 ns    < 2000 ns
#   pages shared per fork               284         30        < 100
#   anonymous page faults               0           2         >= 1
#
# Those three are measured, both sides, by `make test-kbench-negctl`.
#
# The fourth assertion, `bkl_hlt_wait poll passes < 200`, is a WATCHDOG and not
# a control, and saying so is the honest version: measured 137 before and 144
# after, because in this workload /bin/sh's children exit before their parent
# reaches waitpid, so the poll it used to do almost never happened. The counter
# is kept because it is the number M27 exists to drive to zero and a new poll
# loop anywhere in the kernel will show up in it -- 105 of the ~140 are the
# wait-queue self-test's own deliberate control. It is not evidence that this
# line's waitqueue conversion helped; the case that one does is the GUI
# Terminal's pipe, which this serial harness does not drive.
#
# Run it against a build with those changes reverted and it must report FAIL.
# A test that has never been seen to fail is not evidence of anything.
#
# The last one is the important one, and it is a CORRECTNESS gate wearing a
# performance test's clothes. exec no longer maps a program's megabyte of stack
# eagerly; it reserves it and lets the pages arrive on first touch. Every
# coreutil on this disk uses so little stack that it never touches an unmapped
# page, so the demand-paging path would never run and the first program with a
# deep call chain would be the one to discover whether it works. /bin/as has
# one, so the workload below runs it and the test requires the fault path to
# have fired. On a kernel that maps the stack eagerly, `anon` is 0 and this
# fails -- which is exactly what a negative control is.
#
# Usage:  bash tests/boot/run-kbench.sh <iso> <disk.img> [repeat]
# ============================================================================
set -u

ISO="${1:?usage: run-kbench.sh <iso> <disk.img> [repeat]}"
DISK="${2:?usage: run-kbench.sh <iso> <disk.img> [repeat]}"
REPEAT="${3:-1}"
QEMU="${QEMU:-qemu-system-x86_64}"
SMP="${KBENCH_SMP:-4}"

# The kernel starts its battery 6 s in; the shell workload below runs before and
# during it so the path counters see fork/exec/wait traffic and not an idle
# desktop. 75 s is generous: under a contended host a boot that normally takes
# 2 s has been seen to take 20.
DEADLINE="${KBENCH_DEADLINE:-75}"

# One batch of fork+execve+waitpid round trips. This is what makes the syscall
# histogram and the BKL numbers describe a machine doing work.
workload() {
    i=0
    while [ "$i" -lt 40 ]; do printf 'true\n'; i=$((i+1)); done
    # A program with a real call chain, so the demand-paged exec stack is
    # exercised rather than assumed. It also prints its own answer, so a stack
    # that faulted in WRONG is visible as a wrong number and not just as a
    # counter that moved.
    printf '/bin/as /usr/as/examples/fib.as\n'
}

run_one() {
    LOG="$1"
    {
        sleep 5
        workload
        sleep "$DEADLINE"
    } | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
          -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
          -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
          -m 512M -smp "$SMP" -accel tcg,thread=multi \
          -vga none -device virtio-gpu-pci \
          -netdev user,id=n0 -device e1000,netdev=n0 \
          -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
    QPID=$!
    n=0
    while [ "$n" -lt $((DEADLINE * 10)) ]; do
        grep -aq "KBENCH_DONE" "$LOG" && break
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
        n=$((n+1))
    done
    kill "$QPID" 2>/dev/null
    wait "$QPID" 2>/dev/null
}

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

fail=0
bad() { echo "FAIL: $1"; fail=1; }
say() { echo "PASS: $1"; }

# Pull one number out of a log. Every extractor is a separate function so a
# format change breaks in one obvious place rather than silently returning "".
get_systime()  { sed -n 's/.*SYS 10: .* mean \([0-9]*\)\.[0-9]* ns in handler.*/\1/p' "$1" | head -1; }
get_forkpages(){ grep -a '\[mm\] fork:' "$1" | tail -1 | \
                 sed -n 's/.* \([0-9]*\) forks, \([0-9]*\) pages shared.*/\2 \1/p'; }
get_hltwaits() { sed -n 's/.*poll passes through bkl_hlt_wait.*/&/p' "$1" | tail -1 | \
                 sed -n 's/.*switches, [0-9]* threads parked now, \([0-9]*\) poll passes.*/\1/p'; }
get_bugs()     { grep -a '\[mm\] kbench:' "$1" | head -1 | sed -n 's/.* \([0-9]*\) bugs.*/\1/p'; }
get_anon()     { grep -a '\[mm\] kbench:' "$1" | sed -n 's/.*reused, \([0-9]*\) anon.*/\1/p' | head -1; }
get_fib()      { grep -a 'fib(20) = 6765' "$1" | head -1; }
get_ttyper()   { sed -n 's/.*tty console wait: .* -- \([0-9]*\)\/1000 of ONE core.*/\1/p' "$1" | head -1; }

median() {  # median of the numbers on stdin; "" if there are none
    sort -n | awk '{ a[NR] = $1 } END { if (NR == 0) exit; print a[int((NR+1)/2)] }'
}

SYSTIME_ALL=""; FORKPAGES_ALL=""; HLT_ALL=""; ANON_ALL=""
r=0
while [ "$r" -lt "$REPEAT" ]; do
    r=$((r+1))
    LOG="$TMP/boot$r.log"
    run_one "$LOG"
    if ! grep -aq "KBENCH_DONE" "$LOG"; then
        bad "boot $r did not reach KBENCH_DONE within ${DEADLINE}s"
        echo "----- serial tail -----"; tail -40 "$LOG"; echo "-----------------------"
        continue
    fi

    if [ "$r" -eq 1 ]; then
        echo "===== kbench, boot 1 of $REPEAT (-smp $SMP, TCG) ====="
        grep -a '\[kbench\]' "$LOG" | grep -av 'log-cost probe' | sed 's/^.*\[kbench\]/[kbench]/'
        grep -a '\[exec\] ' "$LOG" | tail -1
        grep -a '\[mm\] kbench:' "$LOG"
        echo "====================================================="
    fi

    v="$(get_systime "$LOG")";               [ -n "$v" ] && SYSTIME_ALL="$SYSTIME_ALL$v
"
    set -- $(get_forkpages "$LOG")
    if [ $# -eq 2 ] && [ "${2:-0}" -gt 0 ]; then
        FORKPAGES_ALL="$FORKPAGES_ALL$(( $1 / $2 ))
"
    fi
    v="$(get_hltwaits "$LOG")";              [ -n "$v" ] && HLT_ALL="$HLT_ALL$v
"

    v="$(get_anon "$LOG")";                  [ -n "$v" ] && ANON_ALL="$ANON_ALL$v
"
    if [ -z "$(get_fib "$LOG")" ]; then
        bad "boot $r: /bin/as did not print fib(20) = 6765 -- a program with a real call chain did not run correctly on the demand-paged stack"
    fi

    b="$(get_bugs "$LOG")"
    [ "${b:-1}" = "0" ] || bad "boot $r: the frame allocator reported ${b:-?} invariant violations"
    t="$(get_ttyper "$LOG")"
    [ -n "$t" ] && echo "note: boot $r console wait used ${t}/1000 of one core"
done

# ---- the asserted metrics, median across boots -------------------------------
SYSTIME="$(printf '%s' "$SYSTIME_ALL" | median)"
FORKPAGES="$(printf '%s' "$FORKPAGES_ALL" | median)"
HLT="$(printf '%s' "$HLT_ALL" | median)"
ANON="$(printf '%s' "$ANON_ALL" | median)"

echo "----- medians across $REPEAT boot(s) -----"
echo "SYS_GET_TIME in handler : ${SYSTIME:-?} ns   (all: $(echo $SYSTIME_ALL))"
echo "pages shared per fork   : ${FORKPAGES:-?}      (all: $(echo $FORKPAGES_ALL))"
echo "bkl_hlt_wait poll passes: ${HLT:-?}      (all: $(echo $HLT_ALL))"
echo "anonymous page faults   : ${ANON:-?}      (all: $(echo $ANON_ALL))"
echo "-----------------------------------------"

if [ -z "${ANON:-}" ]; then
    bad "no [mm] kbench: fault accounting"
elif [ "$ANON" -lt 1 ]; then
    bad "0 anonymous page faults -- the demand-paged exec stack never faulted, so it is untested (an eagerly mapped stack looks exactly like this)"
else
    say "$ANON anonymous page faults -- the demand-paged exec stack really is faulted in on use"
fi

if [ -z "${SYSTIME:-}" ]; then
    bad "no SYS_GET_TIME line in the syscall histogram (did the workload run?)"
elif [ "$SYSTIME" -ge 2000 ]; then
    bad "SYS_GET_TIME costs ${SYSTIME} ns in the handler (limit 2000) -- it is reading the CMOS on every call"
else
    say "SYS_GET_TIME costs ${SYSTIME} ns in the handler (limit 2000)"
fi

if [ -z "${FORKPAGES:-}" ]; then
    bad "no [mm] fork: accounting (did /bin/sh fork?)"
elif [ "$FORKPAGES" -ge 100 ]; then
    bad "fork shares ${FORKPAGES} pages per fork (limit 100) -- the child is inheriting an eagerly mapped stack"
else
    say "fork shares ${FORKPAGES} pages per fork (limit 100)"
fi

if [ -z "${HLT:-}" ]; then
    bad "no scheduler line (kbench did not finish?)"
elif [ "$HLT" -ge 200 ]; then
    bad "$HLT poll passes through bkl_hlt_wait (limit 200) -- something is still polling instead of blocking"
else
    say "$HLT poll passes through bkl_hlt_wait (limit 200)"
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL: kbench"
    exit 1
fi
echo "PASS: kbench"
