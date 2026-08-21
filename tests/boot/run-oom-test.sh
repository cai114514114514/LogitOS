#!/usr/bin/env bash
# On-device gate for the out-of-memory killer: run LogitOS on a machine that is
# too small, with TWO processes on it, and require that the one which took the
# memory is the one that dies.
#
# ---------------------------------------------------------------------------
# WHY A TEST THAT ONLY CHECKS "SOMETHING DIED" PASSES FOR THE BUG THIS FIXES
#
# Before c/kernel/mm/oom.c, fault.c returned 0 when memory was gone and the
# faulting process was terminated. Something always died -- that was never the
# problem. The problem is WHICH: on a machine one program has emptied, the next
# process to fault is essentially never that program, because it already has its
# memory and is not asking for more. It is the shell, or whatever the user just
# started.
#
# So the shape here is deliberately not mempress.as's. Two programs:
#
#   THE HOG      /usr/as/examples/oomhog.as takes most of the machine, writes a
#                pattern into every page (so reclaim's free drop tier cannot
#                quietly absorb the pressure), announces OOMHOG-RESIDENT, and
#                then stops allocating and syscalls in a loop forever.
#   THE INNOCENT /usr/as/examples/oomsmall.as then asks for a few MiB and writes
#                and reads it back. It is small, it is new, and under the old
#                behaviour it is the one that dies.
#
# THE ASSERTION IS OOMSMALL-OK. Not the presence of a kill line -- the innocent
# program completing is the only thing that distinguishes the two behaviours.
# Beside it: the kernel names the victim, the victim is the hog, init is alive,
# and fork+exec still works afterwards.
#
# ---------------------------------------------------------------------------
# NO SWAP DEVICE, ON PURPOSE
#
# With swap the machine has somewhere to put the hog's pages and nobody has to
# die -- which is the right behaviour and is already gated by `make test-swap`.
# This harness is about what happens when there is NOWHERE to put them, so the
# machine is booted without one. `make test-swap-negctl` already establishes
# that this configuration genuinely runs out (it requires the workload to FAIL),
# so the pressure here is known-real rather than assumed.
#
# ---------------------------------------------------------------------------
# THE HOG IS AN ORDINARY BACKGROUND JOB, AND THAT IS THE HARD CASE
#
# `as oomhog.as &` makes the hog a child of the serial console's /bin/sh, and
# that shell NEVER REAPS IT: sh sweeps finished background jobs in
# reap_background(), which is called only from its INTERACTIVE loop, and the
# serial console runs the non-interactive branch. proc_reap() (the window
# manager's loop) only collects ORPHANS. So when the killer marks the hog, the
# hog exits and becomes a zombie STILL HOLDING EVERY FRAME IT TOOK, for as long
# as that shell lives.
#
# A kill that frees nothing is not a kill, so c/kernel/mm/oom.c strips zombie
# address spaces itself -- tier 0 before choosing anyone, and again on every
# park of the wait (see oom_task_reap_dead() in c/kernel/exec/proc.c). This
# shape puts that mechanism directly under test rather than staging around it.
#
# An earlier version of this script DID stage around it, with
# `sh -c 'as oomhog.as &'` so the inner shell would exit and orphan the hog.
# It is recorded here because it failed for an unrelated reason worth knowing:
#
#     [execve] /bin/as: read failed
#     [execve] /as: permission denied (not executable)
#     sh: command not found: as
#
# A nested /bin/sh on this machine could not exec /bin/as at all -- PATH
# resolution found it and the READ failed. That is a real defect in something,
# and it is not this line's; it is reported rather than worked around.
#
# Usage: run-oom-test.sh <iso> <disk.img> [ram-MiB] [hog-MiB] [small-MiB]
set -u

. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-oom-test.sh <iso> <disk.img> [ram] [hog] [small]}"
DISK="${2:?usage: run-oom-test.sh <iso> <disk.img> [ram] [hog] [small]}"
RAM="${3:-320}"
HOG="${4:-200}"
SMALL="${5:-96}"

QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${OOM_LOG:-$(mktemp)}"
HOGWAIT="${OOM_HOGWAIT:-260}"
SMALLWAIT="${OOM_SMALLWAIT:-260}"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

fail=0
say() { echo "$1"; }
bad() { echo "FAIL: $1"; fail=1; }

NET="-netdev user,id=n0 -device e1000,netdev=n0"

echo "=== oom killer: ram=${RAM}MiB hog=${HOG}MiB innocent=${SMALL}MiB, no swap ==="

