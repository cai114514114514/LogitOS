#!/usr/bin/env bash
# Boot LogitOS, run /bin/libmcheck over the serial shell, and require its output
# to be byte-identical to a native build of the same third_party/libm sources.
#
# The comparison is on RAW IEEE BITS, not formatted floats: mini-libc has its
# own dtoa and the host has glibc's, and putting two float formatters between
# the thing under test and the diff would make a printf difference read as a
# libm failure. See the header of tests/unit/libmcheck.c.
set -u
ISO="${1:?usage: run-libm-test.sh <iso> <disk.img> <reference.out>}"
DISK="${2:?usage: run-libm-test.sh <iso> <disk.img> <reference.out>}"
REF="${3:?usage: run-libm-test.sh <iso> <disk.img> <reference.out>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
GOT="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG" "$GOT"; }
trap cleanup EXIT

# PINNED TO -smp 1, AND THAT IS A KNOWN KERNEL BUG, NOT A CONVENIENCE.
# Under `-smp 4 -accel tcg,thread=multi` this program wedges the whole machine
# after a few hundred lines -- no further serial output at all, and the shell
# stops answering. `/bin/libmcheck noop` prints the same 1214 lines through the
# same printf and the same serial path WITHOUT calling libm and completes
# every time, so it is the floating point, not the output. Recorded as P2 in
# docs/BUG_BACKLOG.md with what has already been ruled out.
#
# Pinning is right rather than evasive: what this gate proves is that the
# cross-built libm is bit-identical to a native build of the same sources. That
# the kernel saves FP state correctly across cores is a different claim and
# needs its own gate; running them together would mean neither one fails for a
# reason you can name.
#
# No `exit` and no fixed wait: /bin/libmcheck prints ~1200 lines and the
# serial console is the bottleneck (about 17 lines a second under TCG), so a
# sleep long enough to be safe would make every run take that long even when
# it finishes early. The poll loop below watches for LIBM_DONE and the EXIT
# trap kills QEMU, so the gate costs exactly as long as the program does.
{ sleep 5; printf '/bin/libmcheck\n'; sleep 600; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 1 -accel tcg -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "LIBM_DONE" "$LOG" && break
    sleep 1
done

if ! grep -aq "LIBM_DONE" "$LOG"; then
    echo "FAIL: LIBM_DONE marker not seen (boot/run problem)"
    grep -aE "FAIL|fault|libmcheck" "$LOG" | tail -20
    exit 1
fi

# The serial log carries the boot chatter and the shell prompt too; the payload
# is exactly the lines the program prints, and they are all prefixed.
grep -a '^LIBM' "$LOG" | tr -d '\r' > "$GOT"

WANT=$(grep -c '^LIBM' "$REF")
HAVE=$(grep -c '^LIBM' "$GOT")
if [ "$WANT" != "$HAVE" ]; then
    echo "FAIL: line count differs -- host $WANT, target $HAVE"
    diff -u "$REF" "$GOT" | head -20
    exit 1
fi

if diff -u "$REF" "$GOT" > /dev/null; then
    echo "PASS: /bin/libmcheck is bit-identical to a native build of the same musl sources ($HAVE lines)"
    exit 0
fi

echo "FAIL: the cross-built libm differs from a native build of the same sources"
echo "      (left = host reference, right = LogitOS)"
diff -u "$REF" "$GOT" | head -40
exit 1
