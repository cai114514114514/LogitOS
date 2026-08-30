#!/usr/bin/env bash
# The storage wave's on-device gate: SYS_FTRUNCATE and the zero-filled hole,
# through the real fd layer, on the real disk, across a real reboot.
#
#   boot 1  fsroot/as/examples/storgate.as "run": write -> rewrite at a
#           different length -> ftruncate grow (gap must be zeros) ->
#           ftruncate shrink -> mid-file rewrite via lseek+write -> write past
#           EOF (gap must be zeros) -> whole-store rewrite, leaving a known
#           image on disk; plus the refusal checks.
#   boot 2  storgate.as "verify": read the image back and compare
#           byte-for-byte. No -snapshot anywhere: the two boots share one
#           persistent copy of the disk, the same shape run-durability-test.sh
#           uses, because a write that only lived in the first boot's kernel
#           buffer cannot pass boot 2.
#
# THE RED SIDE of this gate is not a stubbed kernel but the PRE-FIX
# measurement: the same asserts, run as fsroot/as/examples/storprobe.as
# against the unmodified kernel, answered "ftruncate rc -1" and
# "hole-nonzero-bytes 10 (of 16)" -- recorded 2026-08-30 in tests/storage.mk's
# header and in the wave's report. The host half and its -DSTORAGE_NEGCTL
# control (watched red: 8 failures) are tests/unit/storage_test.c.
#
# The negative control IS a prerequisite of this gate (below), not a ci-
# decoration: an on-device red would cost two more boots and prove what the
# host control already proved deterministically, but the host control must
# run every time this one does or the pair rots.

set -u
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-storage-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-storage-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
SG=/usr/as/examples/storgate.as

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

boot() {   # $1 = what to type, $2 = log, $3 = settle seconds after typing
    local cmds="$1" log="$2" settle="$3"
    { logit_wait_for_shell "$log" 120; printf '%s' "$cmds"; sleep "$settle"; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    QPID=$!
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 150 ]; do sleep 1; waited=$((waited + 1)); done
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do kill -0 "$QPID" 2>/dev/null || break; sleep 0.1; done
    QPID=""
    tr -d '\r' <"$log" >"$log.n" && mv "$log.n" "$log"
}

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "STORGATE|panic|fault|out of memory" "$f" | tail -25
    done
    exit 1
}

# ---- boot 1: the lifecycle, on the machine ---------------------------------
boot "mkdir /sg
as $SG run
echo BOOT1-DONE
" "$WORK/b1.log" 12
grep -aq BOOT1-DONE "$WORK/b1.log" || fail "boot 1 never finished its commands"
grep -aq STORGATE-RUN-DONE "$WORK/b1.log" || fail "boot 1: storgate run phase never completed"
if grep -aq "STORGATE-FAIL" "$WORK/b1.log"; then
    fail "boot 1: storgate reported failures"
fi
grep -aq "STORGATE-ok readonly ftruncate refused" "$WORK/b1.log" \
    || fail "boot 1: the refusal check did not run (a gate that asserts nothing is not a gate)"

# ---- boot 2: the same disk, a new machine ----------------------------------
boot "as $SG verify
echo BOOT2-DONE
" "$WORK/b2.log" 10
grep -aq BOOT2-DONE "$WORK/b2.log" || fail "boot 2 never finished its commands"
grep -aq STORGATE-VERIFY-DONE "$WORK/b2.log" || fail "boot 2: verify phase never completed"
if grep -aq "STORGATE-FAIL" "$WORK/b2.log"; then
    fail "boot 2: the image did not survive the reboot byte-exact"
fi
grep -aq "STORGATE-ok image across reboot byte-exact" "$WORK/b2.log" \
    || fail "boot 2: the byte-exact check did not run"

echo "PASS: write/rewrite/grow/shrink/midfile byte-verified on the machine,"
echo "      and the final image survived a full reboot on the same disk"
