#!/usr/bin/env bash
# WHAT ONE READ-ONLY DESCRIPTOR COSTS, and proof it still reads the file.
#
# c/kernel/exec/file.c's F_VFS backend used to hold the WHOLE file in one
# kmalloc for every open, read-only or not. This gate is the measurement that
# the read-only half no longer does, and it is deliberately made of numbers a
# person can read rather than a pass/fail:
#
#   OPEN-COST  the kheap live_bytes delta across file_open_vfs() and nothing
#              else. It must be 0 at EVERY size -- that is the whole claim, and
#              it is why two files 231x apart are opened rather than one.
#   fnv1a      the same bytes, checksummed. A descriptor that got cheaper by
#              reading fewer bytes is not a smaller descriptor, it is a broken
#              one, and the size alone cannot tell those apart. /fonts/ui.ttf
#              is 2,222,276 B = 543 reads of 4096 through vfs_pread, compared
#              against ONE whole-file vfs_read of the same file: two independent
#              paths through the filesystem, one number.
#
# Both are printed by /dev/fsbench (c/fs/fsbench.c `openfd`), which this line of
# work did not write and does not own -- so the instrument and the thing
# measured are not the same author's opinion.
#
# THE SYNTHETIC NODE IS THE THIRD CASE AND IT IS NOT DECORATION. /dev/vfsmounts
# is RENDERED on demand, and vfs_pread refuses a synthetic node at any non-zero
# offset (VFS_ENOSYS) rather than rendering it twice -- so a description that
# streamed one would serve its first read and return -1 on the second. It must
# therefore still take the buffer. Asserting on the checksum would NOT catch a
# regression there: fsbench's read loop stops at the first non-positive return,
# so for a node smaller than one 4096-byte chunk a -1 second read and a 0 second
# read produce the identical total and the identical checksum.
#
# THE COST IS NOT A CLEAN DISCRIMINATOR EITHER, and the control found that out --
# at 23 bytes the allocation is magazine-sized and a live_bytes delta can read 0
# for a buffered open. The full measurement and the reason are recorded above
# the assertion itself; the short version is that the node's cost is asserted in
# stream mode only, as corroboration, and this gate has no PROOF that the
# exclusion fires -- only evidence. Closing that needs an instrument that can
# report a descriptor's LAST read return, which fsbench does not have and which
# this line of work does not own.
#
# EXPECT=stream (default) demands OPEN-COST 0 for the regular files; EXPECT=slurp
# demands it EQUAL THE FILE SIZE. The second is what run-fdstream-negctl.sh asks
# of a kernel built -DFILE_SLURP, and it is an EQUALITY rather than "non-zero" on
# purpose: "the control reddened" is satisfied by any breakage, while "the fd
# cost exactly 2,222,276 bytes" is satisfied only by the implementation that
# shipped. Both modes demand the checksum MATCH, because the control has to be
# the plausible wrong answer -- a kernel that also stopped reading correctly
# would redden this gate for a reason that is not the one being measured.
#
#   usage: run-fdstream.sh <iso> <disk.img> [mem]
set -u
EXPECT="${EXPECT:-stream}"
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-fdstream.sh <iso> <disk.img> [mem]}"
DISK="${2:?usage: run-fdstream.sh <iso> <disk.img> [mem]}"
MEM="${3:-512}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${LOG:-build/fdstream.log}"
mkdir -p "$(dirname "$LOG")"

# SMALL then LARGE then SYNTHETIC, and each waits for its own DONE line. There
# is no flow control on this serial line, so a burst-fed script loses whatever
# is typed while the guest is busy -- and a lost `openfd` line and a refused one
# look identical in the log.
: > "$LOG"
{
    logit_wait_for_shell "$LOG" 300
    sleep 2
    want=0
    for c in "openfd /fonts/mono.ttf" "openfd /fonts/ui.ttf" "openfd /dev/vfsmounts"; do
        printf 'echo %s > /dev/fsbench\n' "$c"
        want=$((want + 1))
        for _ in $(seq 1 3000); do
            n=$(grep -ac 'OPENFD-DONE' "$LOG" 2>/dev/null); n=${n:-0}
            [ "$n" -ge "$want" ] && break
            sleep 0.1
        done
        sleep 1
    done
    printf 'cat /dev/vfsmounts\n'
    sleep 3
    # LAST, and never before the three above. `openmax` walks kmalloc down from
    # 512 MiB and a kheap arena is PERMANENT -- grow() takes frames from the PMM
    # and never gives them back -- so running it first would move every number
    # after it. It is printed and NOT asserted on: it is the ceiling that USED
    # to be the answer to "the largest file this machine can open", and the
    # point of putting it beside the openfd lines is that it no longer is.
    printf 'echo openmax > /dev/fsbench\n'
    for _ in $(seq 1 900); do
        grep -aq 'OPENMAX-DONE' "$LOG" 2>/dev/null && break
        sleep 0.1
    done
    sleep 2
    printf 'echo FDSTREAM-END\n'
    sleep 3
} | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m "${MEM}M" -smp 4 -accel "${QEMU_ACCEL:-tcg,thread=multi}" \
    -vga none -device virtio-gpu-pci \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!
