#!/usr/bin/env bash
# Does LogitFS actually store data?
#
# Every other boot harness passes -snapshot, which throws the disk away when QEMU
# exits. That makes those tests deterministic -- and it means nothing in the tree
# has ever asserted that a write survives a reboot. CLAUDE.md says as much:
# "cross-boot write durability (corrupts after repeated non-snapshot boots; use
# -snapshot)". A filesystem only ever exercised with its writes discarded has not
# been shown to be a filesystem.
#
# So: no -snapshot, five real boots against ONE disk image (a copy, so the build
# artifact is never touched).
#
#   boot 1  write three files covering the three inode shapes: one block, several
#           direct blocks, and past direct[12] into the single-indirect block
#   boot 2  verify all three, then CHURN -- allocate and free repeatedly, which is
#           what should shake out a free-block bitmap drifting out of step with
#           the inode table. That damage never shows on the churned files; it
#           shows on a bystander written before, which is exactly what the three
#           originals are.
#   boot 3  verify again, churn again
#   boot 4  verify again
#   boot 5  verify one final time
#
# Verification is byte-for-byte (durcheck.as regenerates the expected content),
# not a length check: a filesystem that hands one block to two files produces a
# file of exactly the right size holding somebody else's data.

set -u

ISO="${1:?usage: run-durability-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-durability-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
DC=/usr/as/examples/durcheck.as

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# One boot against the persistent copy. $1 = what to type, $2 = log, $3 = settle
# seconds after typing (writes go straight through virtio-blk with no cache, so
# this is settle time, not a race we are trying to win).
boot() {
    local cmds="$1" log="$2" settle="$3"
    { sleep 5; printf '%s' "$cmds"; sleep "$settle"; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    QPID=$!
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 150 ]; do sleep 1; waited=$((waited + 1)); done
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    QPID=""
    tr -d '\r' <"$log" >"$log.n" && mv "$log.n" "$log"   # serial is CRLF
}

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "DURCHECK|BOOT[0-9]-DONE|panic|fault|out of memory|cannot" "$f" | tail -25
    done
    echo "  (full logs were in $WORK, now removed; re-run to inspect)"
    exit 1
}

verify_all() {   # $1 = log, $2 = which boot
    grep -aq "DURCHECK-OK /dur/tiny.bin"  "$1" || fail "boot $2: /dur/tiny.bin (single block) did not verify"
    grep -aq "DURCHECK-OK /dur/small.bin" "$1" || fail "boot $2: /dur/small.bin (direct blocks) did not verify"
    grep -aq "DURCHECK-OK /dur/mid.bin"   "$1" || fail "boot $2: /dur/mid.bin (single-indirect) did not verify"
}

VERIFY="as $DC verify /dur/tiny.bin tiny
as $DC verify /dur/small.bin small
as $DC verify /dur/mid.bin mid
"

# ---- boot 1: write ----------------------------------------------------------
boot "mkdir /dur
as $DC write /dur/tiny.bin tiny
as $DC write /dur/small.bin small
as $DC write /dur/mid.bin mid
${VERIFY}echo BOOT1-DONE
" "$WORK/b1.log" 10
grep -aq "BOOT1-DONE" "$WORK/b1.log" || fail "boot 1 never finished its commands"
verify_all "$WORK/b1.log" "1 (before any reboot -- the writes themselves are wrong)"

# ---- boot 2: verify across the first reboot, then churn ---------------------
boot "${VERIFY}as $DC churn /dur
echo BOOT2-DONE
" "$WORK/b2.log" 10
grep -aq "BOOT2-DONE" "$WORK/b2.log" || fail "boot 2 never finished its commands"
verify_all "$WORK/b2.log" 2
grep -aq "DURCHECK-CHURN-DONE" "$WORK/b2.log" || fail "boot 2: churn did not complete"

# ---- boot 3: verify after churn, churn again --------------------------------
boot "${VERIFY}as $DC churn /dur
echo BOOT3-DONE
" "$WORK/b3.log" 10
grep -aq "BOOT3-DONE" "$WORK/b3.log" || fail "boot 3 never finished its commands"
verify_all "$WORK/b3.log" "3 (after one round of allocate/free churn)"

# ---- boots 4 and 5: keep reading ---------------------------------------------
boot "${VERIFY}echo BOOT4-DONE
" "$WORK/b4.log" 8
grep -aq "BOOT4-DONE" "$WORK/b4.log" || fail "boot 4 never finished its commands"
verify_all "$WORK/b4.log" "4 (after two rounds of churn)"

boot "${VERIFY}echo BOOT5-DONE
" "$WORK/b5.log" 8
grep -aq "BOOT5-DONE" "$WORK/b5.log" || fail "boot 5 never finished its commands"
verify_all "$WORK/b5.log" 5

echo "PASS: three files (1 block / direct / single-indirect) verified byte-for-byte"
echo "      across 5 real boots with 2 rounds of allocate-free churn between them"
