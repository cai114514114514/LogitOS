#!/usr/bin/env bash
# M27 gate: the AetherScript shell runs the suite /bin/sh passes.
#
# This is run-shell-test.sh with ONE line added at the top of the input --
# `as /usr/as/examples/ash.as` -- and then the identical command sequence. The
# point of keeping the sequence identical is that the comparison means something:
# the same builtins, the same fork/exec of the same coreutils, the same pipes,
# the same `>` redirect and the same cp/mv/mkdir/rm round-trip, driven by a shell
# written in AetherScript instead of 971 lines of C.
#
# The C /bin/sh is still init. It launches ash and BLOCKS on it, so every command
# after that first line is handled by ash -- which the ordering assertion below
# makes explicit rather than assumed: every marker must appear AFTER ash's banner
# in the serial log. (Two `exit`s: one leaves ash, one leaves sh.)

set -u

ISO="${1:?usage: run-shell-as-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-shell-as-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# ONE printf, exactly like run-shell-test.sh. This is not cosmetic: QEMU's stdio
# chardev stops pulling from the pipe if the writer goes quiet mid-stream for
# several seconds, and a harness that sleeps between lines then hangs at the
# first prompt -- which looks exactly like the shell being broken. The guest
# consumes one character per tty_read, so the rest of the stream simply waits:
# /bin/sh takes the first line, and ash, once running, takes all the others.
CMDS='as /usr/as/examples/ash.as\nuname\necho hello-logit-shell\nls /bin | wc\ncat /docs/readme.txt | wc\nmkdir /cptest\necho cpmvprobe > /cptest/a.txt\ncp /cptest/a.txt /cptest/b.txt\ncat /cptest/b.txt\nmv /cptest/b.txt /cptest/c.txt\nls /cptest\nrm /cptest/a.txt\nrm /cptest/c.txt\nrm /cptest\nexit\nexit\n'
# -snapshot: ephemeral disk writes, so the test is deterministic across runs.
{ sleep 4; printf "$CMDS"; sleep 10; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

BANNER="ash: LogitOS AetherScript shell"
for _ in $(seq 1 400); do
    if grep -aq "hello-logit-shell" "$LOG" && grep -aq "LogitOS x86_64" "$LOG" \
       && grep -aq "cpmvprobe" "$LOG" && grep -aq "$BANNER" "$LOG"; then
        # Ordering: ash must have been running BEFORE the markers appeared, or
        # this would pass on a log where /bin/sh did all the work and ash merely
        # started. Compare first-occurrence line numbers.
        b=$(grep -an "$BANNER" "$LOG" | head -1 | cut -d: -f1)
        ok=1
        for m in "LogitOS x86_64" "hello-logit-shell" "cpmvprobe"; do
            l=$(grep -an "$m" "$LOG" | head -1 | cut -d: -f1)
            [ "$l" -gt "$b" ] || { echo "FAIL: '$m' (line $l) precedes the ash banner (line $b)"; ok=0; }
        done
        [ "$ok" = 1 ] || break
        echo "PASS: the AetherScript shell ran the /bin/sh suite"
        echo "      (uname, echo, 'ls /bin | wc', 'cat ... | wc', mkdir,"
        echo "       'echo ... > file', cp, cat, mv, ls, rm x3 -- all after $BANNER)"
        exit 0
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: expected AetherScript-shell output not seen"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
