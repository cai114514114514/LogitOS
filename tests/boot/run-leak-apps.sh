#!/usr/bin/env bash
# On-device APP-CHURN leak test: does opening and closing GUI apps give the
# memory back?
#
# WHY THIS EXISTS SEPARATELY FROM run-mm-test.sh. That test covers fork/exec/
# exit -- it runs 240 shell commands and requires the free-frame count not to
# drift. It is clean, and it always was: the fork path does not leak. The
# workload it does NOT cover is the one the machine is actually used for,
# opening and closing windowed apps, and that path allocates completely
# different things: a multi-megabyte .aex load buffer, a window surface of
# cw*ch*4 DEVICE pixels, a per-app address space, a proc, an fd table. A leak
# there is invisible to a fork test.
#
# THE DRIVER is the WM's own churn stress (make CHURN=1, c/kernel/gui/wm.c):
# it launches Terminal / Clock / Monitor in rotation and closes each one
# through the real EV_CLOSE path, ~25 steps a second, using wm_launch and the
# reaper exactly as a Dock click and a red-button click do. Nothing about the
# workload is a test harness pretending to be a user.
#
# WHAT IS MEASURED -- three counters, because the failure modes are different:
#
#   1. FREE FRAMES (pmm). The allocator announces every new all-time low, once
#      per 1 MiB step ("[mm] low:"). A busy-but-honest system stops printing
#      that line once the workload has been round once; a leaking one never
#      stops. The SLOPE over the second half of the run is the leak rate.
#   2. KERNEL HEAP ARENA (kheap). kheap takes frames from the PMM and never
#      returns them, so it can consume physical memory forever while every PMM
#      invariant holds and pmm_audit() is clean. "[kheap] grow #N" is the only
#      place that growth is attributable, and after warm-up it must stop.
#   3. INVARIANTS. pmm_bugs() must be 0 throughout -- if it is not, the frames
#      are not merely lost, they are being handed to two owners.
#
# THE TIME AXIS IS APP LAUNCHES, NOT SECONDS, and not the serial shell. The
# first version of this script punctuated the log by echoing marks from /bin/sh
# and split the run at those marks. It passed on a build whose kernel heap had
# eaten 476 of the machine's 511 MiB -- because under churn the shell was slow
# to come up, the marks landed after all the damage, and the "second half" was
# a system already flat on its face with no memory left to lose. A leak test
# that reports PASS on a machine 99.9% consumed is worse than no test.
#
# So the axis is `[wm] launched`, emitted by the launcher itself. Every reading
# is attributed to the launch count at the time, and the run is split at the
# median launch. Nothing about the measurement depends on userspace being
# responsive -- which is exactly the thing a memory leak takes away.
#
# THE TOLERANCE, and why it is not a round number picked to pass: everything
# that legitimately consumes memory once -- the font cache, the wallpaper, the
# first exec of each binary, the arenas the steady state genuinely needs --
# happens in the first half. The second half opens and closes THE SAME three
# apps again, so its cost must be ZERO new arenas: the tolerance is zero because
# the workload is exactly periodic, not because zero sounded strict. Free frames
# get one arena (1024 frames) of slack, since an arena taken legitimately late
# is not a leak.
#
# Usage:  bash tests/boot/run-leak-apps.sh <iso> <disk.img> [seconds]
set -u

ISO="${1:?usage: run-leak-apps.sh <iso> <disk.img> [seconds]}"
DISK="${2:?usage: run-leak-apps.sh <iso> <disk.img> [seconds]}"
SECS="${3:-120}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${LEAK_LOG:-$(mktemp)}"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

# LEAK_REPLAY=<log> re-runs the analysis over a serial log captured earlier,
# with no boot. The assertions below are the interesting part of this script and
# getting them right took several passes; re-deriving them costs two minutes of
# emulation each time otherwise, and a captured log is the same evidence.
if [ -n "${LEAK_REPLAY:-}" ]; then
    LOG="$LEAK_REPLAY"
    echo "(replaying $LOG -- no boot)"
else
HALF=$((SECS / 2))

