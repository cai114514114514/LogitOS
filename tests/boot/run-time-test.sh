#!/usr/bin/env bash
# On-device gate for the time subsystem (c/kernel/core/ktime.c).
#
# Everything asserted here is printed by the kernel on EVERY boot, not only
# under this harness. That is the design: the bug this subsystem was written
# after -- a PIT tick running at exactly twice its programmed rate, for the life
# of the kernel -- survived because nothing ever compared the clock to anything.
# A check that runs only when someone remembers to run it will be wrong on the
# machine that matters.
#
# The five claims:
#   1. a clocksource was CHOSEN and CALIBRATED, and the boot says which and how
#   2. the calibrated clock agrees with an INDEPENDENT oscillator (the CMOS RTC)
#      and with a second one (the PIT interrupt count)
#   3. the guard that makes claim 2 meaningful actually rejects a 2x clock
#      (the negative control, evaluated on device against the same code)
#   4. the PIT fallback RUNS -- the live clock is switched onto it and back, and
#      stays monotonic across both switches
#   5. timers fire, and the actual-vs-requested distribution is reported
#
# Portable: no `timeout` dependency, polls the log.

set -u

ISO="${1:?usage: run-time-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-time-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
SMP="${SMP:-4}"

LOG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

# -rtc base=localtime: the guest RTC follows the host, which is the whole point
# -- the cross-check needs a genuinely independent time source, not a second
# view of the same counter.
{ sleep 30; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp "$SMP" -accel tcg,thread=multi -rtc base=localtime \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    >"$LOG" 2>/dev/null &
QPID=$!

# The last self-test to print is the SMP probe at ~11 s of guest time.
for _ in $(seq 1 600); do
    grep -aq "\[time\] smp-mono" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

dump() { echo "----- serial (time lines) -----"; grep -a "\[time\]" "$LOG"; echo "-------------------------------"; }
fail() { echo "FAIL: $1"; dump; exit 1; }

# No '^' anchors: the serial console is SHARED with /bin/sh, so a kernel line can
# arrive with a "/ $ " prompt already on it. Anchoring cost one debugging round.
SRC_LINE="$(grep -a '\[time\] source=' "$LOG" | tail -1 | tr -d '\r')"
XC_LINE="$(grep  -a '\[time\] xcheck '  "$LOG" | tail -1 | tr -d '\r')"
NEG_LINE="$(grep -a '\[time\] negctl '  "$LOG" | tail -1 | tr -d '\r')"
FB_LINE="$(grep  -a '\[time\] fallback pit ran' "$LOG" | tail -1 | tr -d '\r')"
ACC_LINE="$(grep -a '\[time\] accuracy ' "$LOG" | tail -1 | tr -d '\r')"

grep -aq "LOGIT_BOOT_OK" "$LOG" || fail "the kernel did not reach LOGIT_BOOT_OK"

# --- 1. a source was chosen and calibrated -------------------------------
[ -n "$SRC_LINE" ] || fail "the kernel printed no clocksource line at all -- on unfamiliar hardware that line IS the diagnosis"
case "$SRC_LINE" in
    *"source=tsc"*) SRC=tsc ;;
    *"source=pit"*) SRC=pit ;;
    *) fail "unrecognised clocksource line: $SRC_LINE" ;;
esac
echo "$SRC_LINE" | grep -q "MHz" || fail "the clocksource line does not report a calibrated frequency"

# --- 2. the cross-check against the RTC ----------------------------------
[ -n "$XC_LINE" ] || fail "no calibration cross-check ran (the RTC edge sampler never completed)"
case "$XC_LINE" in
    *" OK"*)   ;;
    *" FAIL"*) fail "the calibrated clock disagrees with the RTC beyond tolerance: $XC_LINE" ;;
    *)         fail "unparsable cross-check line: $XC_LINE" ;;
esac
# The line carries three independent readings; require the PIT interrupt count
# to agree with the calibrated clock too. A tick at 2x shows up HERE as
# tick != mono, which is a sharper signal than either against the RTC.
MONO_MS="$(echo "$XC_LINE" | sed -n 's/.*mono=\([0-9]*\)ms.*/\1/p')"
TICK_MS="$(echo "$XC_LINE" | sed -n 's/.*tick=\([0-9]*\)ms.*/\1/p')"
[ -n "$MONO_MS" ] && [ -n "$TICK_MS" ] || fail "cross-check line has no mono/tick figures: $XC_LINE"
[ "$MONO_MS" -gt 0 ] 2>/dev/null || fail "the monotonic clock did not advance during the cross-check"
DIFF=$(( MONO_MS > TICK_MS ? MONO_MS - TICK_MS : TICK_MS - MONO_MS ))
LIM=$(( MONO_MS / 10 ))                    # 10%: catches 2x with five times to spare
[ "$DIFF" -le "$LIM" ] || fail "the calibrated clock and the PIT interrupt count disagree by ${DIFF}ms of ${MONO_MS}ms (limit ${LIM}ms) -- one of them is wrong about how long a second is"

# --- 3. the negative control ---------------------------------------------
[ -n "$NEG_LINE" ] || fail "the negative control did not run"
case "$NEG_LINE" in
    *"REJECTED"*) ;;
    *) fail "the guard ACCEPTED a clock running at twice its rate -- exactly the bug it exists to catch: $NEG_LINE" ;;
esac

# --- 4. the fallback actually ran ----------------------------------------
[ -n "$FB_LINE" ] || fail "the PIT fallback was never exercised (a fallback that has never run is not a fallback)"
case "$FB_LINE" in
    *"monotonic"*) ;;
    *) fail "the clock went backwards across a clocksource switch: $FB_LINE" ;;
esac
FB_MS="$(echo "$FB_LINE" | sed -n 's/.*pit ran \([0-9]*\)ms.*/\1/p')"
[ -n "$FB_MS" ] && [ "$FB_MS" -ge 400 ] && [ "$FB_MS" -le 700 ] 2>/dev/null \
    || fail "on the PIT fallback a 500ms interval measured ${FB_MS:-?}ms"

# --- 5. timers fired, with a reported distribution -----------------------
[ -n "$ACC_LINE" ] || fail "no timer-accuracy distribution was reported (did any timer fire?)"
echo "$ACC_LINE" | grep -q "p50=" || fail "the accuracy line reports no distribution: $ACC_LINE"

echo "PASS: time subsystem"
echo "  $SRC_LINE"
echo "  $XC_LINE"
echo "  $NEG_LINE"
echo "  $FB_LINE"
echo "  $ACC_LINE"
exit 0
