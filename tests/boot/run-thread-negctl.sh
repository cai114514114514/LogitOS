#!/usr/bin/env bash
# The negative controls for the M30 threads gate.
#
# Four builds of c/apps/coreutils/thrtest.c, each with exactly ONE guard removed
# (tests/thread.mk builds them; the -D names are documented there and in the
# header of thrtest.c). Every one MUST print THREAD_TEST_FAIL. If any of them
# passes, the corresponding assertion in the real thrtest is not testing what it
# says it is -- an assertion nobody has watched fail is not a known-failing
# assertion, which is the whole reason this script exists.
#
#   thrtest-serial   threads created and joined one at a time. The wall-clock
#                    speedup check must fail. THIS IS THE IMPORTANT ONE: it is
#                    what makes the passing run's "T4 < 2*T1" mean "four threads
#                    ran at once" rather than "the number came out small".
#   thrtest-tls      the per-thread value read from a shared global.
#   thrtest-nolock   the mutex removed from a split read-modify-write.
#   thrtest-leak     pthread_detach never called, so nothing is freed.
#
# All four run in ONE boot: each is a separate process, so a failure in one
# cannot affect the next, and a single QEMU start under TCG is minutes cheaper
# than four.

set -u

ISO="${1:?usage: run-thread-negctl.sh <iso> <disk_with_controls.img>}"
DISK="${2:?usage: run-thread-negctl.sh <iso> <disk_with_controls.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

CTRLS="serial tls nolock leak"
NET="-netdev user,id=n0 -device e1000,netdev=n0"

{
  sleep 11
  for c in $CTRLS; do
      printf 'echo NEGCTL-BEGIN-%s\n' "$c"; sleep 2
      printf '/bin/thrtest-%s\n' "$c";      sleep 150
      printf 'echo NEGCTL-END-%s\n' "$c";   sleep 2
  done
  printf 'exit\n'; sleep 2
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Wait for the last marker, or for QEMU to go away.
for _ in $(seq 1 7000); do
    grep -aq "NEGCTL-END-leak" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

rc=0
for c in $CTRLS; do
    # The slice of the log belonging to this control.
    seg=$(awk "/NEGCTL-BEGIN-$c/{f=1;next} /NEGCTL-END-$c/{f=0} f" "$LOG")
    if printf '%s' "$seg" | grep -aq "THREAD_TEST_FAIL"; then
        echo "PASS(control): thrtest-$c failed, as it must"
        printf '%s' "$seg" | grep -a -E "^FAIL|THREAD_TEST_FAIL" | head -3
    elif printf '%s' "$seg" | grep -aq "THREAD_TEST_OK"; then
        echo "FAIL(control): thrtest-$c PASSED -- the assertion it removes is not testing anything"
        rc=1
    else
        echo "FAIL(control): thrtest-$c produced no verdict (did it run? did it hang?)"
        printf '%s' "$seg" | tail -20
        rc=1
    fi
done

if [ "$rc" -ne 0 ]; then
    echo "----- serial output -----"
    tail -200 "$LOG"
    echo "-------------------------"
fi
exit "$rc"
