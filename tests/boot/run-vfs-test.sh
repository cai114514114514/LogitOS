#!/usr/bin/env bash
# The VFS layer, on the machine.
#
# Three claims, none of which a host test can make:
#
#  1. TWO FILESYSTEMS ON TWO DEVICES. The root filesystem is on the first AHCI
#     controller's disk; a second, independent LogitFS image is on the second
#     controller's disk, mounted read-only at /mnt2 by the lfsro driver. A file
#     is read from each and both contents are asserted. That is what makes the
#     mount table a mount table rather than a struct with one row.
#
#  2. AN UNPRIVILEGED PROCESS IS REFUSED. The shell drops to uid 1000 and then
#     tries to read a 0600 root-owned file. The refusal is asserted by the
#     ABSENCE of the file's contents from the serial log and by the presence of
#     cat's open failure -- not by reading back a mode bit, which proves only
#     that a number was stored. It is paired with a 0644 file that the SAME
#     unprivileged process reads successfully, because a system that refuses
#     everything is not enforcing permissions either, it is just broken.
#
#  3. dup(2) SHARES A FILE OFFSET. Two descriptors onto one open-file
#     description: three bytes through the first, three through the second, and
#     the second must return DEF. An implementation where each descriptor
#     carries its own offset returns ABC and passes every other test in the
#     tree.
#
# Everything is driven through /bin/sh on the serial console, so every request
# comes from a real ring-3 process going through the real syscall path.
#
# Usage: run-vfs-test.sh <iso> <disk.img> <disk2.img>

set -u

ISO="${1:?usage: run-vfs-test.sh <iso> <disk.img> <disk2.img>}"
FSIMG="${2:?usage: run-vfs-test.sh <iso> <disk.img> <disk2.img>}"
IMG2="${3:?usage: run-vfs-test.sh <iso> <disk.img> <disk2.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

LOG="$(mktemp)"
IMG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$IMG"
}
trap cleanup EXIT
cp "$FSIMG" "$IMG"

# Two ich9-ahci controllers, which is how the storage line's own test attaches
# two disks: the root image on ahci0, the second filesystem on ahci1. Root
# selection takes the first disk carrying a superblock, so the boot disk is
# ahci0 and ahci1 is only ever reached by mounting it.
AHCI="-device ich9-ahci,id=ahci0 -drive file=$IMG,format=raw,if=none,id=hd0,file.locking=off -device ide-hd,drive=hd0,bus=ahci0.0"
AHCI="$AHCI -device ich9-ahci,id=ahci1 -drive file=$IMG2,format=raw,if=none,id=hd1,file.locking=off -device ide-hd,drive=hd1,bus=ahci1.0"
NET="-netdev user,id=n0 -device e1000,netdev=n0"
GPU="-vga none -device virtio-gpu-pci"

# The markers never appear in a typed command line: the serial console echoes
# what is typed, so a marker in a command would show up whether or not anything
# worked. They live in files on the second disk and reach the log only by being
# read back out of the filesystem.
SCRIPT='
echo VFSTEST_BEGIN
mkdir /mnt2
echo mount lfsro ahci1 /mnt2 > /dev/vfsctl
cat /dev/vfsctl
cat /dev/vfsmounts
cat /mnt2/disk2.txt
head -n 1 /docs/readme.txt
mkdir /vfst
cat /mnt2/secret.txt > /vfst/secret
cat /mnt2/public.txt > /vfst/public
cat /vfst/secret
cat /vfst/public
echo chmod 600 /vfst/secret > /dev/vfsctl
cat /dev/vfsctl
echo stat /vfst/secret > /dev/vfsctl
cat /dev/vfsctl
cat /mnt2/link.txt > /vfst/a
echo ln /vfst/a /vfst/b > /dev/vfsctl
cat /dev/vfsctl
rm /vfst/a
cat /vfst/b
echo ln -s /vfst/b /vfst/sym > /dev/vfsctl
cat /dev/vfsctl
cat /vfst/sym
echo fdtest /vfst/fdt > /dev/vfsctl
cat /dev/vfsctl
echo VFSTEST_DROP
echo su 1000 > /dev/vfsctl
cat /dev/vfsctl
echo id > /dev/vfsctl
cat /vfst/public
cat /vfst/secret
cat /mnt2/disk2.txt
echo VFSTEST_END
exit
'

{ sleep 4; printf '%s' "$SCRIPT"; sleep 6; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" $AHCI -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi $GPU $NET \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "VFSTEST_END" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fails=0
note() { printf '  %-6s %s\n' "$1" "$2"; }
want() {   # want <name> <regex>
    if grep -aq "$2" "$LOG"; then note "ok" "$1"; else note "FAIL" "$1"; fails=$((fails + 1)); fi
}
wantn() {  # wantn <name> <regex> <count> -- an EXACT number of occurrences
    n=$(grep -ac "$2" "$LOG" 2>/dev/null || true)
    [ -z "$n" ] && n=0
    if [ "$n" = "$3" ]; then note "ok" "$1 (x$n)"
    else note "FAIL" "$1 (saw $n, want $3)"; fails=$((fails + 1)); fi
}

echo "--- VFS on device"
want  "the run completed"                       "VFSTEST_END"

echo "1. two filesystems on two devices"
want  "the second disk was found by the driver" "\[lfsro\] mounted ahci1 read-only"
want  "and mounted at /mnt2"                    "ok mounted lfsro ahci1 at /mnt2"
want  "the mount table lists the root"          "logitfs /"
want  "  ... and the second filesystem"         "ahci1 /mnt2"
want  "a file read from the SECOND device"      "VFS_DISK2_OK"
want  "a file read from the FIRST device"       "LogitOS - LogitFS volume"

echo "2. links"
want  "hard link reports nlink 2"               "ok link /vfst/b nlink 2"
want  "content survives unlinking the first name" "VFS_LINK_BYTES"
want  "symlink created"                         "ok symlink /vfst/sym"
want  "stat reports the mode that was set"      "ok mode 0600 uid 0 gid 0 nlink 1 reg"

echo "3. the fd layer"
want  "dup shares the offset"                   "ok fdtest first=ABC second=DEF OFFSET-SHARED"
want  "the description survives one close"      "DESCRIPTION-SURVIVED-CLOSE"

echo "4. an unprivileged process is refused"
want  "the shell dropped to uid 1000"           "ok pid [0-9]* uid 1000 gid 1000"
want  "and cat was refused the 0600 file"       "cat: cannot open /vfst/secret"
# The whole point: the bytes of the 0600 file reached the log exactly ONCE --
# the privileged read before the drop -- and never after it.
wantn "the 0600 file's CONTENT appears once"    "VFS_SECRET_BYTES" 1
# ... while the 0644 file was read twice, before AND after the drop, by the
# same process. A refusal of everything would satisfy the line above too.
wantn "the 0644 file's content appears twice"   "VFS_PUBLIC_BYTES" 2
want  "an unprivileged write to a root node is refused" "sh: cannot open output"

if [ "$fails" = 0 ]; then
    echo "PASS: mount table, permissions enforced against ring 3, links, shared offsets"
    exit 0
fi
echo "FAIL: $fails assertion(s)"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
