#!/usr/bin/env bash
# Boot LogitOS and make it time its own storage path.
#
# The question this answers is "what does opening the Browser cost, and where
# does it go" -- so it runs on the real machine, against the real device, with
# the real filesystem, and prints a median with a spread rather than one sample.
# The host runs several QEMU instances concurrently and TCG throughput swings by
# a factor of two between them; a single number from here is weather.
#
# Usage: run-fsbench.sh <iso> <disk.img> [reps] [blkdev]
#   blkdev: virtio (default) | ahci | nvme -- which driver holds the root disk,
#           because "polled AHCI costs X" is a claim about AHCI and cannot be
#           measured on a virtio device.
#
# Prints the table and exits 0 if it completed. It is a MEASUREMENT, not an
# assertion: it deliberately has no pass threshold, because a threshold on a
# contended host is a flaky test and the numbers are the deliverable.

set -u

ISO="${1:?usage: run-fsbench.sh <iso> <disk.img> [reps] [blkdev]}"
DISK="${2:?usage: run-fsbench.sh <iso> <disk.img> [reps] [blkdev]}"
REPS="${3:-5}"
BLK="${4:-virtio}"
QEMU="${QEMU:-qemu-system-x86_64}"

WORK="$(mktemp -d)"
LOG="$WORK/bench.log"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"                     # a private copy: another session may hold
                                        # QEMU's write lock on the shared image
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

case "$BLK" in
  virtio) DRIVE="-drive file=$DISKC,format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0" ;;
  ahci)   DRIVE="-device ich9-ahci,id=ahci0 -drive file=$DISKC,format=raw,if=none,id=hd0 -device ide-hd,drive=hd0,bus=ahci0.0" ;;
  nvme)   DRIVE="-drive file=$DISKC,format=raw,if=none,id=hd0 -device nvme,drive=hd0,serial=bench0" ;;
  *) echo "unknown blkdev '$BLK'"; exit 2 ;;
esac

NET="-netdev user,id=n0 -device e1000,netdev=n0"

{ sleep 6; printf 'echo all %s > /dev/fsbench\n' "$REPS"; sleep 300; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" $DRIVE -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

waited=0
while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 300 ]; do
    grep -aq "BENCH-DONE" "$LOG" && break
    sleep 1; waited=$((waited + 1))
done
kill -9 "$QPID" 2>/dev/null
for _ in $(seq 1 100); do kill -0 "$QPID" 2>/dev/null || break; sleep 0.1; done
tr -d '\r' <"$LOG" >"$LOG.n" && mv "$LOG.n" "$LOG"

echo "===== fsbench ($BLK, reps=$REPS, TCG -smp 4) ====="
grep -a "^\[bench\]" "$LOG" | sed 's/^\[bench\] //'
echo "================================================="

if ! grep -aq "BENCH-DONE" "$LOG"; then
    echo "FAIL: benchmark did not complete"
    grep -aE "panic|fault|\[fs\]|\[blk\]|\[part\]" "$LOG" | tail -20
    exit 1
fi
exit 0
