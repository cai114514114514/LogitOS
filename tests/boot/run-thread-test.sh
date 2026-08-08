#!/usr/bin/env bash
# M30 threads gate. Boot -smp 4, drive the serial shell to run /bin/thrtest, and
# assert THREAD_TEST_OK -- which only prints if every one of the checks in
# c/apps/coreutils/thrtest.c held: wall-clock parallelism between two ring-3
# threads of ONE process, per-thread `__thread` storage, a mutex that actually
# excludes under contention on 4 cores, join delivering the exit value, detach
# freeing without a join, and 2000 create/join cycles that leave the kernel's
# descriptor table back where it started.
#
# -smp 4 is not decoration. The concurrency check compares the wall time of one
# thread against four; on one core there is nothing to measure and the test
# would be asserting that the scheduler time-slices.
#
# Portable: no `timeout`, same shape as run-smp-test.sh.

set -u

ISO="${1:?usage: run-thread-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-thread-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# Boot to the serial shell (~11s), type the command (serial input is byte-
# buffered and slow), then give the program its window: the two timed batches
# plus 2000 create/join cycles take a while under TCG.
{ sleep 11; printf '/bin/thrtest\n'; sleep 150; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 1800); do
    if grep -aq "THREAD_TEST_OK" "$LOG"; then
        echo "PASS: thrtest -- threads are concurrent, TLS is per-thread, the mutex excludes, join/detach do not leak"
        grep -a -E "^(ok  |FAIL)|thrtest:" "$LOG"
        exit 0
    fi
    if grep -aq "THREAD_TEST_FAIL" "$LOG"; then
        echo "FAIL: thrtest reported a failure"
        grep -a -E "^(ok  |FAIL)|thrtest:|THREAD_TEST_FAIL" "$LOG"
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: THREAD_TEST_OK not seen within timeout"
echo "----- serial output -----"
tail -120 "$LOG"
echo "-------------------------"
exit 1
