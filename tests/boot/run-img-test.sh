#!/usr/bin/env bash
# Headless end-to-end image test: boot LogitOS, run /bin/imgcheck on the
# fixtures packed into the disk image, and require every line it prints to
# equal the line the HOST build of the identical source prints for the same
# bytes.
#
# This is the only test that says the image decoders work ON LogitOS. The host
# tests (test-img-still / test-img-anim / test-img-exif) prove byte-exactness
# against PIL, libwebp and ffmpeg -- but they prove it about a glibc build on
# Linux. The target build is clang -ffreestanding -mno-red-zone -msse2 against
# mini-libc, with a 24 MiB arena instead of an OS heap, a 32 KiB stack instead
# of 8 MiB, files read through virtio-blk and LogitFS, and the
# x86_64-unknown-none build of the Rust staticlib rather than the host one.
# BMP, ICO and WebP are new Rust code and GIF/APNG hold several full canvases
# live at once; either could be right in the first environment and wrong here.
#
# The digest covers every frame's pixels plus the geometry, the loop count and
# the summed per-frame delays, so a disposal, timing or orientation difference
# moves it.
#
# The reference is recomputed here rather than pinned in a file: a pinned
# constant only proves the guest matches a number somebody wrote down once,
# while running the host binary proves the two builds of the same source agree
# today.
#
# Portable: no `timeout` dependency, polls the log like the other boot tests.

set -u

ISO="${1:?usage: run-img-test.sh <iso> <disk.img> <host-imgcheck>}"
DISK="${2:?usage: run-img-test.sh <iso> <disk.img> <host-imgcheck>}"
HOSTBIN="${3:?usage: run-img-test.sh <iso> <disk.img> <host-imgcheck>}"
QEMU="${QEMU:-qemu-system-x86_64}"

[ -x "$HOSTBIN" ] || { echo "FAIL: missing host reference binary $HOSTBIN"; exit 1; }

FILES="still.bmp still.webp icon.ico anim.gif anim.apng rot.jpg"

# Host reference first: if these cannot be produced there is no test.
HOSTOUT="$(mktemp)"
GUESTOUT="$(mktemp)"
LOG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$HOSTOUT" "$GUESTOUT"
}
trap cleanup EXIT

hostargs=""
for f in $FILES; do hostargs="$hostargs tests/fixtures/image/$f"; done
# shellcheck disable=SC2086
"$HOSTBIN" $hostargs | grep '^IMG ' > "$HOSTOUT"
if [ "$(wc -l < "$HOSTOUT")" -ne "$(echo $FILES | wc -w)" ]; then
    echo "FAIL: host build did not produce one line per fixture"
    cat "$HOSTOUT"
    exit 1
fi
echo "--- host ---"
cat "$HOSTOUT"

# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
{
    sleep 4
    printf '/bin/imgcheck'
    for f in $FILES; do printf ' /media/img/%s' "$f"; done
    printf '\n'
    printf 'echo IMGCHECK-FINISHED\nexit\n'
    sleep 90
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 \
      -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
      -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
      >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 1400); do
    if grep -aq "IMGCHECK-FINISHED" "$LOG"; then break; fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

grep -a '^IMG ' "$LOG" | tr -d '\r' > "$GUESTOUT"
echo "--- guest ---"
cat "$GUESTOUT"

if ! diff -u "$HOSTOUT" "$GUESTOUT" > /dev/null; then
    echo "FAIL: the guest and the host disagree about the decoded images"
    diff -u "$HOSTOUT" "$GUESTOUT" || true
    echo "  The host tests prove these decoders byte-exact against PIL/libwebp/ffmpeg,"
    echo "  so a difference here is the TARGET build: mini-libc's arena allocator,"
    echo "  the freestanding flags, the 32 KiB stack, or LogitFS I/O."
    echo "----- serial output (tail) -----"
    tail -40 "$LOG"
    echo "--------------------------------"
    exit 1
fi

n=$(wc -l < "$GUESTOUT")
echo "PASS: LogitOS decoded $n images (bmp/webp/ico/gif-anim/apng/jpeg-exif) to the host's digests"
exit 0
