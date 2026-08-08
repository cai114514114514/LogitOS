#!/usr/bin/env bash
# Headless end-to-end audio test: boot LogitOS, run /bin/audiocheck on each of
# the three sample files packed into the disk image, and require every CRC32 it
# prints to equal the one the HOST build of the same decoder prints for the
# same bytes.
#
# This is the only test that says the audio decoders work ON LogitOS. The host
# build is glibc on x86-64 Linux; the target build is clang -ffreestanding
# against mini-libc, with a different malloc, a 24 MiB arena instead of an OS
# heap, SSE enabled by boot code rather than by the ABI, and file I/O through
# virtio-blk and LogitFS. MP3 in particular is a floating-point decoder whose
# every sample passes through an IMDCT and a 512-tap filter bank; if the guest
# disagreed with the host about one rounding anywhere in a quarter of a million
# samples, the CRC would differ.
#
# The reference is computed here rather than pinned in a file on purpose: a
# pinned constant only proves the guest matches a number somebody wrote down,
# while recomputing it with the host binary proves the two builds of the same
# source agree today.
#
# FLAC gets a second assertion that needs no reference at all: the file's own
# STREAMINFO MD5, checked inside the guest, which is the format's definition of
# a correct lossless decode.
#
# Portable: no `timeout` dependency, polls the log like the other boot tests.

set -u

ISO="${1:?usage: run-audio-test.sh <iso> <disk.img> <host-audiocheck>}"
DISK="${2:?usage: run-audio-test.sh <iso> <disk.img> <host-audiocheck>}"
HOSTBIN="${3:?usage: run-audio-test.sh <iso> <disk.img> <host-audiocheck>}"
QEMU="${QEMU:-qemu-system-x86_64}"

[ -x "$HOSTBIN" ] || { echo "FAIL: missing host reference binary $HOSTBIN"; exit 1; }

# host path  ->  guest path. The three original fixtures live under /media/;
# sample.aac is packed from fsroot/ instead, and lands at the root, because the
# disk image file list lives in the shared Makefile and that file had another
# session's uncommitted work in it. fsroot/* is picked up by the existing
# $(FS_FILES) wildcard, so a new fixture needs no shared edit at all.
FILES="sample.wav sample.flac sample.mp3 sample.aac sample.ogg"

guest_path() {
    case "$1" in
        sample.aac|sample.ogg) echo "/$1" ;;
        *)          echo "/media/$1" ;;
    esac
}

host_path() {
    case "$1" in
        sample.aac|sample.ogg) echo "fsroot/$1" ;;
        *)          echo "tests/fixtures/audio/$1" ;;
    esac
}

# Host reference values first: if these cannot be produced there is no test.
declare -A WANT
for f in $FILES; do
    line="$("$HOSTBIN" "$(host_path "$f")" | grep '^AUDIO-CRC')"
    crc="$(echo "$line" | awk '{print $2}')"
    if [ -z "$crc" ]; then
        echo "FAIL: host build produced no CRC for $f"
        exit 1
    fi
    WANT[$f]="$crc"
    echo "host   $f -> $crc"
done

LOG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
# Decoding under TCG is not fast; give it room before the shell input ends.
{
    sleep 4
    for f in $FILES; do printf '/bin/audiocheck %s\n' "$(guest_path "$f")"; done
    printf 'echo AUDIOCHECK-DONE\nexit\n'
    sleep 150
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 \
      -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
      -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
      >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 2400); do
    if grep -aq "AUDIOCHECK-DONE" "$LOG" || grep -aq "AUDIO-ERR" "$LOG"; then
        break
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

rc=0
n=0
# The guest prints one AUDIO-CRC line per file, in the order they were fed in.
mapfile -t GOTLINES < <(grep -a "AUDIO-CRC" "$LOG" | tr -d '\r')
for f in $FILES; do
    line="${GOTLINES[$n]:-}"
    got="$(echo "$line" | awk '{print $2}')"
    if [ -z "$got" ]; then
        echo "FAIL: guest printed no CRC for $f"
        rc=1
    elif [ "$got" != "${WANT[$f]}" ]; then
        echo "FAIL: $f on-device CRC $got, host CRC ${WANT[$f]}"
        echo "  ($line)"
        echo "  The decoders agree with ffmpeg on the host, so a mismatch here is"
        echo "  the target build: mini-libc, the arena allocator, or SSE state."
        rc=1
    else
        echo "guest  $f -> $got  (frames/ch/rate: $(echo "$line" | awk '{print $3, $4, $5}'))"
    fi
    n=$((n + 1))
done

# FLAC's own criterion, evaluated inside the guest.
md5line="$(grep -a "FLAC-MD5" "$LOG" | tail -1 | tr -d '\r')"
if [ -z "$md5line" ]; then
    echo "FAIL: guest printed no FLAC-MD5 line"
    rc=1
elif ! echo "$md5line" | grep -q "FLAC-MD5 ok"; then
    echo "FAIL: guest FLAC STREAMINFO MD5 check said: $md5line"
    rc=1
else
    echo "guest  sample.flac STREAMINFO MD5 verified in-guest (lossless, bit-exact)"
fi

if [ "$rc" != 0 ]; then
    echo "----- serial output (tail) -----"
    tail -40 "$LOG"
    echo "--------------------------------"
    exit 1
fi

echo "PASS: LogitOS decoded wav/flac/mp3/aac/vorbis to the host's CRCs"
exit 0
