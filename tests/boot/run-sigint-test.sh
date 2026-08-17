#!/usr/bin/env bash
# Ctrl+C on the real machine: does ^C typed on the serial console stop the
# foreground job, and does the shell come back with $? = 130?
#
# This is the only place the claim can be tested. The host suite
# (tests/unit/sh_edit_test.c) drives /bin/sh's own forwarding decision through a
# model of the kernel, which proves the shell asks -- it cannot prove the kernel
# delivers, because delivery is a frame pushed onto a ring-3 stack by
# c/kernel/exec/ksignal.c. Both halves have to be real for ^C to work, and the
# gap between them is exactly what ksignal.c:316 documents:
#
#     "What it does NOT do is deliver to the child, so `sleep 100` is not
#      interruptible by ^C until /bin/sh forwards it."
#
# So: start a 30-second sleep, type ^C three seconds in, and require the prompt
# back with a status of 130 inside a window far shorter than the sleep.
#
# TWO TRAPS THIS HARNESS IS BUILT AROUND, both of which would make it pass on a
# machine where ^C does nothing at all:
#
#   1. THE TTY ECHOES WHAT IS TYPED (c/kernel/exec/file.c, tty_read). Grepping
#      the log for a string that also appears in the input finds the ECHO, not
#      the output -- the shell need never have run the command. So the marker is
#      the OUTPUT of `echo $?`, whose text ("130") does not appear in anything
#      typed.
#   2. `sleep 30` FINISHES ON ITS OWN. A window long enough for that would pass
#      without any signal, so the negative control below runs the identical
#      script with the ^C removed and REQUIRES 130 not to appear.
#   3. A STATUS OF 130 IS NOT PROOF THE JOB DIED, and this one was found by
#      writing the first version of this file and then reading it again. /bin/sh
#      ALREADY returned 130 on ^C before it could signal anything: it abandoned
#      the job into the background and took the prompt back, which looks
#      identical from the console. So the gate also runs `jobs` afterwards and
#      requires it to print NOTHING -- an abandoned job is listed, and listed as
#      "abandoned"; a job that actually died was reaped and its slot released.
#      Without this the harness passes on the unfixed shell.
#
# usage: run-sigint-test.sh <iso> <disk.img> [nointr]

set -u

ISO="${1:?usage: run-sigint-test.sh <iso> <disk.img> [nointr]}"
DISK="${2:?usage: run-sigint-test.sh <iso> <disk.img> [nointr]}"
MODE="${3:-intr}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

feed() {
    sleep 6                       # boot + mount + init's /bin/sh on the console
    printf 'echo GATE_READY\n'
    sleep 2
    printf 'sleep 30\n'           # the foreground job
    sleep 3                       # let it actually be running
    if [ "$MODE" = "intr" ]; then
        printf '\003'             # ^C -- the whole point
    fi
    sleep 2
    printf 'echo $?\n'            # 130 iff the shell got its prompt back
    sleep 2
    printf 'jobs\n'               # silent iff the job actually DIED
    sleep 2
    printf 'uname\n'              # a marker that only the OUTPUT can produce
    sleep 6
}

feed | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Poll for up to ~28 s, for the LAST marker in the script: `uname`'s output. It
# is what says the whole sequence ran, so the `jobs` check below is reading a
# finished log rather than one that has not got there yet. `sleep 30` has not
# finished at any point in that window, which is what makes a 130 attributable
# to the signal.
for _ in $(seq 1 280); do
    grep -aq 'LogitOS x86_64' "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
saw130=0;  grep -aq '^130' "$LOG"      && saw130=1
sawjob=0;  grep -aq 'abandoned' "$LOG" && sawjob=1
sawend=0;  grep -aq 'LogitOS x86_64' "$LOG" && sawend=1

if ! grep -aq 'GATE_READY' "$LOG"; then
    echo "FAIL: the console shell never ran a command -- this says nothing about ^C"
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
fi

if [ "$MODE" = "intr" ]; then
    if [ "$saw130" = 1 ] && [ "$sawjob" = 0 ] && [ "$sawend" = 1 ]; then
        echo "PASS: ^C stopped the foreground job (jobs is empty) and the shell"
        echo "      returned with \$? = 130"
        exit 0
    fi
    if [ "$sawjob" = 1 ]; then
        echo "FAIL: the job is still listed as abandoned -- the shell took its"
        echo "      prompt back but never killed anything. That is the behaviour"
        echo "      c/kernel/exec/ksignal.c:316 describes, not the fix for it."
    elif [ "$sawend" = 0 ]; then
        echo "FAIL: the console shell never reached the end of the script"
    else
        echo "FAIL: ^C did not stop 'sleep 30' -- no 130 within the window"
    fi
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
else
    if [ "$saw130" = 1 ]; then
        echo "NEGATIVE CONTROL FAILED: 130 appeared with no ^C sent, so the"
        echo "positive result is not attributable to the signal"
        echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
        exit 1
    fi
    echo "negative control ok: with no ^C the job kept running and no 130 appeared"
    exit 0
fi
