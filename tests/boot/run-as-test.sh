#!/usr/bin/env bash
# Headless end-to-end AetherScript test: boot, drive the serial shell to run the
# packaged /bin/as on the /usr/as/*.as examples, and assert their output. This
# exercises the whole stack on real Logit: fork+execve /bin/as -> mini-libc fopen
# reads the script off LogitFS -> compile -> VM, incl. A2 (for/range) and A3
# (typed pointer + a direct SYS_WRITE syscall). Portable (no `timeout`).

set -u

ISO="${1:?usage: run-as-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-as-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 4; printf 'as /usr/as/examples/hello.as\nas /usr/as/examples/fib.as\nas /usr/as/examples/ptr.as\nas /usr/as/examples/sys.as\nas /usr/as/examples/use_mod.as\nas /usr/as/examples/dict.as\nas /usr/as/examples/closure.as\nas /usr/as/examples/gc.as\nas /usr/as/examples/classes.as\nas /usr/as/examples/exc.as\nas /usr/as/examples/stdlib.as\nas /usr/as/examples/strings.as\nas /usr/as/examples/sysdemo.as\nas /usr/as/examples/selfhost.as\nas /usr/as/examples/ports.as\nas /usr/as/examples/guidemo.as &\nexit\n'; sleep 12; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
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
       && grep -aq "stdlib ok" "$LOG" \
       && grep -aq "join: a,b,c" "$LOG" && grep -aq "fstr: n=7 sq=49" "$LOG" \
       && grep -aq "compif: 0,2,4,6" "$LOG" && grep -aq "swap: 2 1 unpack: 60" "$LOG" \
       && grep -aq "strings ok" "$LOG" \
       && grep -aq "roundtrip: written by AetherScript" "$LOG" && grep -aq "ls has it: true" "$LOG" \
       && grep -aq "spawn exit: 0" "$LOG" && grep -aq "clock sane: true" "$LOG" \
       && grep -aq "sysdemo ok" "$LOG" && grep -aq "gui ok" "$LOG" \
       && grep -aq "selfhost magic: LAQ1" "$LOG" && grep -aq "selfhost ok" "$LOG" \
       && grep -aq "ports lines: true" "$LOG" && grep -aq "ports pipe: alpha" "$LOG" \
       && grep -aq "ports redir: redirected" "$LOG" \
       && grep -aq "ports drop: 64 true" "$LOG" \
       && grep -aq "ports borrow: true" "$LOG" && grep -aq "ports ok" "$LOG"; then
        echo "PASS: /bin/as ran the full example suite incl. M23.5 sys + GUI + M27 ports + the self-hosted compiler (asc.as) ON Logit"
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
