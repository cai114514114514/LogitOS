#!/usr/bin/env bash
# M25 SMP concurrency proof. Boot -smp 4, drive the serial shell to run
# /bin/smptest, and assert SMP_TEST_OK -- which only prints if (a) no cross-core
# memory corruption (every child's deterministic checksum matched) and (b) genuine
# parallelism (>=2 distinct cores observed AND a wall-clock speedup only possible
# if >=2 ring-3 threads ran simultaneously). Portable (no `timeout`).

set -u

ISO="${1:?usage: run-smp-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-smp-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# Boot to the serial shell prompt (~10s), then send /bin/smptest. Serial input is
# slow (byte-buffered), so the command takes a few seconds to type; smptest then
# runs two compute batches (1 child baseline + 4 children), several seconds each
# under TCG. Give a generous window before exit.
{ sleep 11; printf '/bin/smptest\n'; sleep 75; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Poll for the success marker for up to ~90s.
for _ in $(seq 1 900); do
    if grep -aq "SMP_TEST_OK" "$LOG"; then
        echo "PASS: smptest -- no cross-core corruption + genuine multi-core parallelism"
        grep -a "smptest:" "$LOG" | tail -1
        exit 0
    fi
    if grep -aq "SMP_TEST_FAIL" "$LOG"; then
        echo "FAIL: smptest reported a failure"
        grep -a -E "smptest:|SMP_TEST_FAIL" "$LOG"
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: SMP_TEST_OK not seen within timeout"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
