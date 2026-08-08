#!/usr/bin/env bash
# Does the REAL kernel agree that the filesystem is consistent?
#
# LogitFS now runs a read-only fsck at every mount: it walks the inode table and
# the directory tree and compares what they reference against the free-block
# bitmap, and prints "[fsck] mount check: clean" or a count of what it found.
# The host tests (make test-fs-host) drive that checker exhaustively against a
# simulated device; this one asserts the other half -- that the kernel, on a real
# virtio-blk device, running the same code, says clean about the image this build
# actually produced and about an image it has itself written to.
#
# NO -snapshot. Every other harness in this tree passes it, which throws the disk
# away on exit and is exactly why "corrupts after repeated non-snapshot boots"
# sat in CLAUDE.md unreproduced for as long as it did. Two real boots against one
# image:
#
#   boot 1  mkfs's image: mount check must be clean. Then write files, make a
#           directory, delete something -- work that moves the bitmap and the
#           inode table together.
#   boot 2  the same image, now written by the kernel: mount check must STILL be
#           clean, and the files from boot 1 must read back.
#
# A "[fsck] mount check: N problem(s)" line in either boot fails the test and is
# printed verbatim, because the class of problem is the whole diagnosis.

set -u

# Wait for /bin/sh to exist before typing at it, rather than sleeping a
# guessed number of seconds. See tests/boot/bootwait.sh for why a longer
# sleep is the same bug with a bigger number.
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-fsmount-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-fsmount-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# Never `wait` for a SIGKILLed background-pipeline member under WSL: the SIGCHLD
# is never delivered and bash's do_wait wedges until some other child dies.
# Kill, then poll for the process to vanish (bounded; a stale zombie holds no
# open files and cannot touch the image the next boot is about to open).
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    QPID=""
}

boot() {   # $1 = what to type, $2 = log, $3 = settle seconds
    local cmds="$1" log="$2" settle="$3"
    { logit_wait_for_shell "$log" 120; printf '%s' "$cmds"; sleep 600; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    QPID=$!
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 180 ]; do
        grep -aq "FSMOUNT-DONE" "$log" 2>/dev/null && break
        sleep 1; waited=$((waited + 1))
    done
    sleep "$settle"
    reap
    tr -d '\r' <"$log" >"$log.n" && mv "$log.n" "$log"
}

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "fsck|\[fs\]|FSMOUNT|panic|fault" "$f" | tail -20
    done
    exit 1
}

assert_clean() {   # $1 = log, $2 = which boot
    grep -aq "FSMOUNT-DONE" "$1" || fail "boot $2 never finished its commands"
    grep -aq "\[fs\] mounted" "$1" || fail "boot $2: the filesystem did not mount"
    if grep -aq "mount check: [0-9]* problem" "$1"; then
        echo "  the kernel's own checker reported:"
        grep -aE "mount check:|^\[fsck\]" "$1" | tail -10
        fail "boot $2: the mount-time fsck found problems"
    fi
    grep -aq "mount check: clean" "$1" \
        || fail "boot $2: no mount-check verdict at all -- did the check run?"
    echo "  boot $2: [fsck] mount check: clean"
}

# ---- boot 1: mkfs's image, then write to it --------------------------------
boot "mkdir /fsm
echo hello-from-boot-one > /fsm/a.txt
echo second > /fsm/b.txt
mkdir /fsm/sub
echo third > /fsm/sub/c.txt
rm /fsm/b.txt
cat /fsm/a.txt
echo FSMOUNT-DONE
" "$WORK/b1.log" 10
assert_clean "$WORK/b1.log" 1
grep -aq "hello-from-boot-one" "$WORK/b1.log" || fail "boot 1: the file did not read back"

# ---- boot 2: the same image, now written by the kernel ----------------------
boot "cat /fsm/a.txt
cat /fsm/sub/c.txt
echo FSMOUNT-DONE
" "$WORK/b2.log" 8
assert_clean "$WORK/b2.log" 2
grep -aq "hello-from-boot-one" "$WORK/b2.log" \
    || fail "boot 2: /fsm/a.txt did not survive the reboot"
grep -aq "third" "$WORK/b2.log" \
    || fail "boot 2: /fsm/sub/c.txt did not survive the reboot"

echo "PASS: two real boots with no -snapshot; the kernel's mount-time fsck"
echo "      reported clean on both, and the files written in boot 1 survived"