{
  logit_wait_for_shell "$LOG" 150
  printf 'echo OOM_BEGIN\n';                                              sleep 1
  # THE INNOCENT GOES FIRST, and this order is forced rather than chosen -- see
  # the header of oomsmall.as. /bin/as cannot be exec'd once the hog is
  # resident ("bad aex header", with 64 MiB still free and kmalloc never
  # refusing), so a process that is meant to be alive when memory runs out has
  # to be started while it still can be. It waits by watching the machine's own
  # free-frame count, so nothing here depends on how fast this host is.
  printf 'as /usr/as/examples/oomsmall.as %s &\n' "$SMALL";                sleep 12
  printf 'echo OOM_SMALL_STARTED\n';                                       sleep 2
  # The hog, as an ordinary background job -- whose zombie this shell will never
  # reap. See the header: that is the case, not an accident of the harness.
  printf 'as /usr/as/examples/oomhog.as %s &\n' "$HOG";                    sleep "$HOGWAIT"
  printf 'echo OOM_HOG_STARTED\n';                                         sleep "$SMALLWAIT"
  printf 'echo OOM_SMALL_DONE\n';                                          sleep 2
  # Liveness: a fork+exec needs an address space, a stack and a fault each, so a
  # kernel that survived by luck fails here.
  printf 'uname\n';                                                       sleep 4
  printf 'echo OOM_ALIVE\n';                                              sleep 2
  # The killer's own counters, from ring 3, through MMCTL_OOM (=5).
  printf 'echo print(syscall(94,0,5,0)) > /oomrep.as\n';                  sleep 2
  printf 'as /oomrep.as\n';                                               sleep 8
  printf 'echo print(syscall(94,0,2,0)) > /oomaud.as\n';                  sleep 2
  printf 'as /oomaud.as\n';                                               sleep 8
  printf 'echo OOM_DONE\n';                                               sleep 3
  printf 'rm /oomrep.as\nrm /oomaud.as\nexit\n';                          sleep 2
} | $QEMU -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m "${RAM}M" ${OOM_SMP:--smp 4 -accel tcg,thread=multi} \
      -vga none -device virtio-gpu-pci \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 3200); do
    grep -aq "OOM_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.25
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "----- the programs -----"
grep -a -e "OOMHOG-" -e "OOMSMALL-" "$LOG" | head -14
echo "----- the killer -----"
grep -a "^\[oom\]" "$LOG" | head -14
echo "----- counters -----"
grep -a "\[oomstat\]" "$LOG" | tail -1
echo "----------------------"

stat_of() {   # stat_of <key>
    grep -a "\[oomstat\]" "$LOG" | tail -1 | sed -n "s/.*[^_a-z]$1=\([0-9-]*\).*/\1/p"
}

grep -ac "LOGIT_BOOT_OK" "$LOG" | grep -q '^[1-9]' \
    || bad "the kernel did not boot at ${RAM} MiB"

# --- the pressure has to have been real ------------------------------------
# Every assertion below is meaningless if the machine never ran out, and "the
# machine never ran out" looks exactly like "everything worked".
grep -aq "OOMHOG-RESIDENT" "$LOG" \
    || bad "the hog never became resident -- no pressure was applied at all"

# THE INNOCENT MUST HAVE WAITED FOR THE PRESSURE. It reports the free-frame
# count it saw when it stopped waiting; if that number is high it allocated into
# an empty machine and its OOMSMALL-OK means nothing at all. This is a real
# result from 2026-08-20, not a hypothetical: with too small a poll budget the
# innocent gave up waiting after 12 seconds, found 67,821 frames free, and
# passed -- the whole harness green with the killer never invoked.
GO=$(grep -a "OOMSMALL-GO free frames" "$LOG" | head -1)
if [ -z "$GO" ]; then
    bad "the innocent never reported what it was waiting for"
else
    echo "  $GO"
    GOFREE=$(echo "$GO" | sed -n 's/.*free frames \([0-9]*\).*/\1/p')
    if [ "${GOFREE:-999999}" -gt 20000 ]; then
        bad "the innocent stopped waiting with ${GOFREE} frames free -- it allocated
      into a machine that was NOT out of memory, so OOMSMALL-OK is not evidence
      of anything. Raise MAXPOLL in oomsmall.as or the hog's size."
    fi
fi
if ! grep -aq "^\[oom\]" "$LOG"; then
    bad "the out-of-memory path never ran. The machine did not run out of memory,"
    echo "      so nothing below is evidence. Raise the hog (4th argument) or"
    echo "      lower the RAM (3rd)."
fi

# --- THE ASSERTION ---------------------------------------------------------
if grep -aq "OOMSMALL-OK" "$LOG"; then
    say "PASS: the innocent process survived and its pages were byte-correct"
elif grep -aq "OOMSMALL-CORRUPT" "$LOG"; then
    bad "the innocent process came back with WRONG DATA -- a frame was handed to two owners"
    grep -a "OOMSMALL-BAD" "$LOG" | head -4
