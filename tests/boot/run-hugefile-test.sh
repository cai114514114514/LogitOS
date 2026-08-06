#!/usr/bin/env bash
# The double-indirect tree, on real hardware(-ish).
#
# LogitFS v4 grew files past the single-indirect ceiling (1036 blocks, ~4.1 MiB)
# with a two-level indirect tree. That code path is never touched by any other
# test -- every other fixture fits in single-indirect -- so this harness exists
# to prove it end to end:
#
#   boot 1: write a 4.4 MB file (1075 blocks, past the ceiling), verify it
#           byte-for-byte in the same boot (write path + imap read path)
#   boot 2: verify it again after a clean reboot (the tree on disk, not the
#           page cache, is what gets read)
#
# The content is a function of the byte offset (durcheck.as), so any block
# mapped to the wrong place -- an off-by-one in the L1/L2 split -- shows up as
# a named first-bad-byte, not a shrug.
#
# Slow by nature: the AS interpreter builds 4.4 MB of string twice per verify.
# Budget several minutes per boot.

set -u

ISO="${1:?usage: run-hugefile-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-hugefile-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
DC=/usr/as/examples/durcheck.as

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
WAITMAX=900        # AS builds 4.4 MB char by char; give it room

start_qemu() {   # $1 = what to type, $2 = log
    { sleep 5; printf '%s' "$1"; sleep 1200; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$2" 2>/dev/null &
    QPID=$!
}

normlog() { tr -d '\r' <"$1" >"$1.n" && mv "$1.n" "$1"; }

# Never `wait` for a SIGKILLed background-pipeline member here: under WSL the
# SIGCHLD is never delivered and bash's do_wait wedges until some OTHER child
# dies. Kill, then poll for the process to vanish (bounded; a stale zombie is
# tolerated -- it holds no open files and cannot touch the disk image).
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    QPID=""
}

boot() {         # run, wait for BOOTMARK-DONE (long), settle, stop
    local cmds="$1" log="$2"
    start_qemu "$cmds" "$log"
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt "$WAITMAX" ]; do
        grep -aq "BOOTMARK-DONE" "$log" 2>/dev/null && break
        sleep 2; waited=$((waited + 2))
    done
    sleep 5
    reap
    normlog "$log"
}

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "DURCHECK|BOOTMARK|\[fs\]|panic|fault|corrupt|cannot" "$f" | tail -20
    done
    exit 1
}

check_ok() {       # $1 = log, $2 = context
    grep -aq "BOOTMARK-DONE" "$1" || fail "$2: never finished"
    grep -aq "\[fs\] mounted" "$1" || fail "$2: filesystem did not mount"
    if grep -aq "DURCHECK-FAIL /dur/huge.bin" "$1"; then
        fail "$2: $(grep -a 'DURCHECK-FAIL /dur/huge.bin' "$1" | head -1)"
    fi
    grep -aq "DURCHECK-OK /dur/huge.bin" "$1" || fail "$2: no verdict for /dur/huge.bin"
}

# ---- boot 1: write across the ceiling, verify in the same boot ----------------
echo "== boot 1: write + verify 4.4 MB (double-indirect) =="
boot "mkdir /dur
as $DC write /dur/huge.bin huge
as $DC verify /dur/huge.bin huge
echo BOOTMARK-DONE
" "$WORK/b1.log"
check_ok "$WORK/b1.log" "boot 1"
echo "  write + same-boot verify: OK"

# ---- boot 2: clean reboot, verify from disk -----------------------------------
echo "== boot 2: verify after reboot =="
boot "as $DC verify /dur/huge.bin huge
echo BOOTMARK-DONE
" "$WORK/b2.log"
check_ok "$WORK/b2.log" "boot 2"
echo "  post-reboot verify: OK"

echo "PASS: 4.4 MB file (1075 blocks, past the single-indirect ceiling) verified"
echo "      byte-for-byte in the writing boot and again after a clean reboot"
