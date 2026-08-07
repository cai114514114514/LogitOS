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

set -u

ISO="${1:?usage: run-clock-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-clock-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

LOG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
# -rtc base=localtime: the guest RTC follows the host clock, which is the whole
# point -- the wall clock has to be an independent time source, not another view
# of the same tick counter.
{ sleep 4; printf 'as /usr/as/examples/monotonic.as\nexit\n'; sleep 25; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi -rtc base=localtime \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 600); do
    grep -aq "MONO-STEP" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

LINE="$(grep -a '^MONO ' "$LOG" | tail -1 | tr -d '\r')"
STEP="$(grep -a '^MONO-STEP ' "$LOG" | tail -1 | tr -d '\r')"
if [ -z "$LINE" ] || [ -z "$STEP" ]; then
    echo "FAIL: monotonic.as printed no reading"
    echo "----- serial output (tail) -----"
    tail -40 "$LOG"
    echo "--------------------------------"
    exit 1
fi

T0="$(echo "$LINE"  | awk '{print $2}')"
T1="$(echo "$LINE"  | awk '{print $3}')"
DELTA="$(echo "$LINE" | awk '{print $4}')"
WALL="$(echo "$LINE"  | awk '{print $5}')"
SPINS="$(echo "$LINE" | awk '{print $6}')"
S0="$(echo "$STEP"  | awk '{print $2}')"
S1="$(echo "$STEP"  | awk '{print $3}')"

fail() { echo "FAIL: $1"; echo "  ($LINE / $STEP)"; exit 1; }

# A zero spin count means the wait loop exited on its first look at the wall
# clock, so nothing was measured at all -- a different bug from a stopped
# counter, and worth saying so rather than reporting "the clock did not advance".
[ "${SPINS:-0}" -gt 100 ] 2>/dev/null || fail "the wait loop ran only ${SPINS:-?} times: the wall clock jumped, so no interval was measured"
[ "$T1" -gt "$T0" ] 2>/dev/null || fail "the clock did not advance (t0=$T0 t1=$T1)"
[ "$WALL" -ge 4 ] 2>/dev/null   || fail "the wall clock did not advance 4s; the cross-check is meaningless"

# The RTC reports whole seconds, so WALL of them means the real elapsed time is
# in (WALL-1, WALL+1); the extra 25% of slack is for TCG, where a loaded guest
# can miss PIT interrupts and read SLOW -- an honest limit of an emulated tick,
# not a unit error.
#
# These bounds are tight enough to catch a factor of two, which is not
# hypothetical: the PIT was programmed in mode 3 (square wave) and the tick ran
# at exactly 2x the requested rate for the life of the kernel, invisible because
# every consumer was a timeout whose only specification was a comment. See
# c/drivers/timer/pit.c.
LO=$(( (WALL - 1) * 1000 * 3 / 4 ))
HI=$(( (WALL + 1) * 1000 * 5 / 4 ))
[ "$DELTA" -ge "$LO" ] 2>/dev/null || fail "delta ${DELTA}ms over ${WALL}s wall -- clock too slow (want >= ${LO})"
[ "$DELTA" -le "$HI" ] 2>/dev/null || fail "delta ${DELTA}ms over ${WALL}s wall -- clock too fast (want <= ${HI})"

[ "$S0" = "0" ] && [ "$S1" = "0" ] || fail "readings are not 10 ms aligned (t0%10=$S0 t1%10=$S1); the ABI documents a 10 ms step"

echo "PASS: SYS_MONOTONIC_MS advanced ${DELTA}ms across ${WALL}s of wall clock (want ${LO}..${HI}), 10 ms aligned"
exit 0
