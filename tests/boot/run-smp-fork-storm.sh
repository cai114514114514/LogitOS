#!/usr/bin/env bash
# Does this machine still survive a plain fork/exec storm on four cores?
#
# WHY THIS EXISTS. docs/superpowers/specs/2026-06-08-smp-bkl-deadlock.md records
# a -smp 4 g_bkl <-> g_sched_lock ABBA deadlock and marks it FIXED. Something in
# that class is back: a long run of ordinary `fork + execve + exit` wedges the
# whole machine mid-print -- no panic, no fault, every core spinning -- and the
# same run at -smp 1 completes.
#
# The program it forks is deliberately the DULLEST one available. /bin/libctest
# contains no AetherScript, no GUI, no network and no filesystem writes, so a
# failure here cannot be blamed on any of the subsystems that were being changed
# when it turned up. If this hangs, the bug is in the kernel's own scheduling
# and locking.
#
#   run-smp-fork-storm.sh <iso> <disk.img> [reps] [smp]
#
# Exit 0 = all reps completed. Exit 1 = wedged, and the last completed rep is
# printed, which is the number to bisect on.
set -u

ISO="${1:?usage: run-smp-fork-storm.sh <iso> <disk.img> [reps] [smp]}"
DISK="${2:?usage: run-smp-fork-storm.sh <iso> <disk.img> [reps] [smp]}"
REPS="${3:-40}"
SMP="${4:-4}"
QEMU="${QEMU:-qemu-system-x86_64}"
PROG="${PROG:-/bin/libctest}"
LOG="$(mktemp)"
QPID=""
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null
    [ -n "$QPID" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

# One line per rep, each followed by a marker carrying its number. The marker is
# a shell builtin, so it cannot itself fork -- if it prints, the fork before it
# returned and was reaped.
#
# NO REDIRECT, and that is not a style choice. The first version of this wrote
# "$PROG > /dev/null" and measured nothing: this machine has no /dev/null (see
# below), so the shell refused the redirect and the child exited BEFORE execve.
# It still forked, so it still wedged -- which is how a broken harness produced
# a real-looking result. The program's own output goes to the serial log now;
# the markers are what is graded.
cmds=""
for i in $(seq 1 "$REPS"); do
    cmds="${cmds}${PROG}
echo STORM-$i-OK
"
done
cmds="${cmds}echo STORM-ALL-DONE
"

ACCEL="-accel tcg"
[ "$SMP" != "1" ] && ACCEL="-accel tcg,thread=multi"

{ sleep 4; printf '%s' "$cmds"; sleep 240; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp "$SMP" $ACCEL -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

deadline=$((SECONDS + 300))
while [ $SECONDS -lt $deadline ]; do
    grep -aq "STORM-ALL-DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 2
done

last=0
for i in $(seq 1 "$REPS"); do
    grep -aq "STORM-$i-OK" "$LOG" && last=$i
done

if grep -aq "STORM-ALL-DONE" "$LOG"; then
    echo "PASS: -smp $SMP survived $REPS x fork+exec of $PROG"
    exit 0
fi
echo "FAIL: -smp $SMP wedged after $last/$REPS completed reps of $PROG"
echo "  last 6 serial lines before the stall:"
tail -6 "$LOG" | sed 's/^/    /'
exit 1
