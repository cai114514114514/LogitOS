#!/usr/bin/env bash
# ON THE MACHINE: one process attaches to another, reads its registers and its
# memory, and is told no when it has not attached.
#
# There is no host half of this and there cannot be a useful one. Every part of
# ptrace that can be wrong is a property of the real machine: whether a ring-3
# loop reaches the stop inside c/kernel/exec/ksigframe.c within a timer tick,
# whether the saved trap frame is the tracee's, and whether walking another
# process's page table by hand lands on the right physical frame. A model of
# those is a model of the answer.
#
# The fixture is fsroot/as/examples/{ptracer,ptracee}.as -- read the header of
# ptracer.as for what each check is for. The two facts worth restating here:
#
#   * the exact one: the tracer writes a sentinel into the tracee's STACK with
#     POKEDATA, reads it back with PEEKDATA, and then reads its OWN memory at
#     the same virtual address and requires it to be unchanged. Both processes
#     are /bin/as at the same link base with a stack at the same address, so a
#     ptrace that operated on the CALLER's address space -- the easiest way to
#     get this wrong, and one that passes every other check -- would put the
#     sentinel right there. Measured: `read back 6556698125781548768, ours now
#     0`.
#   * the tracee's rip differs across four stops, so it is genuinely executing
#     between them. Four and not two, and "at least two distinct" and not
#     "different": the tracee spins in the VM's interpreter dispatch, which is
#     a few dozen instructions, so two timer interrupts land on the same one
#     often enough to matter. Measured in the control run above: stops 3 and 4
#     reported the SAME rip. A `!=` between two samples would be a gate that
#     reddens on a correct kernel every few dozen runs.
#   * a THIRD process that attached to nothing walks every pid on the machine
#     asking for registers, while the tracee is stopped, and must be refused by
#     all of them. `PTINTRUDE readable 0` is that result.
#
# -snapshot, unlike the core-dump harness: nothing here writes to the disk.

set -u

ISO="${1:?usage: run-ptrace-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-ptrace-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
WANT_INTRUDE="${WANT_INTRUDE:-0}"     # the negative control sets this to 1
LOG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 5; printf 'as /usr/as/examples/ptracer.as\n'; sleep 25; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 2 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq 'PTRACE-OK\|PTRACE-FAILED' "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "----- ptrace on the machine -----"
grep -a 'PTRACER-UP\|PTRACEE-UP\|^ok  :\|^FAIL:\|^     \|PTINTRUDE\|PTRACE-OK\|PTRACE-FAILED' "$LOG" || true
echo "---------------------------------"

if ! grep -aq 'PTRACEE-UP' "$LOG"; then
    echo "FAIL: the tracee never started -- nothing below measures ptrace"
    echo "----- serial -----"; cat "$LOG"; echo "------------------"
    exit 1
fi

# tr -d '\r' IS LOAD-BEARING. The guest's console is CRLF, so awk's third field
# is "0\r" and `[ "0\r" != "0" ]` is TRUE -- which showed up as
# `intruder could read 0 of 32 pids (want 0)` followed immediately by
# `FAIL: intruder read 0 pids, expected 0`, a harness contradicting itself on
# one line. Measured here, and it is the same CR trap this tree records in its
# shell-quoting notes.
INTRUDE="$(grep -a 'PTINTRUDE readable' "$LOG" | head -1 | tr -d '\r' | awk '{print $3}')"
if [ -z "$INTRUDE" ]; then
    echo "FAIL: the intruder never reported -- the ownership rule is unmeasured"
    exit 1
fi
echo "intruder could read $INTRUDE of 32 pids (want $WANT_INTRUDE)"
if [ "$INTRUDE" != "$WANT_INTRUDE" ]; then
    echo "FAIL: intruder read $INTRUDE pids, expected $WANT_INTRUDE"
    exit 1
fi

if [ "$WANT_INTRUDE" != "0" ]; then
    # The negative-control run. The point is the intruder, not the tracer's
    # verdict, so it is not required here -- see tests/coredump.mk's sibling
    # argument about controls asserting their own effect and nothing else.
    echo "PASS(control): with the ownership rule removed, a process that"
    echo "               attached to nothing read $INTRUDE stopped process(es)"
    exit 0
fi

if ! grep -aq 'PTRACE-OK' "$LOG"; then
    echo "FAIL: the tracer did not finish clean"
    grep -a 'PTRACE-FAILED' "$LOG" || echo "      (and it never reached a verdict)"
    exit 1
fi

echo "PASS: attach + GETREGS + PEEKDATA/POKEDATA + DETACH on a live ring-3"
echo "      process; its rip differed across four stops, a sentinel poked into"
echo "      its stack was readable there and absent from the tracer's own"
echo "      memory at the same address, and a process that did not attach was"
echo "      refused by every pid on the machine"
exit 0
