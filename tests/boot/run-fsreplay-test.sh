#!/usr/bin/env bash
# Deterministic log_recover test -- no kill -9 timing luck involved.
#
# The random crash harness (run-fscrash-test.sh) proves the journal never lets
# a torn write through, but it almost never witnesses a replay: the seal->clear
# window is milliseconds wide, so virtually every kill lands either before the
# seal (nothing to replay) or after the clear (nothing to replay). This harness
# instead CRAFTS the on-disk state a crash in that window leaves behind (a
# sealed log header, one body block, target untouched), boots on it, and then
# asserts -- on the host, byte for byte -- that mounting the filesystem
# installed the logged block and cleared the header.
#
# Victim: /docs/readme.txt (170 bytes, a single data block, ships in every
# image). The sentinel replaces its whole content, so any install error shows
# up as wrong bytes, not a shifted file.

set -u

ISO="${1:?usage: run-fsreplay-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-fsreplay-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
VICTIM=/docs/readme.txt
SENTINEL="LOGIT-REPLAY-SENTINEL: this block came through the write-ahead log"

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

fail() { echo "FAIL: $1"; exit 1; }

# 1) craft the crashed-mid-transaction image
python3 "$HERE/mkreplay.py" craft "$DISK" "$DISKC" "$VICTIM" "$SENTINEL" \
    || fail "could not craft the image"

# 2) boot on it; recovery happens at mount, long before the shell
NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 5; printf 'echo BOOTMARK-DONE\n'; sleep 600; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
    -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$WORK/boot.log" 2>/dev/null &
QPID=$!
waited=0
while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 120 ]; do
    grep -aq "BOOTMARK-DONE" "$WORK/boot.log" 2>/dev/null && break
    sleep 1; waited=$((waited + 1))
done
sleep 3
# Never `wait` a SIGKILLed pipeline member under WSL (do_wait wedges); bounded poll.
kill -9 "$QPID" 2>/dev/null
for _ in $(seq 1 100); do
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
QPID=""
tr -d '\r' <"$WORK/boot.log" >"$WORK/boot.log.n" && mv "$WORK/boot.log.n" "$WORK/boot.log"

grep -aq "BOOTMARK-DONE" "$WORK/boot.log" || {
    grep -aE "\[fs\]|panic|fault" "$WORK/boot.log" | tail -10
    fail "the system did not come up on the crafted image"
}
grep -aq "\[fs\] log: replayed 1 block(s)" "$WORK/boot.log" \
    || fail "mount did not report replaying the crafted transaction"
grep -aq "\[fs\] mounted" "$WORK/boot.log" \
    || fail "filesystem did not mount after replay"

# 3) the real assertion, on the host: installed body + cleared header
python3 "$HERE/mkreplay.py" check "$DISKC" "$VICTIM" "$SENTINEL" \
    || fail "host-side image check failed"

echo "PASS: a sealed-but-not-installed transaction was replayed at mount --"
echo "      logged block installed byte-for-byte, log header cleared"
