#!/usr/bin/env bash
# Headless end-to-end test for SYS_MONOTONIC_MS: boot LogitOS, run the
# AetherScript example that reads the new clock and the CMOS wall clock across
# the same interval, and compare the two numbers it prints.
#
# The claim being tested is not "the syscall returns something". It is:
#   1. the counter ADVANCES        (t1 > t0 -- a clock, not a constant)
#   2. its UNIT is milliseconds    (delta ~= wall_seconds * 1000, cross-checked
#                                   against a different physical timer, so a
#                                   tick/ms mix-up shows up as a 100x error)
#   3. its STEP is 10 ms           (both readings divisible by 10 -- the
#                                   granularity the ABI documents, asserted
#                                   rather than merely commented)
#
# Same shape as run-video-test.sh: boot, run one program, compare a printed
# value. Portable -- no `timeout` dependency, polls the log.
# Correction to "100x" above: a 100 Hz tick represents 10 ms; the native
# control returns ticks and must therefore fail at one tenth of the real rate.

set -u

ISO="${1:?usage: run-clock-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-clock-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

LOG="$(mktemp)"
READINGS="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$READINGS"
}
trap cleanup EXIT

# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
# -rtc base=localtime: the guest RTC follows the host clock, which is the whole
# point -- the wall clock has to be an independent time source, not another view
# of the same tick counter.
# Keep the standalone prebuilt-disk harness usable as well as make test-clock.
# The shared source-version router selects the packaged native artifact and
# never falls back to the retired VM when that artifact is missing.
CLOCK_COMMAND="$(python3 tools/as_examples.py commands fsroot/as/examples/monotonic.as)" || exit 1
{ sleep 4; printf '%s\nexit\n' "$CLOCK_COMMAND"; sleep 25; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi -rtc base=localtime \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 600); do
    grep -aq "MONO-STEP" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

# Extract only the program records from the interleaved serial console. The
# shared oracle checks their complete structure, arithmetic, rate and step.
# Keeping a second copy of those bounds here previously let harnesses drift.
grep -aE '^MONO(-STEP)? ' "$LOG" | tr -d '\r' > "$READINGS"
if ! python3 tests/unit/as_clock_test.py "$READINGS"; then
    tail -40 "$LOG"
    exit 1
fi
