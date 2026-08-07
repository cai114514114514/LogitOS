#!/usr/bin/env bash
# Headless mini-libc test: boot Logit, drive the serial shell to run /bin/libctest
# (which links the real mini-libc and exercises string/stdlib/stdio/ctype + the
# additions), and assert "LIBC_OK". Portable (no `timeout`). Mirrors run-as-test.sh.
set -u
ISO="${1:?usage: run-libc-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-libc-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT
NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 5; printf '/bin/libctest\n'; sleep 5; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!
for _ in $(seq 1 300); do
    if grep -aq "LIBC_OK" "$LOG"; then
        echo "PASS: $(grep -a 'LIBC_OK' "$LOG" | tail -1)"
        exit 0
    fi
    if grep -aq "LIBC_FAIL" "$LOG"; then
        echo "FAIL: libctest reported failures:"
        grep -aE "FAIL:|LIBC_FAIL" "$LOG" | tail -20
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.3
done
echo "FAIL: LIBC_OK marker not seen (boot/run problem)"; grep -aE "FAIL:|libctest|fault" "$LOG" | tail -20
exit 1
