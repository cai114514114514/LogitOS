#!/usr/bin/env bash
# Headless test for /bin/libctest2 (tests/libc.mk): boot Logit, run it over
# the serial shell, assert "LIBC2_OK". Mirrors run-libc-test.sh exactly.
set -u
ISO="${1:?usage: run-libc2-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-libc2-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT
NET="-netdev user,id=n0 -device e1000,netdev=n0"
# LOGIT_ENVTEST is exported HERE, in the shell, on purpose: t_environ's
# assertion is that a value crosses fork + execve + crt0 + env_init, and a
# value the test set itself would prove none of that.
{ sleep 5; printf 'export LOGIT_ENVTEST=42\n'; sleep 1; printf '/bin/libctest2\n'; sleep 5; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!
for _ in $(seq 1 300); do
    if grep -aq "LIBC2_OK" "$LOG"; then
        echo "PASS: $(grep -a 'LIBC2_OK' "$LOG" | tail -1)"
        exit 0
    fi
    if grep -aq "LIBC2_FAIL" "$LOG"; then
        echo "FAIL: libctest2 reported failures:"
        grep -aE "FAIL:|LIBC2_FAIL" "$LOG" | tail -20
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.3
done
echo "FAIL: LIBC2_OK marker not seen (boot/run problem)"; grep -aE "FAIL:|libctest2|fault" "$LOG" | tail -20
exit 1
