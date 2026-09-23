#!/usr/bin/env bash
# virtio-rng boot test: does the driver actually pull real entropy off the
# device, on a CPU model that has no RDSEED/RDRAND of its own to fall back to?
#
# `-cpu qemu64` is not an arbitrary choice -- it is the exact configuration
# the Makefile documents as the one where rng.c's own hardware entropy path is
# absent (Makefile:1182-1185, "The default TCG cpu (qemu64) has no RDRAND/
# RDSEED..."). Running with it is what makes this a test of the DEVICE, not of
# a CPU instruction the kernel would have used anyway.
#
# c/drivers/virtio/virtio_rng.c has no syscall path into it yet (see its
# header comment), so the only observable is its own boot self-test line,
# printed once from probe(): two INDEPENDENT 16-byte reads, reported only as
# verdicts. This script asserts on the four properties that distinguish a
# working device from a stub that "succeeds" without doing the work:
#   1. the self-test ran at all (driver bound, queue came up, DRIVER_OK sent)
#   2. neither read came back all-zero
#   3. the two reads differ (the same 16 bytes twice would prove no freshness)
#   4. the machine still reaches LOGIT_BOOT_OK (the driver did not wedge boot)
#
# ENABLE=0 is the negative control: boot the IDENTICAL kernel and command line
# with `-device virtio-rng-pci` removed. The positive assertion (self-test
# line present) must be UNSATISFIABLE there -- run with ENABLE=0 and confirm
# it fails; that is the control, not a separate passing test.
#
#   run-virtio-rng-test.sh <iso> <disk.img>
set -u

ISO="${1:?usage: run-virtio-rng-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-virtio-rng-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
ENABLE="${ENABLE:-1}"
LOG="$(mktemp)"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
RNGDEV=""
[ "$ENABLE" = 1 ] && RNGDEV="-device virtio-rng-pci"

# cpu=qemu64 on purpose -- see the header. Everything else matches the other
# boot harnesses' standard machine (virtio-blk root disk, virtio-gpu, -snapshot).
# shellcheck disable=SC2086
"$QEMU" -cpu qemu64 -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $RNGDEV $NET -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "LOGIT_BOOT_OK" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
# Give a slow self-test line (or its absence) a moment to land after boot-ok.
sleep 1

if ! grep -aq "LOGIT_BOOT_OK" "$LOG" 2>/dev/null; then
    echo "FAIL: kernel did not reach LOGIT_BOOT_OK"
    exit 1
fi

LINE="$(rg -a '^VIRTIO_RNG_SELFTEST' "$LOG" | head -n 1 | tr -d '\r')"

if [ "$ENABLE" != 1 ]; then
    # Negative control: the positive assertion must be UNSATISFIABLE here.
    if [ -z "$LINE" ]; then
        echo "PASS (control): no virtio-rng device -> no self-test line, as required."
        exit 0
    fi
    echo "CONTROL FAILED: self-test line present with no virtio-rng-pci device"
    exit 1
fi

if [ -z "$LINE" ]; then
    echo "FAIL: no VIRTIO_RNG_SELFTEST line -- driver did not bind or did not run"
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
fi
if python3 tests/boot/check-virtio-rng-line.py "$LINE"; then
    echo "self-test: $LINE"
    echo "PASS: virtio-rng delivers device bytes on a CPU with no RDSEED/RDRAND"
    exit 0
fi
# A failed format check may mean the kernel regressed to logging raw entropy.
# Do not copy that serial line into the test report as well.
echo "FAIL: invalid RNG self-test verdict; serial bytes withheld"
exit 1
