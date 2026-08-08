#!/usr/bin/env bash
# Signals, on the machine.
#
# Boots LogitOS, drives the serial shell to run /bin/sigtest, and asserts
# SIGTEST_OK. Delivery is a ring-3 mechanism -- the kernel builds a frame on a
# real user stack and enters a real handler at ring 3 -- so there is nowhere
# else it can be tested. Portable (no `timeout`); mirrors run-libc-test.sh.
#
# The disk is build/sigdisk.img, not build/disk.img: see the long comment in
# tests/signal.mk for why this test carries its own.
#
# THE FAILURE THIS EXISTS FOR is a signal frame with no FPU/SSE state, which
# makes every handler work and every floating-point computation interrupted by
# a signal silently wrong. `make test-signal-negctl` builds exactly that kernel
# and requires this harness to fail on it. So: this script must EXIT NON-ZERO
# on failure. Twenty-two harnesses in this tree print FAIL and exit 0, which
# makes them decorations; this one is the control for another target and would
# be actively misleading if it did that.
set -u
ISO="${1:?usage: run-signal-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-signal-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

# The suite sleeps ~4 s of its own (alarm(1) twice, plus a 3 s child for the
# SA_RESTART case) and runs under TCG, so the input script waits generously
# before asking for the exit.
{ sleep 6; printf '/bin/sigtest\n'; sleep 45; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    if grep -aq "SIGTEST_OK" "$LOG"; then
        echo "PASS: $(grep -a 'SIGTEST_OK' "$LOG" | tail -1)"
        grep -a "sigtest: delivered=" "$LOG" | tail -1 | sed 's/^/      /'
        exit 0
    fi
    if grep -aq "SIGTEST_FAIL" "$LOG"; then
        echo "FAIL: sigtest reported failures:"
        grep -aE "^FAIL:|^      xmm|SIGTEST_FAIL" "$LOG" | tail -30 | sed 's/^/      /'
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.3
done

echo "FAIL: SIGTEST_OK marker not seen (boot or run problem)"
# A ring-3 fault in sigtest itself, or a kernel panic, is the interesting case
# and is what these two greps are for -- "no marker" on its own says nothing.
grep -aE "FAIL:|sigtest|\[signal\]|\[fault\]|EXCEPTION" "$LOG" | tail -30 | sed 's/^/      /'
exit 1
