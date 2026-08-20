#!/usr/bin/env bash
# THE DEVICE NEGATIVE CONTROL for poll().
#
# /bin/polltest-nopoll is the same program with every poll() replaced by
# mini-libc's PRE-SYS_POLL implementation, quoted from the header that file used
# to carry: a regular file is ready, everything else is never ready, and a
# timeout is a sleep followed by "nothing became ready that we could detect".
#
# WHY THIS IS THE RIGHT CONTROL. The old behaviour was not absurd -- it was a
# careful answer from a kernel that could not answer, and it is EXACTLY what a
# poll() that compiles, links, returns sensible values and never crashes looks
# like when it is measuring nothing. A gate that cannot tell it from the real
# thing is not a gate on poll(); it is a gate on "the program ran". So this
# build MUST fail, and it must fail on the readiness checks specifically.
#
# THE CHECKS THAT MUST SURVIVE. Two of polltest's assertions do NOT depend on
# readiness -- "an empty-pipe probe returns 0" and "a poll that times out
# returns 0" are both satisfied by reporting nothing ready -- and they must keep
# passing. If they reddened too, the control would be proving that -D broke the
# program rather than that the readiness answers are what the gate measures.

set -u

ISO="${1:?usage: run-poll-negctl.sh <iso> <disk.img>}"
DISK="${2:?usage: run-poll-negctl.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 11; printf '/bin/polltest-nopoll\n'; sleep 90; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 2 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 2000); do
    grep -aq "polltest: " "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

if ! grep -aq "polltest: " "$LOG"; then
    echo "FAIL: the control never reported -- it must FAIL, not disappear"
    tail -80 "$LOG"
    exit 1
fi

FAILED=$(grep -ac "^POLL_FAIL" "$LOG" || true)
PASSED=$(sed -n 's/^polltest: \([0-9]*\) passed.*/\1/p' "$LOG" | head -1)

echo "--- device negative control: poll() replaced by the pre-SYS_POLL stub ---"
grep -a -E "^POLL_FAIL|^polltest: " "$LOG"

if grep -aq "POLL_TEST_OK" "$LOG"; then
    echo "NEGCTL FAILED TO FAIL: the stub satisfied every readiness check"
    exit 1
fi
if [ "${FAILED:-0}" -lt 5 ]; then
    echo "NEGCTL TOO WEAK: only ${FAILED:-0} checks reddened; the readiness"
    echo "assertions are not the thing this gate is measuring"
    exit 1
fi
if [ "${PASSED:-0}" -lt 2 ]; then
    echo "NEGCTL TOO BROAD: only ${PASSED:-0} checks still pass. A control that"
    echo "breaks everything proves the -D did something, not what it did --"
    echo "the probe and the timeout must still be satisfied by 'nothing ready'."
    exit 1
fi
echo "control reddened as required: $FAILED failed, $PASSED still passing"
exit 0
