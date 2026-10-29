#!/usr/bin/env bash
# THE TWO-PROCESS TEST. Copy in one process, let it EXIT, paste in another.
#
# That sentence is the entire feature. Everything else about a clipboard is a
# detail; a clipboard that cannot cross a process boundary is a variable. So
# this is a test and not a demo, and it is deliberately built so that it cannot
# pass by accident:
#
#   * /bin/clip gen and /bin/clip verify are separate fork+execve'd processes
#     under the serial /bin/sh. The shell waits for each, so the writer is a
#     ZOMBIE THAT HAS BEEN REAPED by the time the reader starts -- its address
#     space is gone (proc_reap -> vmm_free_space). If the clipboard held a
#     pointer into it, this reads freed memory rather than passing.
#   * `clip info` between them prints the owner pid and the reader's own pid.
#     The assertion below requires them to DIFFER. Without that, a clipboard
#     that quietly served the same process twice would pass.
#   * the payload is compared BYTE FOR BYTE, not by length. A store that hands
#     one buffer to two owners returns exactly the right number of somebody
#     else's bytes, and a length check cannot see it.
#   * the payload is generated identically in both processes rather than typed,
#     so the serial console's byte handling is not in the path -- see the header
#     of c/apps/coreutils/clip.c.
#
# It also runs the UTF-8 boundary sweep (`clip trunc`: every short-buffer paste
# of a 4 KiB mixed-width string, each required to end on a character boundary)
# and the cap check (`clip big`: at the cap accepted, one past it REFUSED and
# the previous content untouched), because those are properties of the same
# store and a second boot to check them would cost minutes for nothing.
#
# Usage: run-clip-test.sh <iso> <disk.img>

set -u

ISO="${1:?usage: run-clip-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-clip-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# 4096 bytes is not arbitrary: it is bigger than any stack buffer in the path,
# it is several pages, and the generator's cycle means it contains hundreds of
# 4-byte codepoints -- so a store that was quietly 8-bit clean but not
# multi-byte clean fails on the first character rather than on none of them.
SCRIPT='clip gen 4096
clip info
clip verify 4096
clip trunc
clip big
notify Build "kernel.elf linked" 0
exit
'

{ sleep 4; printf '%s' "$SCRIPT"; sleep 6; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 300); do
    grep -aq "CLIP_BIG_OK" "$LOG" && break
    grep -aq "CLIP_VERIFY_BAD\|CLIP_TRUNC_BAD\|CLIP_BIG_BAD" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fail() { echo "FAIL: $1"; echo "----- serial -----"; cat "$LOG"; echo "------------------"; rm -f "$LOG"; exit 1; }

grep -aq "CLIP_GEN_OK" "$LOG"    || fail "the writing process never put anything on the clipboard"
grep -aq "CLIP_VERIFY_OK" "$LOG" || fail "the reading process did not get the bytes back"
grep -aq "CLIP_TRUNC_OK" "$LOG"  || fail "a short paste split a UTF-8 character"
grep -aq "CLIP_BIG_OK" "$LOG"    || fail "the size cap did not behave"

# The claim, checked rather than assumed: the pid that copied is not the pid
# that pasted. `clip verify` prints both.
VLINE=$(grep -a "CLIP_VERIFY_OK" "$LOG" | tail -1)
WPID=$(grep -a "CLIP_GEN_OK"    "$LOG" | tail -1 | sed -n 's/.*pid \([0-9]*\).*/\1/p')
RPID=$(printf '%s' "$VLINE" | sed -n 's/.*pid \([0-9]*\).*/\1/p')
OWNER=$(printf '%s' "$VLINE" | sed -n 's/.*owner \([0-9]*\).*/\1/p')
BYTES=$(printf '%s' "$VLINE" | sed -n 's/.*bytes \([0-9]*\).*/\1/p')

[ -n "$WPID" ] && [ -n "$RPID" ] || fail "could not read the pids back ($VLINE)"
[ "$WPID" != "$RPID" ] || fail "SAME PROCESS ($WPID) copied and pasted -- this test proved nothing"
[ "$OWNER" = "$WPID" ] || fail "the clipboard's owner is $OWNER, but $WPID did the copy"
[ "${BYTES:-0}" -ge 4000 ] || fail "only $BYTES bytes came back"

TR=$(grep -a "CLIP_TRUNC_OK" "$LOG" | tail -1)
MID=$(printf '%s' "$TR" | sed -n 's/.*midchar \([0-9]*\).*/\1/p')
# The sweep has to have actually hit the case it is about. A run in which no cut
# ever landed mid-character would print CLIP_TRUNC_OK and mean nothing.
[ "${MID:-0}" -gt 1000 ] || fail "only $MID of the cuts landed mid-character -- the sweep tested nothing"

grep -aq "\[wm\] notify post" "$LOG" || fail "a CLI process could not raise a notification"

echo "PASS: process $WPID copied $BYTES bytes, exited; process $RPID pasted them back byte for byte"
echo "      $MID short pastes landed mid-character and none of them split one"
echo "      $(grep -a 'CLIP_BIG cap' "$LOG" | tail -1)"
rm -f "$LOG"
exit 0
