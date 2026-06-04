#!/usr/bin/env bash
# Headless end-to-end shell test: boot, feed commands to the serial console where
# the kernel's init has launched /bin/sh, and assert the expected output. This
# exercises the whole real-process stack -- tty stdin -> sh -> fork+execve a
# coreutil -> pipe -> tty stdout. Portable (no `timeout` dependency).

set -u

ISO="${1:?usage: run-shell-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-shell-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# -snapshot: ephemeral disk writes, so the test is deterministic across runs.
{ sleep 4; printf 'uname\necho hello-aqua-shell\nls /bin | wc\ncat /docs/readme.txt | wc\nexit\n'; sleep 5; } | \
  "$QEMU" -cdrom "$ISO" -drive file="$DISK",format=raw,if=ide,index=0,media=disk -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Poll for the markers for up to ~20s.
for _ in $(seq 1 200); do
    if grep -aq "hello-aqua-shell" "$LOG" && grep -aq "Aqua OS x86_64" "$LOG"; then
        # both the echo builtin path and an exec'd coreutil produced output
        echo "PASS: shell ran builtins + fork/exec'd coreutils on the serial console"
        exit 0
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: expected shell output not seen"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
