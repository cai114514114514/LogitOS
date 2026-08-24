#!/usr/bin/env bash
# run-sysroot-test.sh -- boot LogitOS with the sysroot test image and run
# /bin/hello-tcc: a program COMPILED AND LINKED BY TCC (the host-built one,
# tests/sysroot.mk) against the packed sysroot -- /usr/lib/libc.a, crt1.o,
# libtcc1.a -- and wrapped by mkaex. It is the only thing in the tree that
# asks whether an ELF written by tcc's own linker (its segment layout, its
# -Ttext, no PT_INTERP with -static) is something this kernel's loader runs,
# and whether the sysroot's libc.a + crt0 actually work when reached through
# tcc rather than ld.lld.
#
# The expected line is fixed in tests/unit/sysroot_hello.c; every field in it
# comes from a different part of the sysroot (stdio, libm's sqrt, libtcc1's
# __va_arg and __floatundidf, crt0's argc). Shape mirrors run-libc-test.sh.
set -u
ISO="${1:?usage: run-sysroot-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-sysroot-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
WANT="hello from tcc: sum=42 sqrt2=1.41421 big=9223372036854775808 argc=1"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT
NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 6; printf '/bin/hello-tcc\n'; sleep 5; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!
for _ in $(seq 1 400); do
    if grep -aqF "$WANT" "$LOG"; then
        echo "PASS: $(grep -aF "$WANT" "$LOG" | tail -1)"
        exit 0
    fi
    if grep -aqE "hello-tcc.*(denied|not found|fault|No such)|\[execve\].*hello-tcc.*(fail|error|invalid)" "$LOG"; then
        echo "FAIL: the kernel refused /bin/hello-tcc:"
        grep -aE "hello-tcc|execve|fault" "$LOG" | tail -20
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.3
done
echo "FAIL: expected output not seen: $WANT"
echo "--- lines mentioning hello-tcc / execve / fault / 'hello from':"
grep -aE "hello-tcc|execve|fault|hello from|LogitOS shell" "$LOG" | tail -30
exit 1
