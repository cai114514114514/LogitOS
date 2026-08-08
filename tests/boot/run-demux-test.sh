#!/usr/bin/env bash
# Headless end-to-end container test: boot LogitOS, run /bin/demuxcheck on the
# container fixtures packed into the disk image, and require every line it
# prints to be IDENTICAL to what the host build of the same program printed.
#
# This is the only test that says the demuxers work ON LogitOS. Everything else
# in the suite is a glibc build on Linux. The target build is clang
# -ffreestanding against mini-libc, with a different malloc (a fixed arena, and
# a realloc that cannot always grow in place -- which is exactly what a sample
# index does thousands of times), SSE enabled by boot code rather than by the
# ABI, and disk I/O through virtio-blk and LogitFS. Any of those can break a
# parser that is correct on the host.
#
# It compares the WHOLE digest, not a summary: per-track sample-boundary and
# timestamp CRCs, the codec-configuration CRC, the interleave order, and -- for
# the files inside the decoders' proven envelope -- the CRC32 of the decoded
# pictures and of the decoded audio. A single wrong sample boundary anywhere
# changes one of those hex numbers.
#
# Portable: no `timeout` dependency, polls the log like the other boot tests.

set -u

ISO="${1:?usage: run-demux-test.sh <iso> <disk.img> <expected.txt>}"
DISK="${2:?usage: run-demux-test.sh <iso> <disk.img> <expected.txt>}"
WANT="${3:?usage: run-demux-test.sh <iso> <disk.img> <expected.txt>}"
QEMU="${QEMU:-qemu-system-x86_64}"

[ -f "$WANT" ] || { echo "FAIL: missing $WANT (run 'make test-demux-expect')"; exit 1; }

LOG="$(mktemp)"
GOT="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$GOT"
}
trap cleanup EXIT

# The files, and whether to decode them on the device. Decoding under TCG is
# the slow part, so only the two that are inside the decoders' proven envelope
# are decoded; the rest are demuxed, which is what this test is about.
CMDS='
/bin/demuxcheck -decode /media/clip.mp4
/bin/demuxcheck -decode /media/clip.mkv
/bin/demuxcheck /media/clip-frag.mp4
/bin/demuxcheck /media/clip-bframes.mp4
/bin/demuxcheck /media/clip-laced.mkv
/bin/demuxcheck /media/clip.webm
'

{ sleep 4; printf '%s\n' "$CMDS"; printf 'echo DEMUXCHECK_ALL_DONE\nexit\n'; sleep 90; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 1400); do
    grep -aq "DEMUXCHECK_ALL_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

# Keep only the lines the program emits, in order. Everything else on the
# serial console (the shell prompt, kernel chatter) is noise here.
grep -aE '^(MEDIA |MEDIA-|TRACK |ORDER |DEMUX-CRC)' "$LOG" | tr -d '\r' > "$GOT"

if [ ! -s "$GOT" ]; then
    echo "FAIL: /bin/demuxcheck printed nothing"
    echo "----- serial output (tail) -----"
    tail -40 "$LOG"
    echo "--------------------------------"
    exit 1
fi

if ! diff -u "$WANT" "$GOT" > /tmp/demux_boot_diff.$$ 2>&1; then
    echo "FAIL: the on-device digest differs from the host's"
    head -40 /tmp/demux_boot_diff.$$
    rm -f /tmp/demux_boot_diff.$$
    echo "  The demuxers are byte-identical to ffmpeg on the host, so a"
    echo "  mismatch here is the target build: mini-libc, the arena"
    echo "  allocator's realloc, or SSE state."
    exit 1
fi
rm -f /tmp/demux_boot_diff.$$

echo "PASS: LogitOS demuxed $(grep -c '^MEDIA container' "$GOT") containers and"
echo "      produced a digest identical to the host's ($(wc -l < "$GOT") lines)"
exit 0
