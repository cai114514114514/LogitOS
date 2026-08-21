#!/usr/bin/env bash
# ON THE MACHINE: make a real ring-3 program fault, and then read the core dump
# the kernel wrote for it -- with gdb, on the host, out of the real disk image.
#
# The host gate (make test-coredump) drives c/kernel/exec/coredump.c's builder
# against a MODELLED address space. This is the other half: the region list
# comes from c/kernel/mm/vma.c, "is this page resident" comes from a real page
# table, the bytes come from a real process, and the file goes through LogitFS
# onto a real disk -- from inside a page-fault handler, with the BKL held.
# None of that is exercised by the host gate and all of it can fail on its own.
#
# NO -snapshot, AND THAT IS THE POINT. Every other .as harness in tests/boot
# runs with it; this one has to keep the write. It works on a COPY of the disk
# so a persistent write cannot leak into the image the other harnesses share --
# c/fs's own durability tests make the same distinction.
#
# THE TWO CHANNELS THE GATE COMPARES:
#   [fault] ... rip=... err=... cr2=... rsp=...   the TRAP FRAME, printed by
#                                                 c/kernel/cpu/interrupts.c
#   the FILE, parsed by tests/unit/corecheck.c (c/apps/coreutils/corefmt.h, the
#   same parser /bin/readcore uses) and independently by gdb.
# Plus cr2, which was written down in fsroot/as/examples/crashme.as before the
# machine booted, so it is an oracle rather than a second reading.
#
# THE [core] LINE IS ITSELF A SECOND PATH, before the host sees anything: the
# kernel quotes rip and rsp by READING THEM BACK out of the bytes it just
# wrote (coredump_read_gregs), not by reprinting the trap frame. So a register
# note that was dropped or laid out wrong shows up on the serial log of a live
# machine, not only under a host test.

set -u

ISO="${1:?usage: run-core-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-core-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
CORECHECK="${CORECHECK:-build/corecheck}"
LOG="$(mktemp)"
WORKDISK="$(mktemp -u).img"
DUMP="$(mktemp -u).core"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$WORKDISK" "$DUMP"
}
trap cleanup EXIT

[ -x "$CORECHECK" ] || { echo "FAIL: $CORECHECK is not built"; exit 1; }
cp "$DISK" "$WORKDISK"

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# `sync` after the crash is not available as a coreutil, so the shell runs one
# more file operation and then poweroff, which unmounts. LogitFS commits and
# checkpoints per write (see the barrier comment above log_commit in
# c/fs/logitfs.c), so the dump is on media before this, but a clean shutdown is
# what makes that a property of the test rather than of the timing.
{ sleep 5
  printf 'as /usr/as/examples/crashme.as\n'
  sleep 6
  printf 'ls /\n'
  sleep 2
  printf 'poweroff\n'
  sleep 4
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$WORKDISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d \
      -m 512M -smp 2 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

ok=0
for _ in $(seq 1 400); do
    if grep -aq '\[core\] pid' "$LOG" && grep -aq '\[fault\] app exception' "$LOG"; then
        ok=1
        break
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
# Let the shutdown finish so the filesystem is quiescent before the image is read.
for _ in $(seq 1 200); do kill -0 "$QPID" 2>/dev/null || break; sleep 0.1; done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "----- serial (the two lines that matter) -----"
grep -a 'CRASHME\|\[fault\]\|\[core\]' "$LOG" || true
echo "----------------------------------------------"

if grep -aq 'CRASHME DID NOT FAULT' "$LOG"; then
    echo "FAIL: the fixture did not fault -- 0xdeadbee0 was writable, so nothing"
    echo "      here measures a core dump. Fix the fixture, not the kernel."
    exit 1
fi
if [ "$ok" -ne 1 ]; then
    echo "FAIL: no [core] line on the serial log"
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
fi

FAULT="$(grep -a '\[fault\] app exception' "$LOG" | head -1)"
COREL="$(grep -a '\[core\] pid' "$LOG" | head -1)"
f_rip=$(echo "$FAULT" | sed -n 's/.*rip=\(0x[0-9a-fA-F]*\).*/\1/p')
f_err=$(echo "$FAULT" | sed -n 's/.*err=\([0-9a-fA-F]*\) .*/0x\1/p')
f_cr2=$(echo "$FAULT" | sed -n 's/.*cr2=\(0x[0-9a-fA-F]*\).*/\1/p')
f_rsp=$(echo "$FAULT" | sed -n 's/.*rsp=\(0x[0-9a-fA-F]*\).*/\1/p')
c_rip=$(echo "$COREL" | sed -n 's/.*rip=\(0x[0-9a-fA-F]*\).*/\1/p')
c_rsp=$(echo "$COREL" | sed -n 's/.*rsp=\(0x[0-9a-fA-F]*\).*/\1/p')
SLOT="$(echo "$COREL" | sed -n 's|.*-> \(/core\.[0-9]\).*|\1|p')"

if [ -z "$f_rip" ] || [ -z "$f_cr2" ] || [ -z "$SLOT" ]; then
    echo "FAIL: could not read the numbers off the serial lines above"
    exit 1
fi
echo "fault frame: rip=$f_rip rsp=$f_rsp err=$f_err cr2=$f_cr2"
echo "core line  : rip=$c_rip rsp=$c_rsp slot=$SLOT"

# The kernel read those two back OUT OF THE FILE. If they differ from the trap
# frame, the register note is wrong and the host half below would be checking a
# file against itself.
[ "$f_rip" = "$c_rip" ] || { echo "FAIL: [core] rip $c_rip != [fault] rip $f_rip"; exit 1; }
[ "$f_rsp" = "$c_rsp" ] || { echo "FAIL: [core] rsp $c_rsp != [fault] rsp $f_rsp"; exit 1; }
echo "ok  : the kernel's own read-back of the file agrees with the trap frame"

python3 tests/boot/lfs_extract.py "$WORKDISK" "$SLOT" "$DUMP" || {
    echo "FAIL: $SLOT is not on the disk -- the kernel said it wrote it"
    echo "----- root directory of the image -----"
    python3 tests/boot/lfs_extract.py "$WORKDISK" --ls || true
    exit 1
}

# 0xdeadbee0 is the address written down in fsroot/as/examples/crashme.as before
# the machine booted. err is 0x6 by construction: a WRITE (bit 1) from USER
# (bit 2) to a page that is NOT PRESENT (bit 0 clear).
"$CORECHECK" "$DUMP" \
    cr2=0xdeadbee0 siaddr=0xdeadbee0 signo=11 trapno=0xe \
    err="$f_err" rip="$f_rip" rsp="$f_rsp" || {
    echo "FAIL: the dump does not agree with the kernel's own [fault] line"
    exit 1
}

echo "PASS: a ring-3 fault produced $SLOT, and its register file, fault address"
echo "      and error code agree with the trap frame, with the address the"
echo "      fixture chose before boot, and with gdb"
exit 0
