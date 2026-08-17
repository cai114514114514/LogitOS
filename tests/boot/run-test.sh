#!/usr/bin/env bash
# Headless boot smoke test: boot the ISO in QEMU with no display, capture the
# serial output to a file, and assert the kernel printed its boot marker.
# Portable (no `timeout`/`gtimeout` dependency).

set -u

ISO="${1:?usage: run-test.sh <iso> [disk.img]}"
DISK="${2:-}"
MARKER="LOGIT_BOOT_OK"
LOG="$(mktemp)"
QEMU="${QEMU:-qemu-system-x86_64}"

# What this machine must say it trusts, EXACTLY. kernel_main calls
# trust_banner() beside net_init (c/kernel/core/kmain.c), so every boot -- not
# only one that opens a TLS connection -- enumerates the compiled-in CA set on
# serial. These two numbers are literals on purpose: matching only the SHAPE of
# the line would pass just as happily on a store that had silently shrunk to
# three roots, which is the failure this assertion exists for. Changing
# tools/roots/ is supposed to make this test red until somebody updates the
# number here and says why in the commit.
EXPECT_ROOTS="${EXPECT_ROOTS:-130}"
EXPECT_SKIPPED="${EXPECT_SKIPPED:-0}"
# One root by name, to prove the NAMES reached the wire and not merely the
# counts: logit_root_names[] could be compiled in, correct, and never printed,
# and the count line would read identically. isrg_x1 is Let's Encrypt's ISRG
# Root X1 -- first row of the bundle, and the anchor most of the web chains to.
EXPECT_ROOT_NAME="${EXPECT_ROOT_NAME:-isrg_x1}"

# BLK selects the disk controller the image is attached to: virtio (default) or
# nvme (exercises the from-scratch NVMe driver -- logitfs must mount + read off it).
BLK="${BLK:-virtio}"
case "$BLK" in
    nvme)   BLK_DEV="-device nvme,drive=hd0,serial=logit0" ;;
    virtio) BLK_DEV="-device virtio-blk-pci,drive=hd0" ;;
    *)      echo "unknown BLK='$BLK' (want virtio|nvme)" >&2; exit 2 ;;
esac

DISK_ARGS=""
[ -n "$DISK" ] && DISK_ARGS="-drive file=$DISK,format=raw,if=none,id=hd0 $BLK_DEV -boot d"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

# An e1000 on SLIRP user networking lets the boot-time net self-test run
# (prints LOGIT_NET_OK); the pass marker stays LOGIT_BOOT_OK so the test still
# passes on hosts where outbound networking is unavailable.
NET_ARGS="-netdev user,id=n0 -device e1000,netdev=n0"
GPU_ARGS="-vga none -device virtio-gpu-pci"
# M25: boot under -smp 4 (multi-core TCG) so this smoke test exercises the SMP
# scheduler + BKL, matching the rest of the suite.
SMP_ARGS="-smp 4 -accel tcg,thread=multi"
"$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" $DISK_ARGS $NET_ARGS $GPU_ARGS $SMP_ARGS -m 512M -serial "file:$LOG" -display none -no-reboot &
QPID=$!

# Poll the serial log for up to ~15s.
booted=0
for _ in $(seq 1 150); do
    if grep -q "$MARKER" "$LOG" 2>/dev/null; then
        booted=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        break   # QEMU exited early
    fi
    sleep 0.1
done

if [ "$booted" -ne 1 ]; then
    echo "FAIL: boot marker '$MARKER' not seen within timeout"
    echo "----- serial output -----"
    cat "$LOG"
    echo "-------------------------"
    exit 1
fi
echo "PASS: kernel reached 64-bit C (found '$MARKER' on serial)"

# The trust store, asserted against the same log. Safe to read now: kmain.c
# prints it at net_init, which is above the line that prints $MARKER, so if the
# marker is in the log the banner either is too or is genuinely missing.
TRUST_LINE="trust store: $EXPECT_ROOTS roots, $EXPECT_SKIPPED skipped"
fail=0
if grep -qF "$TRUST_LINE" "$LOG" 2>/dev/null; then
    echo "PASS: trust store enumerated at boot ($TRUST_LINE)"
else
    echo "FAIL: expected '$TRUST_LINE' on serial; got:"
    grep -F "trust store:" "$LOG" 2>/dev/null || echo "  (no trust-store line at all)"
    fail=1
fi
if grep -qF "$EXPECT_ROOT_NAME" "$LOG" 2>/dev/null; then
    echo "PASS: roots named on serial (found '$EXPECT_ROOT_NAME')"
else
    echo "FAIL: trust store printed no root named '$EXPECT_ROOT_NAME' --"
    echo "      logit_root_names[] is compiled in but not reaching the wire"
    fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "----- serial output -----"
    cat "$LOG"
    echo "-------------------------"
    exit 1
fi
exit 0
