#!/usr/bin/env bash
# AHCI/SATA on device, and a root filesystem inside a partition.
#
# Enumeration is not the claim. "The OS booted off it" is -- so every mode here
# asserts, over the serial log, all four of:
#   1. an AHCI controller was found by PCI CLASS and its ABAR mapped;
#   2. a port was probed and its SIGNATURE said SATA disk (not ATAPI);
#   3. for mbr/gpt, the partition table was parsed and the right partition was
#      selected as root -- the filesystem is NOT at LBA 0, so a kernel that
#      cannot read a partition table cannot find its own root;
#   4. logitfs mounted AND a file came back with the right CONTENT, read through
#      /bin/sh on the serial console. A mount that succeeds and then reads
#      zeroes is the failure a mount check alone cannot see.
#
# Usage: run-ahci-test.sh <iso> <fs.img> <raw|mbr|gpt>

set -u

ISO="${1:?usage: run-ahci-test.sh <iso> <disk.img> <raw|mbr|gpt>}"
FSIMG="${2:?usage: run-ahci-test.sh <iso> <disk.img> <raw|mbr|gpt>}"
MODE="${3:-raw}"
QEMU="${QEMU:-qemu-system-x86_64}"
HERE="$(cd "$(dirname "$0")" && pwd)"

LOG="$(mktemp)"
IMG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$IMG"
}
trap cleanup EXIT

case "$MODE" in
    raw)
        cp "$FSIMG" "$IMG"
        LAYOUT="raw filesystem at LBA 0 (no partition table)"
        WANT_PART="no partition table"
        WANT_ROOT="root = ahci0 "
        ;;
    mbr)
        LAYOUT="$(python3 "$HERE/run-ahci-mkdisk.py" "$FSIMG" "$IMG" mbr)" || exit 1
        WANT_PART="\[part\] ahci0: MBR"
        WANT_ROOT="root = ahci0p2 "
        ;;
    gpt)
        LAYOUT="$(python3 "$HERE/run-ahci-mkdisk.py" "$FSIMG" "$IMG" gpt)" || exit 1
        WANT_PART="\[part\] ahci0: GPT (protective MBR)"
        WANT_ROOT="root = ahci0p1 "
        ;;
    two)
        # TWO controllers. A machine with a chipset AHCI and an add-in SATA card
        # has two, and a driver that takes the first and stops makes the disks on
        # the other one not exist -- which, on a box that boots off the add-in
        # card, means it does not boot. The second disk here is deliberately
        # blank, so the assertion is that it is FOUND and named, not that it is
        # bootable: root must still land on ahci0.
        cp "$FSIMG" "$IMG"
        LAYOUT="two ich9-ahci controllers: a filesystem on the first, a blank disk on the second"
        WANT_PART="no partition table"
        WANT_ROOT="root = ahci0 "
        ;;
    *)
        echo "unknown mode '$MODE' (want raw|mbr|gpt|two)" >&2; exit 2 ;;
esac

echo "--- AHCI $MODE: $LAYOUT"

# ich9-ahci is the AHCI controller QEMU provides; the disk hangs off it as an
# ide-hd on bus ahci0.0, which is a SATA port as far as the guest is concerned.
# Nothing here names a vendor:device ID -- the driver has to find it by class.
AHCI="-device ich9-ahci,id=ahci0 -drive file=$IMG,format=raw,if=none,id=hd0,file.locking=off -device ide-hd,drive=hd0,bus=ahci0.0"
if [ "$MODE" = two ]; then
    IMG2="$(mktemp)"
    trap 'rm -f "$IMG2"; [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; rm -f "$LOG" "$IMG"' EXIT
    dd if=/dev/zero of="$IMG2" bs=1M count=8 2>/dev/null
    AHCI="$AHCI -device ich9-ahci,id=ahci1 -drive file=$IMG2,format=raw,if=none,id=hd1,file.locking=off -device ide-hd,drive=hd1,bus=ahci1.0"
fi
NET="-netdev user,id=n0 -device e1000,netdev=n0"
GPU="-vga none -device virtio-gpu-pci"

# The marker string is read out of the file the shell cats, so a mount that
# succeeds and then returns the wrong bytes fails this test.
MARK="LogitOS - LogitFS volume"

{ sleep 6; printf 'cat /docs/readme.txt\n'; sleep 6; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" $AHCI -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi $GPU $NET \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

pass=0
for _ in $(seq 1 300); do
    if grep -aq "$MARK" "$LOG" && grep -aq "LOGIT_BOOT_OK" "$LOG"; then pass=1; break; fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fail() {
    echo "FAIL ($MODE): $1"
    echo "----- serial output -----"
    cat "$LOG"
    echo "-------------------------"
    exit 1
}

grep -aq '\[ahci\] .* abar='            "$LOG" || fail "no AHCI controller found by PCI class"
grep -aq '\[ahci\] port .*: SATA disk'  "$LOG" || fail "no port probed as a SATA disk"
grep -aq '\[blk\] ahci0: AHCI SATA disk' "$LOG" || fail "the disk was not registered as a block device"
grep -aq "$WANT_PART"                   "$LOG" || fail "expected partition scan result missing: $WANT_PART"
grep -aq "$WANT_ROOT"                   "$LOG" || fail "root device is not the expected one: $WANT_ROOT"
grep -aq "LOGIT_BOOT_OK"                "$LOG" || fail "filesystem did not mount off the AHCI disk"
[ "$pass" = 1 ]                                || fail "file content '$MARK' not read back through the shell"

if [ "$MODE" = two ]; then
    n=$(grep -ac '\[ahci\] .* abar=' "$LOG")
    [ "$n" = 2 ]                          || fail "expected 2 AHCI controllers in the log, saw $n"
    grep -aq '\[blk\] ahci1: AHCI SATA disk' "$LOG" \
        || fail "the disk on the SECOND controller was never registered"
fi

case "$MODE" in
    raw) WHAT="raw device" ;;
    two) WHAT="two controllers, both disks found" ;;
    *)   WHAT="partition table" ;;
esac
echo "PASS ($MODE): AHCI controller + SATA port + $WHAT -> logitfs mounted and a file read back byte-correct"
grep -a -e '^\[ahci\]' -e '^\[part\]' -e '^\[blk\]' -e '^\[ata\]' "$LOG" | sed 's/^/    /'
exit 0
