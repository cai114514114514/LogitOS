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

# ---- what every later boot must see ----------------------------------------
# The WHOLE line, not just the mode. An implementation that got the mode right
# and the owner wrong, or that kept reporting 0741 after losing the "this is
# stored on the medium" bit, is broken in a way a mode check cannot see. attr
# must carry 0x3 = LSTA_MODE_STORED | LSTA_MODE_DURABLE.
READ="stat /meta/f.txt /meta/d /meta/locked.txt /meta/bystander.txt
"

verify_all() {   # $1 = log, $2 = which boot
    local L="$1" N="$2"
    grep -aq "STAT /meta/f.txt mode=741 type=reg uid=7 gid=9 " "$L" ||
        fail "boot $N: /meta/f.txt lost its mode or owner (want mode=741 uid=7 gid=9)"
    grep -aq "STAT /meta/d mode=700 type=dir " "$L" ||
        fail "boot $N: /meta/d lost its directory mode (want mode=700)"
    # chmod 000 is the trap case: if "unset" were encoded as a zero mode, a file
    # its owner deliberately locked comes back 0644 -- silently readable by
    # everyone. The on-disk presence bit is what keeps them apart.
    grep -aq "STAT /meta/locked.txt mode=0 type=reg " "$L" ||
        fail "boot $N: chmod 000 came back as something else -- 0 is a mode, not 'unset'"
    # And the bystander: still the DEFAULT, and still SAYING it is a default.
    grep -aq "STAT /meta/bystander.txt mode=644 type=reg uid=0 gid=0 " "$L" ||
        fail "boot $N: an untouched file stopped reporting the 0644 default"

    # attr: bit 0 STORED, bit 1 DURABLE. A file whose mode was set must have
    # both; the untouched bystander must have NEITHER, because its 0644 is a
    # default nobody chose and a caller has to be able to tell.
    local A
    A="$(grep -a "STAT /meta/f.txt mode=" "$L" | tail -1 | sed 's/.*attr=//')"
    case "$A" in
        0x3|0x7|0xb|0xf|0x13|0x17|0x1b|0x1f) : ;;
        *) fail "boot $N: /meta/f.txt attr=$A lacks STORED|DURABLE (0x3) -- the mode is right but nothing says it is stored" ;;
    esac
    A="$(grep -a "STAT /meta/bystander.txt mode=" "$L" | tail -1 | sed 's/.*attr=//')"
    case "$A" in
        *0|*2|*4|*6|*8|*a|*c|*e) : ;;   # bit 0 clear: not STORED
        *) fail "boot $N: the untouched bystander claims attr=$A (STORED set) -- a default is being reported as a choice" ;;
    esac
}

# ---- boot 2: the assertion the RAM store could never have passed ------------
boot "${READ}echo BOOT2-DONE
" "$WORK/b2.log" 8
grep -aq "BOOT2-DONE" "$WORK/b2.log" || fail "boot 2 never finished its commands"
verify_all "$WORK/b2.log" "2 (the first reboot -- this is the whole claim)"

# ---- boot 3: churn, then read again ----------------------------------------
# A metadata store that survives a quiet reboot but not an ALLOCATING one is a
# real failure mode, and it never shows on the files being churned -- it shows
# on the bystanders, which is what /meta holds.
boot "echo c1 > /meta/churn1.txt
echo c2 > /meta/churn2.txt
rm /meta/churn1.txt
echo c3 > /meta/churn3.txt
rm /meta/churn2.txt
${READ}echo BOOT3-DONE
" "$WORK/b3.log" 10
grep -aq "BOOT3-DONE" "$WORK/b3.log" || fail "boot 3 never finished its commands"
verify_all "$WORK/b3.log" "3 (after a round of allocate/free churn)"

# ---- boot 4: one more time -------------------------------------------------
boot "${READ}echo BOOT4-DONE
" "$WORK/b4.log" 8
grep -aq "BOOT4-DONE" "$WORK/b4.log" || fail "boot 4 never finished its commands"
verify_all "$WORK/b4.log" "4 (after two reboots and a churn)"

echo "PASS: mode, owner, a directory mode and chmod 000 all survived 4 real boots"
echo "      with allocate/free churn between them, and the untouched bystander"
echo "      still reports its 0644 as a DEFAULT rather than as a choice"
