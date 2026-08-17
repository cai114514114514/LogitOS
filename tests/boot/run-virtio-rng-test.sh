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
# printed once from probe(): two INDEPENDENT 16-byte reads, hex-dumped to
# serial. This script asserts on the four properties that distinguish a
# working device from a stub that "succeeds" without doing the work:
#   1. the self-test ran at all (driver bound, queue came up, DRIVER_OK sent)
#   2. neither read came back all-zero
#   3. the two reads differ (same 32 bytes twice is not entropy)
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

fail=0
say() { echo "$1"; }
need() { if [ "$2" = 0 ]; then say "ok   $1"; else say "FAIL $1"; fail=1; fi; }

if ! grep -aq "LOGIT_BOOT_OK" "$LOG" 2>/dev/null; then
    echo "FAIL: kernel did not reach LOGIT_BOOT_OK"
    fail=1
fi

LINE="$(grep -a '^VIRTIO_RNG_SELFTEST' "$LOG" | sed -n 1p | tr -d '\r')"

if [ "$ENABLE" != 1 ]; then
    # Negative control: the positive assertion must be UNSATISFIABLE here.
    if [ -z "$LINE" ]; then
        echo "PASS (control): no virtio-rng device -> no self-test line, as required."
        exit 0
    fi
    echo "CONTROL FAILED: self-test line present with no virtio-rng-pci device:"
    echo "  $LINE"
    exit 1
fi

if [ -z "$LINE" ]; then
    echo "FAIL: no VIRTIO_RNG_SELFTEST line -- driver did not bind or did not run"
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
fi
echo "self-test: $LINE"

A=$(echo "$LINE" | sed -n 's/.* a=\([0-9a-f]*\) .*/\1/p')
B=$(echo "$LINE" | sed -n 's/.* b=\([0-9a-f]*\) .*/\1/p')
ZA=$(echo "$LINE" | sed -n 's/.*zero_a=\([0-9]\) .*/\1/p')
ZB=$(echo "$LINE" | sed -n 's/.*zero_b=\([0-9]\) .*/\1/p')
SAME=$(echo "$LINE" | sed -n 's/.*same=\([0-9]\).*/\1/p')

need "self-test line parsed (32 hex chars each read)" "$([ ${#A} -eq 32 ] && [ ${#B} -eq 32 ] && echo 0 || echo 1)"
need "read A is not all-zero"                          "$([ "$ZA" = 0 ] && echo 0 || echo 1)"
need "read B is not all-zero"                           "$([ "$ZB" = 0 ] && echo 0 || echo 1)"
need "read A and read B differ"                         "$([ "$SAME" = 0 ] && echo 0 || echo 1)"

if [ "$fail" = 0 ]; then
    echo
    echo "PASS: virtio-rng delivers real entropy on a CPU with no RDSEED/RDRAND"
    exit 0
fi
echo
echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
exit 1