for _ in $(seq 1 ${WAIT:-6000}); do
    grep -aq 'FDSTREAM-END' "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 2
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "===== fd cost (mem=${MEM}M, TCG -smp 4) ====="
grep -aE "openfd |openmax" "$LOG" | sed "s|^|  |"
echo "============================================="
# READ THE devcmds LINE BEFORE READING THE COST AS A WIN. A streamed read asks
# the device once per 4096-byte chunk, because one chunk IS one block and there
# is nothing for the block layer to coalesce -- where a whole-file vfs_read hands
# inode_pread the entire length and bread_run() collapses it into a handful of
# commands. The memory cost went to zero and the device traffic went up; both
# numbers are printed here rather than only the flattering one. The lever is the
# REQUEST SIZE, not the block layer, which is the readahead line's business.

fail=0
if ! grep -aq 'LogitOS shell' "$LOG"; then
    echo "FAIL: the guest never reached a shell -- this is a BOOT finding, not an fd one"
    tail -40 "$LOG"
    exit 1
fi

# --- the two regular files: cost 0 at both sizes, and the bytes still right ---
for f in /fonts/mono.ttf /fonts/ui.ttf; do
    line=$(grep -a "openfd $f: size=" "$LOG" | head -1)
    if [ -z "$line" ]; then
        echo "  FAIL: $f -- no openfd line at all; the command never ran"
        fail=1; continue
    fi
    cost=$(printf '%s' "$line" | sed -n 's/.*OPEN-COST=\([0-9]*\) B.*/\1/p')
    size=$(printf '%s' "$line" | sed -n 's/.*size=\([0-9]*\).*/\1/p')
    if [ "$EXPECT" = "slurp" ]; then
        # THE FILE PLUS A CONSTANT, not the file exactly. That constant is 12 B
        # and it is kheap's per-block header, which live_bytes counts -- measured
        # here, not assumed: this assertion was written as an equality first and
        # the control reddened at 9,636 -> 9,648 and 2,222,276 -> 2,222,288. The
        # SAME twelve at both sizes is the point, and a stronger statement than
        # "cost >= size" would be: the buffer is the file and one header, so
        # there is no second copy hiding in the number.
        over=$(( ${cost:-0} - ${size:-0} ))
        if [ "$over" -ge 0 ] && [ "$over" -lt 64 ]; then
            echo "  ok[control]: $f -- an fd cost $cost B = the whole file + $over B of kheap header"
            slurp_over="${slurp_over:-} $over"
        else
            echo "  FAIL[control]: $f ($size B) -- an fd cost $cost B, i.e. $over B beside the"
            echo "        file; the control is not the implementation that shipped"
            fail=1
        fi
    elif [ "${cost:-x}" = "0" ]; then
        echo "  ok: $f ($size B) -- an fd costs $cost B of kheap"
    else
        echo "  FAIL: $f ($size B) -- an fd cost $cost B; a read-only description holds no bytes"
        fail=1
    fi
    if grep -aq "openfd $f: read .* fnv1a MATCH" "$LOG"; then
        echo "  ok: $f -- every byte read back through the fd matches one whole-file vfs_read"
    else
        echo "  FAIL: $f -- the streamed read does not match the whole-file read"
        grep -a "openfd $f: read " "$LOG" | sed "s|^|        |"
        fail=1
    fi
done

