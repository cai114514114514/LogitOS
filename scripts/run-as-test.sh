#!/usr/bin/env bash
# Headless end-to-end AetherScript test: boot, drive the serial shell to run the
# packaged /bin/as on the /usr/as/*.as examples, and assert their output. This
# exercises the whole stack on real Aether: fork+execve /bin/as -> mini-libc fopen
# reads the script off AetherFS -> compile -> VM, incl. A2 (for/range) and A3
# (typed pointer + a direct SYS_WRITE syscall). Portable (no `timeout`).

set -u

ISO="${1:?usage: run-as-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-as-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 4; printf 'as /usr/as/examples/hello.as\nas /usr/as/examples/fib.as\nas /usr/as/examples/ptr.as\nas /usr/as/examples/sys.as\nas /usr/as/examples/use_mod.as\nas /usr/as/examples/dict.as\nas /usr/as/examples/closure.as\nas /usr/as/examples/gc.as\nas /usr/as/examples/classes.as\nas /usr/as/examples/exc.as\nas /usr/as/examples/stdlib.as\nexit\n'; sleep 7; } | \
  "$QEMU" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Markers: A2 for/range (count 4), recursion (fib), A3 typed pointer (1337),
# A3 direct syscall write, and modules (import: quad(2)=16 via a module-mate call).
for _ in $(seq 1 300); do
    if grep -aq "count 4" "$LOG" && grep -aq "fib(20) = 6765" "$LOG" \
       && grep -aq "p\[0\] + p\[1\] = 1337" "$LOG" && grep -aq "hello via syscall" "$LOG" \
       && grep -aq "quad(2) = 16" "$LOG" && grep -aq "from-import square(9) = 81" "$LOG" \
       && grep -aq "sum = 6" "$LOG" \
       && grep -aq "counts: 1 2 3" "$LOG" && grep -aq "double 21: 42" "$LOG" \
       && grep -aq "gc ok" "$LOG" \
       && grep -aq "dog: Rex makes a sound (woof) name: Rex" "$LOG" \
       && grep -aq "counter: 1 2 3" "$LOG" \
       && grep -aq "exc ok" "$LOG" && grep -aq "caught: boom" "$LOG" \
       && grep -aq "runtime caught" "$LOG" \
       && grep -aq "stdlib ok" "$LOG"; then
        echo "PASS: /bin/as ran A1+A2+A3+import+dict+closures+gc+classes+exceptions+stdlib scripts on Aether"
        exit 0
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: expected AetherScript output not seen"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
