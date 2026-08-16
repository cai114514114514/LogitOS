#!/usr/bin/env bash
# Which lock serialises concurrent kmalloc?
#
# `make test-smp` fails with "no wall-clock speedup (kmalloc still serialized by
# the BKL?)" -- a question mark the test could not answer for itself, and the
# guess in it is stale: SYS_KHEAP_STRESS is the ONE syscall on
# syscall_is_bkl_free()'s allow-list, so whatever serialises it, it is not the
# BKL.
#
# This runs the same workload with QMP attached and samples every lock's ticket
# counter before and after. A lock taken millions of times during a workload
# that took seconds is the one; a lock that barely moves is not. No guessing,
# and no need for the machine to be frozen first.
#
#   run-smp-lockprobe.sh <iso> <disk.img>
set -u

ISO="${1:?usage: run-smp-lockprobe.sh <iso> <disk.img>}"
DISK="${2:?usage: run-smp-lockprobe.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
ELF="${ELF:-build/kernel.elf}"
LOG="$(mktemp)"
SOCK="$(mktemp -u)"
QPID=""
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null
    [ -n "$QPID" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$SOCK"
}
trap cleanup EXIT

LOCKS="g_bkl kheap_lock pmm_lock g_sched_lock g_proc_lock"

{ sleep 11; printf '/bin/smptest\n'; sleep 90; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -qmp "unix:$SOCK,server,nowait" \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Wait for the prompt, then sample just before the workload and again after it.
for _ in $(seq 1 60); do grep -aq '\$' "$LOG" && break; sleep 1; done
sleep 2
echo "--- before ---"
python3 tests/boot/qmp_lockdump.py "$SOCK" "$ELF" $LOCKS

for _ in $(seq 1 120); do grep -aq 'smptest: T1=' "$LOG" && break; sleep 2; done
echo "--- after ---"
python3 tests/boot/qmp_lockdump.py "$SOCK" "$ELF" $LOCKS
echo "--- what the workload reported ---"
grep -a 'smptest:' "$LOG" | tail -1
