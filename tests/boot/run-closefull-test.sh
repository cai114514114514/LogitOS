#!/usr/bin/env bash
# THE GATE for "make close() able to report a failed write" (audit item 3;
# CLAUDE.md structural gap #3's tail: "the only symptom of a failed write is
# that the file quietly is not there").
#
# WHY THIS BOOTS QEMU rather than being a host test: file.c's flush lives at
# the last close of a REAL fd, over the REAL VFS, over a REAL LogitFS mount --
# reproducing that on the host would mean stubbing kmalloc, the scheduler, the
# BKL and the console (see tests/fdstream.mk's own "WHY NOT A HOST TEST" for
# the same call about the same file). What IS avoided is writing ~510 MB
# through the block layer to actually fill a 512 MiB image: tests/boot/
# mkdiskfull.py edits the persisted free-block BITMAP directly (the same bytes
# balloc()/bit_test() in c/fs/logitfs.c read), leaving a handful of blocks
# free and every existing file on the image untouched and byte-identical.
#
# /bin/closefull (tests/unit/closefull_main.c) then:
#   1. writes a SMALL file, well inside what was left free, and demands
#      close() return 0 -- the control INSIDE the gate. Without it, "close()
#      reported a failure" cannot be told apart from "close() now always
#      reports a failure", which is not the claim.
#   2. writes a BIG file, bigger than what was left free, and demands
#      close() return NONZERO -- the actual claim.
#
# EXPECT=ok (default) demands exactly that. EXPECT=broken is what
# run-closefull-negctl.sh asks of a kernel built -DFILE_CLOSE_ALWAYS_OK: the
# write still fails on disk (the file is short or absent either way -- this
# script does not re-check that, mkdiskfull.py's arithmetic already proves the
# blocks are not there), but CLOSETEST-BIG's rc line must read 0 instead of
# nonzero, because ONLY whether close() SAYS SO is what that build changes.
#
#   usage: run-closefull-test.sh <iso> <disk.img> [mem] [keep_free_blocks]
set -u
EXPECT="${EXPECT:-ok}"
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-closefull-test.sh <iso> <disk.img> [mem] [keep_free_blocks]}"
FSIMG="${2:?usage: run-closefull-test.sh <iso> <disk.img> [mem] [keep_free_blocks]}"
MEM="${3:-512}"
KEEP_FREE="${4:-2}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${LOG:-build/closefull.log}"
mkdir -p "$(dirname "$LOG")"

DISKFULL="${DISKFULL:-$(dirname "$LOG")/diskfull.img}"
python3 "$(dirname "$0")/mkdiskfull.py" "$FSIMG" "$DISKFULL" "$KEEP_FREE" \
    || { echo "FAIL: mkdiskfull.py could not craft a near-full image from $FSIMG"; exit 1; }

: > "$LOG"
{
    logit_wait_for_shell "$LOG" 300
    sleep 2
    printf 'closefull\n'
    for _ in $(seq 1 900); do
        grep -aq 'CLOSETEST-DONE' "$LOG" 2>/dev/null && break
        sleep 0.1
    done
    sleep 1
    printf 'echo CLOSEFULL-END\n'
    sleep 3
} | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISKFULL",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m "${MEM}M" -smp 4 -accel "${QEMU_ACCEL:-tcg,thread=multi}" \
    -vga none -device virtio-gpu-pci \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!
for _ in $(seq 1 ${WAIT:-3000}); do
    grep -aq 'CLOSEFULL-END' "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 1
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "===== close() on a near-full disk (keep_free=$KEEP_FREE blocks, EXPECT=$EXPECT) ====="
grep -aE "CLOSETEST-(SMALL|BIG)" "$LOG" | sed "s|^|  |"
echo "======================================================================================"

SMALL_LINE=$(grep -aE "^CLOSETEST-SMALL " "$LOG" | tail -1)
BIG_LINE=$(grep -aE "^CLOSETEST-BIG " "$LOG" | tail -1)
[ -n "$SMALL_LINE" ] || { echo "FAIL: no CLOSETEST-SMALL line -- did the shell run closefull at all?"; exit 1; }
[ -n "$BIG_LINE" ]   || { echo "FAIL: no CLOSETEST-BIG line -- did the shell run closefull at all?"; exit 1; }

small_rc=$(printf '%s' "$SMALL_LINE" | sed -E -n 's/.*rc=(-?[0-9]+).*/\1/p')
big_rc=$(printf '%s' "$BIG_LINE"   | sed -E -n 's/.*rc=(-?[0-9]+).*/\1/p')
[ -n "$small_rc" ] && [ -n "$big_rc" ] || { echo "FAIL: could not parse an rc= field"; exit 1; }

# The control INSIDE the gate, checked regardless of EXPECT: a write that fits
# in what mkdiskfull.py left free must still succeed, in EITHER kernel -- if
# this ever fails, the near-full image was built wrong (too little kept free),
# not the close() path this gate is about.
if [ "$small_rc" != "0" ]; then
    echo "FAIL: CLOSETEST-SMALL rc=$small_rc, want 0 -- the small write should fit in the"
    echo "      blocks mkdiskfull.py left free; this is the apparatus, not the fix"
    exit 1
fi

case "$EXPECT" in
    ok)
        if [ "$big_rc" = "0" ]; then
            echo "FAIL: CLOSETEST-BIG rc=0 -- close() reported success for a write that could"
            echo "      not have fit in $KEEP_FREE free block(s); the fix did not reach SYS_CLOSE"
            exit 1
        fi
        echo "PASS: close() on a full disk returns nonzero (rc=$big_rc) -- the caller can tell"
        ;;
    broken)
        if [ "$big_rc" != "0" ]; then
            echo "FAIL: CLOSETEST-BIG rc=$big_rc under -DFILE_CLOSE_ALWAYS_OK -- the control"
            echo "      should have restored the pre-fix \"always 0\" behaviour and did not"
            exit 1
        fi
        echo "negative control ok -- with file_close() built -DFILE_CLOSE_ALWAYS_OK, a write"
        echo "      that ran out of disk still reports rc=0"
        ;;
    *)
        echo "FAIL: unknown EXPECT=$EXPECT"; exit 1 ;;
esac
exit 0
