#!/usr/bin/env bash
# Headless end-to-end AquaScript test: boot, drive the serial shell to run the
# packaged /bin/aqs on the /usr/aqs/*.aqs examples, and assert their output. This
# exercises the whole stack on real Aqua: fork+execve /bin/aqs -> mini-libc fopen
# reads the script off AquaFS -> compile -> VM, incl. A2 (for/range) and A3
# (typed pointer + a direct SYS_WRITE syscall). Portable (no `timeout`).

set -u

ISO="${1:?usage: run-aqs-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-aqs-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 4; printf 'aqs /usr/aqs/hello.aqs\naqs /usr/aqs/fib.aqs\naqs /usr/aqs/ptr.aqs\naqs /usr/aqs/sys.aqs\naqs /usr/aqs/use_mod.aqs\naqs /usr/aqs/dict.aqs\nexit\n'; sleep 7; } | \
  "$QEMU" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Markers: A2 for/range (count 4), recursion (fib), A3 typed pointer (1337),
# A3 direct syscall write, and modules (import: quad(2)=16 via a module-mate call).
for _ in $(seq 1 300); do
    if grep -aq "count 4" "$LOG" && grep -aq "fib(20) = 6765" "$LOG" \
       && grep -aq "p\[0\] + p\[1\] = 1337" "$LOG" && grep -aq "hello via syscall" "$LOG" \
       && grep -aq "quad(2) = 16" "$LOG" && grep -aq "from-import square(9) = 81" "$LOG" \
       && grep -aq "sum = 6" "$LOG"; then
        echo "PASS: /bin/aqs ran A1+A2+A3+import+dict scripts on Aqua"
        exit 0
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: expected AquaScript output not seen"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
