#!/usr/bin/env bash
# Step-0 measurement harness for the storage-kernel wave: boots the machine on
# a PERSISTENT copy of the disk (no -snapshot -- these writes are supposed to
# land) and runs fsroot/as/examples/storprobe.as, printing every STORPROBE
# line. Asserts nothing: this is the BEFORE picture. The gate that asserts is
# tests/boot/run-storage-test.sh, which lands with the fix.
set -u
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-storage-probe.sh <iso> <disk.img>}"
DISK="${2:?usage: run-storage-probe.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

{ logit_wait_for_shell "$WORK/boot.log" 120
  printf '%s' "mkdir /sp
as /usr/as/examples/storprobe.as
echo PROBE-FINISHED
"
  sleep 20
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
    -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial stdio -display none -no-reboot >"$WORK/boot.log" 2>/dev/null &
QPID=$!
waited=0
while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 180 ]; do sleep 1; waited=$((waited + 1)); done
kill -9 "$QPID" 2>/dev/null
tr -d '\r' <"$WORK/boot.log" >"$WORK/boot.n" && mv "$WORK/boot.n" "$WORK/boot.log"
cp "$WORK/boot.log" /tmp/storage-probe-boot.log

grep -aE "STORPROBE|STORCHILD|PROBE-FINISHED|panic|fault" "$WORK/boot.log" || {
  echo "no STORPROBE output; tail of log:"; tail -20 "$WORK/boot.log"; exit 1; }
grep -aq PROBE-FINISHED "$WORK/boot.log" || { echo "probe never finished"; exit 1; }
