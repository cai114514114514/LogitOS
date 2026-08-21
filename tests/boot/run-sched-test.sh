#!/usr/bin/env bash
# Weighted-scheduler gate. Boot, drive the serial shell to run /bin/schedtest,
# and require SCHED-RESULT to be a clean sweep.
#
# WHAT THE PROGRAM ACTUALLY MEASURES is in c/apps/coreutils/schedtest.c: two
# forked CPU-bound children at chosen nice values, and the ratio of the work
# each COMPLETED (an iteration count kept in ring 3) against the ratio of the
# weights the kernel says its pick loop is using. Three cases -- 1:1, 2:1, 4:1
# -- plus four API checks.
#
# WHY THE 1:1 CASE MATTERS MOST HERE: it is the run in which the feature under
# test contributes nothing, so whatever it deviates by is the machine's own
# interference (the compositor, kworker, the shell) and not the scheduler's
# arithmetic. Read it before reading the other two.
#
# -smp 2 rather than 1 or 4. One core makes "who runs next" the only question
# and hides any effect of ring order; four cores under TCG give two CPU-bound
# children a core each, so neither ever waits and every ratio comes out 1:1
# whatever the weights are -- which would be this gate passing while measuring
# nothing. Two cores with the compositor and the shell also runnable is the
# smallest machine on which the children genuinely contend.
#
# Portable: no `timeout`, same shape as run-thread-test.sh.

set -u

ISO="${1:?usage: run-sched-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-sched-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG" "$LOG.disk"; }
trap cleanup EXIT

# WORK FROM A PRIVATE COPY OF THE DISK, not from build/disk.img itself. This is
# not caution: the negative control reported "/bin/schedtest: permission denied
# (not executable)" for a program that had executed correctly ten minutes
# earlier on the same image, because another session ran `make build/disk.img`
# while QEMU had the file open. -snapshot stops the GUEST writing to the image
# and does nothing at all about the HOST rewriting it underneath. A 64 MiB copy
# costs well under a second and makes the run reproducible whatever else the
# tree happens to be doing.
DISKCOPY="$LOG.disk"
cp "$DISK" "$DISKCOPY"
DISK="$DISKCOPY"

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# Boot to the serial shell (~11s), run the measurement, then run the two
# CONSUMERS in the same boot.
#
# `/bin/nice -n 10 /bin/renice -g 0` is one line and it is the end-to-end proof
# that nothing else here gives: /bin/nice sets a value on ITSELF and then
# execve()s a different program, and /bin/renice -- now a separate image in the
# same thread -- reads 10 back out of the kernel. If execve did not preserve
# nice there would be no code anywhere to notice, because there IS no code
# anywhere that carries a priority across exec; it survives because the thread
# does. schedtest cannot cover this: it uses the libc API inside one process
# and never execs.
{ sleep 11; printf '/bin/schedtest 4000\n'; sleep 90; \
  printf '/bin/nice\n'; sleep 3; \
  printf '/bin/nice -n 10 /bin/renice -g 0\n'; sleep 3; \
  printf '/bin/echo SCHED-CONSUMER-DONE\n'; sleep 3; \
  printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 2 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 3000); do
    if grep -aq "SCHED-CONSUMER-DONE" "$LOG"; then
        grep -a -E "^SCHED-(CASE|RAW|FAIL|RESULT)" "$LOG"
        # tr -d '\r' is NOT cosmetic: the guest console is a serial tty and every
        # line arrives CRLF-terminated, so `awk '{print $2}'` yields "6/6\r" and
        # the string compare below silently never matches. This gate reported
        # FAIL on a run whose own printed output said 6/6 -- the shape CLAUDE.md
        # names as the expensive one, a right measurement with a sentence around
        # it that sends the reader somewhere else.
        R=$(grep -a "^SCHED-RESULT" "$LOG" | tail -1 | tr -d '\r' | awk '{print $2}')
        rc=0
        if [ "$R" != "6/6" ]; then echo "FAIL: schedtest reported ${R:-nothing}"; rc=1; fi

        # THE CONSUMERS. Exact strings, not "did it print something": /bin/nice
        # with no argument must report 0 (this shell's nice, untouched), and the
        # exec chain must report exactly nice=10 with the weight the table gives
        # for it. Checking the WEIGHT as well as the nice is the half that
        # matters -- a kernel that stored 10 and weighted it as 1024 would print
        # a perfectly plausible "nice=10" and schedule nothing differently.
        # Same CR again, and it bit twice: `grep '^0$'` cannot match the line
        # "0\r" because $ anchors AFTER the carriage return. Every anchored
        # match against this log has to strip it first.
        if tr -d '\r' < "$LOG" | grep -aq "^0$"; then
            echo "ok   /bin/nice with no argument reports this shell's nice: 0"
        else
            echo "FAIL: /bin/nice did not report 0"; rc=1
        fi
        if tr -d '\r' < "$LOG" | grep -aq "^0: nice=10 weight=512$"; then
            echo "ok   /bin/nice -n 10 -> execve -> /bin/renice reads back nice=10 weight=512"
        else
            echo "FAIL: nice did not survive execve into renice"
            grep -a "nice=" "$LOG" | tail -3
            rc=1
        fi

        if [ "$rc" = 0 ]; then
            echo "PASS: weighted scheduling -- 1:1, 2:1 and 4:1 hold (CPU share within 5%,"
            echo "      completed work within 25%; see the two-bound note in schedtest.c),"
            echo "      API checks green, and /bin/nice + /bin/renice work across execve"
        fi
        exit $rc
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: SCHED-CONSUMER-DONE not seen within timeout"
echo "----- serial output -----"
tail -120 "$LOG"
echo "-------------------------"
exit 1
