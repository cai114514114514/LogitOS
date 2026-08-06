#!/usr/bin/env bash
# Does the journal actually ask the hardware for the ordering it depends on?
#
# LogitFS v4 has a write-ahead log, and a log's whole claim is an ordering: the
# blocks land, THEN the commit record, THEN the checkpoint. But a completed block
# write only means the DEVICE accepted the block -- a disk reorders freely inside
# its own write cache, so without a barrier between those steps the claim is not
# something the hardware ever agreed to.
#
# That is not theoretical here. The kernel prints what the device told it at boot:
#     [virtio-blk] ready, cache=writeback (barriers REQUIRED)
#
# This asserts both halves: that the device says it has a cache, and that a file
# write issues barriers rather than merely appearing to be ordered. The count is
# the kernel's own (sysinfo "Barriers"), so it measures what the filesystem asked
# of the hardware, not what the source looks like it does.

set -u

ISO="${1:?usage: run-barrier-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-barrier-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
# Work on a COPY of the image. -snapshot still opens the backing file for write
# in order to take QEMU's image lock, so pointing at the shared build/disk.img
# makes this test fail whenever anything else in the tree is booting -- a real
# problem in a repo two people build in at once, and one whose error message
# ("Failed to get shared write lock") looks nothing like its cause.
WORK="$(mktemp -d)"
cp "$DISK" "$WORK/disk.img"
DISK="$WORK/disk.img"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -f "$LOG"; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 5; printf 'mkdir /dur\nas /usr/as/examples/barriers.as\necho BARRIER-RUN-DONE\n'; sleep 12; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "BARRIER-RUN-DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.2
done
kill -9 "$QPID" 2>/dev/null; QPID=""
tr -d '\r' <"$LOG" >"$LOG.n" && mv "$LOG.n" "$LOG"

fail() {
    echo "FAIL: $1"
    echo "----- serial (filtered) -----"
    grep -aE "virtio-blk|BARRIERS|fs\]|panic" "$LOG" | tail -20
    exit 1
}

# The device has a write cache -- otherwise the rest of this test is vacuous, and
# a run where it silently became "cache=none" would pass while proving nothing.
grep -aq "cache=writeback (barriers REQUIRED)" "$LOG" \
    || fail "the device did not report a writeback cache -- this test cannot show anything"

grep -aq "BARRIERS-OK" "$LOG" || {
    grep -aq "BARRIERS-FAIL" "$LOG" \
        && fail "$(grep -a 'BARRIERS-FAIL' "$LOG" | tail -1)"
    fail "the barrier probe produced no verdict"
}

echo "PASS: device reports a writeback cache, and a file write issues barriers"
echo "      $(grep -a '^BARRIERS ' "$LOG" | tail -1)"
