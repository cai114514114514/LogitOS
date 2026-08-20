#!/usr/bin/env bash
# poll() on the machine. Boot, drive the serial shell to run /bin/polltest, and
# assert POLL_TEST_OK -- which prints only if every check in
# c/apps/coreutils/polltest.c held against the real backends: a pipe whose
# readiness comes from the same wait queue a blocking read() parks on, POLLHUP
# on the writer closing, POLLNVAL on a closed fd with an INFINITE timeout (the
# case that hangs forever if it is wrong), select() over the same pipe, an
# eventfd written from another thread waking a poll in this one, and a timerfd
# advanced by the 100 Hz interrupt rather than by anything the program does.
#
# THE LOST-WAKEUP RACE IS NOT TESTED HERE, on purpose. It is proved on the host
# (make test-poll), where the event can be injected at the exact instruction
# between the readiness check and the sleep and the failure is deterministic in
# both directions. Provoking it here would be hoping for a race in a window a
# few hundred instructions wide, which is a flaky gate pretending to be a strict
# one.
#
# -smp 2 rather than 4: the eventfd and pipe checks want a genuine second core
# so the waker and the poller are not merely time-sliced, and nothing here
# measures wall-clock speedup, so more cores buy nothing and cost TCG time.
#
# It also PRINTS THE CEILING (POLL_CEILING lines) -- descriptors per process and
# threads per process, both measured by asking for them until one fails. Those
# two numbers are quoted in include/abi/logit_abi.h's poll block and had never
# been run on this machine.
#
# Portable: no `timeout`, same shape as run-thread-test.sh.

set -u

ISO="${1:?usage: run-poll-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-poll-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 11; printf '/bin/polltest\n'; sleep 90; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 2 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 2000); do
    if grep -aq "POLL_TEST_OK" "$LOG"; then
        echo "PASS: polltest -- pipes, the tty, eventfd and timerfd all answer poll() correctly"
        grep -a -E "POLL_CEILING|polltest:" "$LOG"
        exit 0
    fi
    if grep -aq "POLL_TEST_FAILED" "$LOG"; then
        echo "FAIL: polltest reported a failure"
        grep -a -E "POLL_FAIL|POLL_CEILING|polltest:" "$LOG"
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: POLL_TEST_OK not seen within timeout"
echo "----- serial output -----"
tail -120 "$LOG"
echo "-------------------------"
exit 1