else
    bad "the innocent process did not complete. This is the pre-oom.c behaviour:"
    echo "      the process that dies is whoever touched memory next, and that is"
    echo "      the small new program rather than the hog holding ${HOG} MiB."
    grep -a "OOMSMALL-" "$LOG" | tail -3 | sed 's/^/      /'
fi

# --- and the victim has to have been the RIGHT one -------------------------
VICTIM=$(grep -a "^\[oom\] victim:" "$LOG" | head -1)
if [ -z "$VICTIM" ]; then
    # Not automatically a failure: if the zombie tier answered the shortage,
    # nobody had to die, and that is a better outcome rather than a missing one.
    if grep -aq "recovered from .* had already exited" "$LOG"; then
        say "NOTE: no live process was killed -- the shortage was answered by memory"
        say "      belonging to processes that had already exited. Reported, not asserted."
    else
        bad "no victim was ever named. A process that vanishes with no record is"
        echo "      indistinguishable from a crash, which is the whole point of the line."
    fi
else
    echo "  $VICTIM"
    # BY SIZE, NOT BY NAME. Both programs are /bin/as, so the process name is
    # "as" for the hog AND for the innocent -- a grep for it matches whichever
    # died and cannot fail, which is a check that only looks like one. The
    # resident set separates them completely: the hog holds HOG MiB and the
    # innocent, at the moment of the shortage, holds what was left over (about
    # 65 MiB in the measured run). Half the hog's page count sits between them
    # with a wide margin either side.
    VRSS=$(echo "$VICTIM" | sed -n 's/.*rss=\([0-9]*\) frames.*/\1/p')
    MINVICTIM=$(( HOG * 256 / 2 ))
    if [ "${VRSS:-0}" -lt "$MINVICTIM" ]; then
        bad "the victim held ${VRSS} frames, less than half the hog's ${MINVICTIM}.
      The WRONG process was chosen -- most likely the innocent one, which is
      exactly the behaviour c/kernel/mm/oom.c exists to replace."
    else
        say "PASS: the victim was the hog -- ${VRSS} frames, against a ${MINVICTIM}-frame bar"
    fi
    # It must NOT have been the console shell. proc_kill() refuses that
    # structurally, so this is a check on the refusal still being wired, and it
    # is the one failure here that takes the machine down.
    grep -aq "^\[oom\] victim: pid [0-9]* \"sh\"" "$LOG" \
        && bad "THE CONSOLE SHELL WAS KILLED -- init is not protected"
fi

# --- containment: everything else kept working -----------------------------
grep -aq "OOM_ALIVE" "$LOG" || bad "the shell stopped answering after the kill"
grep -aq "LogitOS x86_64" "$LOG" || bad "fork+exec did not work after the kill"

# --- the counters agree with the log ---------------------------------------
KILLS=$(stat_of kills);   SAVED=$(stat_of saved)
NOVIC=$(stat_of novictim); REAPS=$(stat_of reaps)
EXPIRED=$(stat_of expired); RFRAMES=$(stat_of reapedframes)
if [ -z "${KILLS:-}" ]; then
    bad "MMCTL_OOM printed no [oomstat] line -- the killer is not readable from ring 3"
else
    FSAVED=$(stat_of faultsaved); FRETRY=$(stat_of faultretry)
    say "COUNTERS: kills=${KILLS} saved=${SAVED} novictim=${NOVIC} expired=${EXPIRED} reaps=${REAPS} reapedframes=${RFRAMES}"
    say "          faultretry=${FRETRY} faultsaved=${FSAVED}"
    # `faultsaved` is THE number this whole line exists to make nonzero: page
    # faults that would have ended a process before oom.c existed and instead
    # COMPLETED. Asserted on rather than `saved`, which is the optimistic half
    # (memory was available; the fault may still have lost the race for it).
    [ "${FSAVED:-0}" -gt 0 ] \
        || bad "no fault was ever rescued (faultsaved=0) -- whatever else happened,
      no process was saved from dying, which is the whole claim"
fi

AUD=$(grep -a "\[mm\] audit:" "$LOG" | tail -1)
if [ -z "$AUD" ]; then
    bad "the on-device audit did not run"
else
    echo "  $AUD"
    echo "$AUD" | grep -q "audit: 0 inconsistencies" \
        || bad "the frame allocator and the reverse map disagree after the kill"
fi

if [ "$fail" -ne 0 ]; then
    echo "----- serial tail -----"
    tail -60 "$LOG"
    echo "-----------------------"
    echo "  full log: $LOG"
    exit 1
fi
[ -n "${OOM_LOG:-}" ] || rm -f "$LOG"
echo "PASS: on-device out-of-memory killer"
