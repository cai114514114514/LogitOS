#!/usr/bin/env bash
# Headless boot smoke test: boot the ISO in QEMU with no display, capture the
# serial output to a file, and assert the kernel printed its boot marker.
# Portable (no `timeout`/`gtimeout` dependency).

set -u

ISO="${1:?usage: run-test.sh <iso> [disk.img]}"
DISK="${2:-}"
MARKER="AQUA_BOOT_OK"
LOG="$(mktemp)"
QEMU="${QEMU:-qemu-system-x86_64}"

DISK_ARGS=""
[ -n "$DISK" ] && DISK_ARGS="-drive file=$DISK,format=raw,if=ide,index=0,media=disk -boot d"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

# An e1000 on SLIRP user networking lets the boot-time net self-test run
# (prints AQUA_NET_OK); the pass marker stays AQUA_BOOT_OK so the test still
# passes on hosts where outbound networking is unavailable.
NET_ARGS="-netdev user,id=n0 -device e1000,netdev=n0"
"$QEMU" -cdrom "$ISO" $DISK_ARGS $NET_ARGS -m 512M -serial "file:$LOG" -display none -no-reboot &
QPID=$!

# Poll the serial log for up to ~15s.
for _ in $(seq 1 150); do
    if grep -q "$MARKER" "$LOG" 2>/dev/null; then
        echo "PASS: kernel reached 64-bit C (found '$MARKER' on serial)"
        exit 0
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        break   # QEMU exited early
    fi
    sleep 0.1
done

echo "FAIL: boot marker '$MARKER' not seen within timeout"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