# --- the synthetic node: it must still take the buffer ------------------------
#
# READ THIS BEFORE READING THE NUMBER. The cost of a 23-byte open is NOT a sound
# discriminator, and the control is what proved it -- this block asserted
# "cost > 0" in both modes and reddened the control on a node the control does
# not change. Measured, DEVICE, one boot each:
#
#     shipped kernel   /dev/vfsmounts  OPEN-COST 80 B, 80 B, 80 B
#                      (three consecutive opens in one boot -- stable)
#     -DFILE_SLURP     /dev/vfsmounts  OPEN-COST 0 B
#
# BOTH kernels take the identical buffered branch for this node: FILE_SLURP
# guards the regular-file branch only, and vfs_streamable() refuses a synthetic
# node at offset 1 in the shipped one. So 80 and 0 are the same code path, and
# the 0 is the allocator, not the file: kmalloc(23) is ALIGN16'd to 32, which is
# an exact per-core magazine class (c/kernel/mm/kheap.c, mag_class runs AFTER
# the align), and a kfree into a magazine deliberately does NOT decrement
# st_live -- the block is still ALLOCATED, which that file argues for at length.
# fsbench's reference read does kmalloc(sz)/kfree immediately before the
# measured open, so whether the open's 32 bytes appear in a live_bytes delta at
# all depends on what ran in between: in the shipped kernel vfs_streamable()'s
# probe does, in the control nothing does.
#
# So: asserted in stream mode, where it is CORROBORATION and is named as such --
# something happens on the synthetic path that does not happen on the streamed
# path, and 0 there would mean the exclusion had stopped firing. NOT asserted in
# slurp mode, because the control cannot answer a question about an allocator
# state it does not produce, and a control that reddens on the wrong row is
# worse than one that is silent on it. What the node IS asked in both modes is
# the row below: that it still reads end to end.
line=$(grep -a "openfd /dev/vfsmounts: size=" "$LOG" | head -1)
if [ -z "$line" ]; then
    echo "  FAIL: /dev/vfsmounts -- no openfd line; the exclusion is UNMEASURED"
    fail=1
else
    cost=$(printf '%s' "$line" | sed -n 's/.*OPEN-COST=\([0-9]*\) B.*/\1/p')
    size=$(printf '%s' "$line" | sed -n 's/.*size=\([0-9]*\).*/\1/p')
    if [ "$EXPECT" = "slurp" ]; then
        echo "  note[control]: /dev/vfsmounts ($size B) cost $cost B -- printed, not asserted;"
        echo "        see the block above for why this number is the allocator's and not the file's"
    elif [ "${cost:-0}" -gt 0 ] 2>/dev/null; then
        echo "  ok: /dev/vfsmounts ($size B) -- a RENDERED node still costs $cost B, i.e. the"
        echo "        streaming branch did not take it (corroboration, not proof -- see above)"
    else
        echo "  FAIL: /dev/vfsmounts ($size B) cost $cost B -- nothing was allocated on a path"
        echo "        that must not stream: if it WAS streamed its second read is VFS_ENOSYS"
        echo "        instead of end of file"
        fail=1
    fi
fi
# and it still reads end to end through the ordinary read path
if grep -aq "^logitfs /" "$LOG"; then
    echo "  ok: cat /dev/vfsmounts still prints the mount table"
else
    echo "  FAIL: cat /dev/vfsmounts printed no mount line"
    fail=1
fi

# NOTHING MAY RUN OUT OF MEMORY WHILE THIS OPENS 2 MiB -- but only up to the
# openmax command, and that cut is not a convenience. `openmax` DELIBERATELY
# walks kmalloc down from 512 MiB and its first probe is MEANT to be refused;
# the "[oom] kmalloc(536870912) refused" it prints is the instrument working,
# not the kernel failing. Judging the whole log reddened this gate on the
# control's own successful measurement, which is how the cut came to be here.
if sed -n '1,/echo openmax/p' "$LOG" | grep -aq '\[oom\]'; then
    echo "  FAIL: an [oom] line appeared before openmax, during a run that opens 2 MiB"
    sed -n '1,/echo openmax/p' "$LOG" | grep -a '\[oom\]' | head -3 | sed 's/^/        /'
    fail=1
fi

[ "$fail" -ne 0 ] && { echo "FAIL: test-fdstream EXPECT=$EXPECT (transcript: $LOG)"; exit 1; }
if [ "$EXPECT" = "slurp" ]; then
    # THE OVERHEAD MUST BE THE SAME AT BOTH SIZES. A per-block header is; a cost
    # that grew with the file by anything other than the file would not be.
    set -- ${slurp_over:-}
    if [ "$#" -eq 2 ] && [ "$1" = "$2" ]; then
        echo "  ok[control]: the same $1 B beside the file at 9,636 B and at 2,222,276 B --"
        echo "        a per-block header, not a second copy"
    else
        echo "  FAIL[control]: the overhead beside the file differs by size:${slurp_over:-} "
        exit 1
    fi
    echo "PASS[control]: with the kmalloc back an fd costs the whole file at both sizes"
    echo "      and still reads back byte-identical"
else
    echo "PASS: a read-only F_VFS descriptor costs 0 B at 9,636 B and at 2,222,276 B,"
    echo "      reads back byte-identical, and a rendered node still keeps its buffer"
fi
exit 0
