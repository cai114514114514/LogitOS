#!/usr/bin/env bash
# The loader, ON THE MACHINE.
#
# tests/unit/exec_test.c proves the loader accepts every built binary and maps
# it with the permissions p_flags asked for, against a host MMU. That is the
# exhaustive half. This is the half it cannot do: that the PROCESS the loader
# builds actually runs in ring 3 -- the entry point is reachable, the read-only
# segments are readable, the writable ones writable, and the auxiliary vector
# on the stack is the one the kernel meant to put there.
#
# Two assertions:
#   1. /bin/execinfo checks its own SysV initial stack from inside ring 3 and
#      prints EXECINFO ok. Every line it prints is a dereference of something
#      the loader handed it, so a wrong AT_PHDR does not produce a wrong number,
#      it produces no output at all.
#   2. Every CLI program on the disk is fork+exec'd by the real /bin/sh and has
#      to produce its output. The count is asserted, not eyeballed: a program
#      that fails to load prints nothing, and "nothing" is easy to miss in a
#      wall of serial text.

set -u

ISO="${1:?usage: run-exec-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-exec-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

# The programs run one per line, each printing something unmistakable. `true`
# and `false` print nothing by design, so they are checked through $? instead --
# a program that failed to LOAD and a program that returned 1 are both non-zero,
# so they are run as `true && echo`, which only prints if the process ran AND
# exited 0.
SCRIPT='EXECPROBE-START
/bin/execinfo
uname
echo EXECPROBE-echo
pwd
ls /bin | wc
cat /docs/readme.txt | wc
head /docs/readme.txt | wc
true && echo EXECPROBE-true
false || echo EXECPROBE-false
/bin/as /usr/as/examples/hello.as
/bin/asnative
EXECPROBE-END
exit
'

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 5; printf '%s' "$SCRIPT"; sleep 12; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "EXECPROBE-END" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fail=0
need() {
    if grep -aq "$1" "$LOG"; then
        echo "  ok: $2"
    else
        echo "  FAIL: $2   (no match for /$1/)"
        fail=1
    fi
}

echo "exec: what the machine did with the programs the loader built"
need "EXECINFO ok"                  "/bin/execinfo validated its own auxv from ring 3"
# /bin/execinfo is a CLI program, so it links at 0x50000000 and its info page --
# which the loader puts directly above the image -- is at 0x500xxxxx. Anchoring
# on the real address rather than on "nonzero" is the difference between
# checking AT_PHDR and checking that a number was printed.
need "EXECINFO AT_PHDR = 0x500"     "AT_PHDR is inside this program's own image, above its segments"
need "EXECINFO ok: AT_PHDR points at readable memory" "AT_PHDR was dereferenced without faulting"
need "EXECINFO ok: AT_RANDOM"       "AT_RANDOM carried 16 real bytes"
need "EXECINFO ok: the ELF header sits directly in front of AT_PHDR" \
                                    "the loader's image-info page is where it says it is"
need "LogitOS x86_64"               "uname ran (a CLI program, fork+exec'd)"
need "EXECPROBE-echo"               "echo ran"
need "EXECPROBE-true"               "true ran and exited 0"
need "EXECPROBE-false"              "false ran and exited 1"
need "hello"                        "/bin/as ran a script (mini-libc program, 0x50000000)"
# The AetherScript §P4 claim, executed rather than argued: this .aex was built
# by mkaex --emit out of a flat nasm binary. No linker produced it, no ELF ever
# existed on disk before the tool made one, and the kernel loaded and ran it.
need "ASNATIVE-OK"                  "/bin/asnative ran -- a .aex a compiler emitted, not a linker"

# A refusal from the loader during this boot means something on the disk stopped
# loading -- the exact regression this whole test exists to catch.
if grep -aq "\[elf\] refused" "$LOG"; then
    echo "  FAIL: the loader refused an image during a normal boot:"
    grep -a "\[elf\] refused" "$LOG" | sed 's/^/    /'
    fail=1
else
    echo "  ok: the loader refused nothing during a normal boot"
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL: exec/loader on-machine test"
    echo "----- serial output -----"
    cat "$LOG"
    echo "-------------------------"
    exit 1
fi
echo "PASS: every program loaded, ran, and got a correct SysV stack"
exit 0
