#!/usr/bin/env bash
# Does a file's MODE survive a reboot, on the real machine?
#
# The host suite (make test-statmeta) proves this against a simulated device.
# This proves it against QEMU, virtio-blk and the kernel that ships -- and it is
# not redundant, because the two can disagree in exactly one direction that
# matters: the host stub keeps its media in a malloc'd array that no amount of
# missing barriers can lose, while a real device has a write cache and a real
# reboot discards every byte of RAM the kernel was holding.
#
# NO -snapshot. That is the entire point, and CLAUDE.md warns specifically
# against adding it to a harness to work around a write: every other boot test
# throws the disk away when QEMU exits, which is why nothing in the tree
# asserted cross-boot durability until test-durability did. Four real boots
# against ONE image (a copy, so the build artifact is never touched):
#
#   boot 1  set modes and owners: a file, a directory, and chmod 000 on a third.
#           Create a fourth and never touch it -- the bystander that proves an
#           untouched file still reports the DEFAULT and says so.
#   boot 2  read them all back. This is the assertion the RAM store could never
#           have passed, and the one the negative control fails.
#   boot 3  churn: write unrelated files, then re-read the modes. A metadata
#           store that survives a quiet reboot but not an allocating one is a
#           real failure mode and it does not show on the churned files.
#   boot 4  read them one final time.
#
# What is checked is the WHOLE line /bin/stat prints, not just the mode: an
# implementation that got the mode right and the owner wrong, or that lost the
# LSTA_MODE_DURABLE bit while still reporting 0741, is broken in a way a mode
# check alone cannot see.

set -u

ISO="${1:?usage: run-statmeta-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-statmeta-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# Never `wait` for a SIGKILLed background-pipeline member here: under WSL the
# SIGCHLD is never delivered and bash's do_wait wedges until some OTHER child
# dies. Kill, then poll for the process to vanish.
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    QPID=""
}

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
    reap
    tr -d '\r' <"$log" >"$log.n" && mv "$log.n" "$log"   # serial is CRLF
}

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "STAT |BOOT[0-9]-DONE|panic|fault|cannot" "$f" | tail -25
    done
    echo "  (full logs were in $WORK, now removed; re-run to inspect)"
    exit 1
}

# ---- boot 1: set the metadata ----------------------------------------------
boot "mkdir /meta
echo hello > /meta/f.txt
echo plain > /meta/bystander.txt
echo locked > /meta/locked.txt
mkdir /meta/d
stat -c 741 /meta/f.txt
stat -o 7:9 /meta/f.txt
stat -c 700 /meta/d
stat -c 0 /meta/locked.txt
stat /meta/f.txt /meta/d /meta/locked.txt /meta/bystander.txt
echo BOOT1-DONE
" "$WORK/b1.log" 10
grep -aq "BOOT1-DONE" "$WORK/b1.log" || fail "boot 1 never finished its commands"
grep -aq "STAT-CHMOD-OK /meta/f.txt" "$WORK/b1.log" || fail "boot 1: chmod was refused"
grep -aq "STAT-CHOWN-OK /meta/f.txt" "$WORK/b1.log" || fail "boot 1: chown was refused"
grep -aq "STAT /meta/f.txt mode=741 " "$WORK/b1.log" ||
    fail "boot 1: the mode did not read back even WITHIN the boot -- nothing else here can pass"
