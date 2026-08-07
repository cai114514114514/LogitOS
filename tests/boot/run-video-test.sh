#!/usr/bin/env bash
# Headless end-to-end video test: boot LogitOS, run /bin/vidcheck on the sample
# stream packed into the disk image, and require the CRC32 it prints to equal
# the one pinned in tests/fixtures/video/sample.crc32 -- the same number the
# host gate (make test-h264) checks.
#
# This is the only test that says the decoder works ON LogitOS. The host build
# is glibc on Linux; the target build is clang -ffreestanding against mini-libc,
# with a different malloc, a 24 MiB arena instead of an OS heap, SSE enabled by
# boot code rather than by the ABI, and disk I/O through virtio-blk and LogitFS.
# Any of those can break a decoder that is bit-exact on the host, and a single
# wrong bit anywhere in 60 frames changes the CRC.
#
# Portable: no `timeout` dependency, polls the log like the other boot tests.

set -u

ISO="${1:?usage: run-video-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-video-test.sh <iso> <disk.img>}"
WANT_FILE="${3:-tests/fixtures/video/sample.crc32}"
QEMU="${QEMU:-qemu-system-x86_64}"

[ -f "$WANT_FILE" ] || { echo "FAIL: missing $WANT_FILE"; exit 1; }
WANT="$(tr -d ' \t\r\n' < "$WANT_FILE")"
[ -n "$WANT" ] || { echo "FAIL: $WANT_FILE is empty"; exit 1; }

LOG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
# Decoding 60 frames of 320x240 in a TCG-emulated ring 3 is not fast; give it
# room before the input stream closes the shell.
{ sleep 4; printf '/bin/vidcheck /media/sample.h264\nexit\n'; sleep 40; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    >"$LOG" 2>/dev/null &
QPID=$!

# Poll for up to ~60s: decode is the slow part, not boot.
for _ in $(seq 1 600); do
    if grep -aq "H264-CRC" "$LOG" || grep -aq "H264-ERR" "$LOG"; then
        break
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

LINE="$(grep -a "H264-CRC" "$LOG" | tail -1 | tr -d '\r')"
if [ -z "$LINE" ]; then
    echo "FAIL: /bin/vidcheck printed no CRC"
    echo "----- serial output (tail) -----"
    tail -40 "$LOG"
    echo "--------------------------------"
    exit 1
fi

GOT="$(echo "$LINE" | awk '{print $2}')"
FRAMES="$(echo "$LINE" | awk '{print $3}')"
if [ "$GOT" != "$WANT" ]; then
    echo "FAIL: on-device CRC $GOT, host/pinned CRC $WANT"
    echo "  ($LINE)"
    echo "  The decoder is bit-exact on the host, so a mismatch here is the"
    echo "  target build: mini-libc, the arena allocator, or SSE state."
    exit 1
fi

echo "PASS: LogitOS decoded $FRAMES frames to CRC $GOT (matches the host)"
exit 0