# The serial console is /bin/sh. It is used only for an INDEPENDENT reading of
# free memory (`cat /dev/kstat`) that does not come from the allocator's own
# trace. Nothing the test asserts depends on it: under a heavy leak the shell
# stops answering, and a leak test must not go quiet exactly when it is right.
{
  sleep 25
  printf 'cat /dev/kstat\n'; sleep "$HALF"
  printf 'cat /dev/kstat\n'; sleep "$HALF"
  printf 'cat /dev/kstat\necho LEAK_DONE\n'; sleep 8
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      -netdev user,id=n0 -device e1000,netdev=n0 \
      -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

WAITED=0
while [ "$WAITED" -lt $((SECS + 60)) ]; do
    grep -aq "LEAK_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1; WAITED=$((WAITED + 1))
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
fi

fail=0
say() { echo "$1"; }
bad() { echo "FAIL: $1"; fail=1; }

echo "----- boot report -----"
grep -a "\[mm\] boot:" "$LOG" | head -2
echo "-----------------------"

# The churn driver has to have actually run, or everything below passes
# vacuously. Each "[s]" is 32 open/close steps.
STEPS=$(grep -ac "^\[s\]" "$LOG" || true)
LAUNCHES=$(grep -ac "\[wm\] launched" "$LOG" || true)
if [ "${LAUNCHES:-0}" -lt 20 ]; then
    bad "the app-churn driver did not run (only ${LAUNCHES:-0} launches). Build with CHURN=1."
    echo "----- serial tail -----"; tail -40 "$LOG"; exit 1
fi
say "churn: ${LAUNCHES} app launches (${STEPS} heartbeats of 32 steps)"

# One awk pass attributes every reading to the launch count at the time, and
# reports the two halves. HALF is the median launch.
read -r G1 G2 ARENA F1 F2 FEND <<EOF
$(awk -v half="$((LAUNCHES / 2))" '
  /\[wm\] launched/ { n++ }
  /\[kheap\] grow #/ {
      if (n <= half) g1++; else g2++;
      for (i = 1; i <= NF; i++) if ($i == "arena") arena = $(i+1) + 0;
  }
  /\[mm\] low:/ {
      for (i = 1; i <= NF; i++)
          if ($(i+1) == "frames" && $(i+2) == "free") f = $i + 0;
      if (n <= half) f1 = f; else f2 = f;
      fend = f;
  }
  END {
      if (f1 == 0) f1 = fend;               # no low crossed the first half
      if (f2 == 0) f2 = f1;                 # ...nor the second: nothing was lost
      printf "%d %d %d %d %d %d\n", g1+0, g2+0, arena+0, f1+0, f2+0, fend+0
  }' "$LOG")
EOF

# --- 1. kernel-heap arena ------------------------------------------------
# Every grow is 4 MiB of frames the PMM will never see again. The second half
# repeats the first half's work, so it must need none.
say "kheap: arena ${ARENA} KiB at the end; grows over the first ${LAUNCHES} launches:"
say "       $G1 in the first half, $G2 in the second"
if [ "$G2" -gt 0 ]; then
    bad "the kernel heap took $G2 more arenas ($((G2 * 4)) MiB) in the second half,"
    bad "  opening and closing the same apps it had already opened and closed."
    bad "  It is not reusing what it frees."
    grep -a "\[kheap\] grow #" "$LOG" | tail -4
else
    say "PASS: no new kernel-heap arenas over the second half of the churn"
fi

# --- 2. free frames ------------------------------------------------------
# "[mm] low:" is announced on each new all-time low, one per 1 MiB, by the
# allocator itself -- so a settled system simply stops printing it.
DROP=$((F1 - F2))
say "free frames: low-water $F1 at the half-way launch, $F2 at the end (drop $DROP = $((DROP * 4)) KiB)"
if [ "$DROP" -gt 1024 ]; then
    bad "free memory fell a further $((DROP * 4 / 1024)) MiB in the second half of the churn"
else
    say "PASS: the second half cost less than one 4 MiB arena ($DROP frames)"
fi

# --- 2b. the machine must not be out of memory --------------------------
# The floor itself, not only its slope. A leak that has already finished
# consuming the machine has a flat slope, and every relative check above would
# pass on it -- which is exactly how the first version of this script passed a
# build whose heap had taken 476 of 511 MiB.
TOTAL=$(grep -a "\[mm\] boot:" "$LOG" | head -1 | sed -n 's/.* \([0-9]*\) frames total.*/\1/p')
if [ -n "${TOTAL:-}" ] && [ "${FEND:-0}" -gt 0 ]; then
    PCT=$((FEND * 100 / TOTAL))
    say "free at the end: $FEND of $TOTAL frames (${PCT}%)"
    # Half the machine. Three small apps opening and closing cannot legitimately
    # need more than that, and below it nothing large can be launched at all.
    if [ "$PCT" -lt 50 ]; then
        bad "the churn consumed $((100 - PCT))% of physical memory opening three small apps"
    else
        say "PASS: over half of RAM is still free after ${LAUNCHES} launches"
    fi
fi
OOM=$(grep -ac "pmm_alloc_contig(.*) FAILED" "$LOG" || true)
[ "${OOM:-0}" = "0" ] || bad "the kernel heap could not get memory ${OOM} times -- the machine ran out"

# An independent reading that does not come from the allocator's own trace.
echo "----- /dev/kstat mem_free_bytes -----"
grep -a "mem_free_bytes" "$LOG" || echo "(no kstat readings -- the serial shell did not answer)"
echo "-------------------------------------"

# --- 3. invariants -------------------------------------------------------
BUGS=$(grep -ac "\[mm\] BUG:" "$LOG" || true)
[ "${BUGS:-0}" = "0" ] || bad "the frame allocator reported ${BUGS} invariant violations"
DF=$(grep -ac "\[kheap\] double free" "$LOG" || true)
[ "${DF:-0}" = "0" ] || bad "${DF} double frees of kernel heap blocks"
CY=$(grep -ac "free list corrupt" "$LOG" || true)
[ "${CY:-0}" = "0" ] || bad "${CY} corrupted kernel-heap free lists"
[ "$fail" -eq 0 ] && say "PASS: no allocator invariant violations during the churn"

# --- 4. the machine survived --------------------------------------------
# Reported, not asserted, and deliberately so: whether the shell answers is a
# consequence of the memory state, not an independent fact about it. The
# assertions that decide this test are the three above.
if grep -aq "LEAK_DONE" "$LOG"; then
    say "the serial shell was still answering at the end of the churn"
else
    say "NOTE: the serial shell did not answer by the end -- see the assertions above"
fi

if [ "$fail" -ne 0 ]; then
    echo "----- serial tail -----"
    tail -40 "$LOG"
    echo "-----------------------"
    [ -n "${LEAK_LOG:-}${LEAK_REPLAY:-}" ] || rm -f "$LOG"
    exit 1
fi
[ -n "${LEAK_LOG:-}" ] || rm -f "$LOG"
echo "PASS: app open/close churn returns its memory"
