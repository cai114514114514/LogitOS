#!/usr/bin/env bash
# MEASURE THE ELF FILE-BACKED-TEXT GATE, on the real machine, from ring 3.
#
# Three numbers, and one boot produces all three so that they describe the same
# machine in the same state:
#
#   G1 FRAMES.  Start three copies of the SAME 327 KiB binary and keep them
#               alive, reading the free-frame count between each. The claim is
#               that copy 2 and copy 3 cost only their PRIVATE pages, because
#               the whole pages of R and R E that elf_file_runs() found are one
#               set of frames in the page cache rather than three.
#               The steps A1->A2->A3 are the measurement; A0->A1 is not, because
#               that one also pays for whatever the first load warms.
#
#   G2 RECLAIM. reclaim's tier-1 SECOND producer (try_drop_cached,
#               c/kernel/mm/reclaim.c:281) has never fired in this tree, because
#               until now nothing produced a VMM_PTE_FILE page -- SYS_MMAP_FILE's
#               only caller is a one-page script written to exercise it. Forced
#               here with MMCTL_RECLAIM so that a zero is a statement about the
#               MECHANISM. Under natural load a zero would only be a statement
#               about a desktop that peaks at 229 MiB of 511 and never reaches
#               the watermark, which is a different claim entirely.
#
#   G3 EXEC.    [exec] N execs, M kcycles each: L in aex_load. Read at the same N
#               in both builds. NOTE WHAT L STILL CONTAINS: exec.c kmallocs and
#               vfs_reads the WHOLE file, and aex.c CRC-32s the whole ELF image,
#               both before a single page is mapped. Sharing removes the per-page
#               memcpy and nothing else, so this number is expected to move very
#               little -- and that is a finding about where an exec's time goes,
#               not a failed measurement.
#
# Plus R5: [pcache] peak P of 4096 slots, the 16 MiB system-wide ceiling past
# which pcache.c:546 still SUCCEEDS and hands the fault back UNCACHED.
#
# THE HARNESS RUNS UNCHANGED ON A KERNEL WITHOUT THE FEATURE, which is what
# makes it a measurement rather than a demonstration: nothing below requires the
# "[exec] load" line, which only the new loader prints, and nothing below
# requires any count to be nonzero. Point it at an old ISO and it prints the old
# machine's numbers in the same shape.
#
# Usage:  bash tests/boot/run-elfshare.sh <iso> <disk.img> [label]
# Makefile equivalent (NOT wired -- the Makefile is owned by another line):
#     test-elfshare: $(ISO) $(DISK)
#         @bash tests/boot/run-elfshare.sh $(ISO) $(DISK)
set -u

. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-elfshare.sh <iso> <disk.img> [label]}"
DISK="${2:?usage: run-elfshare.sh <iso> <disk.img> [label]}"
LABEL="${3:-run}"
BATCH="${ELFSHARE_EXECBATCH:-16}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${ELFSHARE_LOG:-$(mktemp)}"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

AS=/bin/as
EX=/usr/as/examples

# Wait for a line to appear in the log rather than sleeping a guessed number of
# seconds. "the subject had not finished loading yet" and "the sharing did not
# happen" produce the SAME free-frame reading, and only one of them is a
# finding -- so every step below is gated on the previous step's own output.
w() { logit_wait_for_marker "$LOG" "$1" "${2:-120}"; }

{
  logit_wait_for_shell "$LOG" 200

  # --- G1 ---------------------------------------------------------------
  # A0: nobody but the measurer. Exactly one copy of /bin/as is resident while
  # this line is printed, and that stays true of A1..A3 -- see elfstat.as.
  printf '%s %s/elfstat.as A0\n' "$AS" "$EX";        w "ELFSTAT-END A0"
  printf '%s %s/elfspin.as s1 &\n' "$AS" "$EX";      w "ELFSPIN-UP s1"
  printf '%s %s/elfstat.as A1\n' "$AS" "$EX";        w "ELFSTAT-END A1"
  printf '%s %s/elfspin.as s2 &\n' "$AS" "$EX";      w "ELFSPIN-UP s2"
  printf '%s %s/elfstat.as A2\n' "$AS" "$EX";        w "ELFSTAT-END A2"
  printf '%s %s/elfspin.as s3 &\n' "$AS" "$EX";      w "ELFSPIN-UP s3"
  printf '%s %s/elfstat.as A3\n' "$AS" "$EX";        w "ELFSTAT-END A3"

  # --- G2: force a pass, with the three subjects still resident ---------
  printf '%s %s/elfstat.as R1 256\n' "$AS" "$EX";    w "ELFSTAT-END R1"
  printf '%s %s/elfstat.as R2 1024\n' "$AS" "$EX";   w "ELFSTAT-END R2"

  # --- G3: repeat exec of one binary, warm ------------------------------
  # /bin/as and not /bin/true: true.aex has no whole page of R E to share, so a
  # batch of it would measure the loader on the one shape the feature cannot
  # apply to. exec.c's report is CUMULATIVE, so the batch is a fixed size and
  # the marginal cost is recovered from two consecutive reports.
  #
  # THE BATCH SIZE IS A KNOB BECAUSE THE CYCLE COUNT IS NOISY AND THE FRAME
  # COUNT IS NOT. exec.c reports every 8th exec, so ELFSHARE_EXECBATCH=48 gives
  # six marginals from ONE boot instead of two -- which is the only way to tell
  # "this build is faster" from "the host was quieter during that run". The
  # default stays 16 so a plain invocation reproduces the earlier runs exactly.
  i=0
  while [ $i -lt "$BATCH" ]; do printf '%s %s/hello.as\n' "$AS" "$EX"; i=$((i+1)); done
  printf 'echo ELFSHARE_EXECBATCH\n';                w "ELFSHARE_EXECBATCH" 600

  # A final report AFTER the batch: pcache peak over the whole session.
  printf '%s %s/elfstat.as FINAL\n' "$AS" "$EX";     w "ELFSTAT-END FINAL"
  printf 'echo ELFSHARE_DONE\n'; sleep 3
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      -netdev user,id=n0 -device e1000,netdev=n0 \
      -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 9000); do
    grep -aq "ELFSHARE_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "===================== elfshare: $LABEL ====================="
echo "log: $LOG"

# Did the machine do what the harness asked? Printed BEFORE any number, because
# a free-frame series taken with two subjects instead of three is not a wrong
# number, it is a number about a different experiment.
UP=$(grep -ac "ELFSPIN-UP" "$LOG")
STATS=$(grep -ac "ELFSTAT-END" "$LOG")
echo "-- control: $UP subject(s) came up, $STATS measurement(s) completed (want 3 and 7)"
grep -a "ELFSTAT-AUDIT" "$LOG" | sed 's/^/   /'

echo
echo "-- G1: free frames at each named moment (from [mmstat], 4 KiB/frame)"
awk '
  /ELFSTAT-BEGIN/ { tag = $2 }
  /\[mmstat\]/ {
      free = "";
      for (i = 1; i <= NF; i++) if ($i ~ /^free=/) free = substr($i, 6);
      if (tag != "" && !seen[tag]++) {
          printf "   %-6s free=%-8s", tag, free;
          if (prev != "") printf "  delta=%+d frames (%+d KiB)", free - prev, (free - prev) * 4;
          printf "\n";
          prev = free;
      }
  }
' "$LOG"

echo
echo "-- G1 corroboration: what the loader said it did, per image"
grep -a "\[exec\] load" "$LOG" | sed 's/.*\[exec\]/   [exec]/' | sort | uniq -c | sort -rn | head -12
echo "   (absent on a kernel without the feature -- exec_note_load is new)"
echo "-- G1 corroboration: the page cache's own view"
grep -a "\[pcache\].*hits" "$LOG" | tail -3 | sed 's/.*\[pcache\]/   [pcache]/'

echo
echo "-- G2: tier-1 drops, split zero vs page-cache"
grep -a "dropped free" "$LOG" | sed 's/.*\[reclaim\]/   [reclaim]/' | tail -4
grep -a "ELFSTAT-RECLAIM" "$LOG" | sed 's/^/   /'

echo
echo "-- G3: what an exec costs (cumulative means; marginal is the difference)"
grep -a "in aex_load" "$LOG" | sed 's/.*\[exec\]/   [exec]/'
grep -a "\[exec\] loader:" "$LOG" | tail -2 | sed 's/.*\[exec\]/   [exec]/'

echo
echo "-- R5: the page cache against its 4096-slot (16 MiB) ceiling"
grep -a "pages resident (peak" "$LOG" | tail -2 | sed 's/.*\[pcache\]/   [pcache]/'

echo
echo "-- boot accounting"
grep -a "\[mm\] boot:" "$LOG" | head -1 | sed 's/^/   /'
echo "============================================================"

# The harness asserts only what must be true of ANY build: that the experiment
# ran. Every number above is a measurement to be compared against the other
# build, not a threshold -- a pass/fail on a frame count would be this harness
# deciding the answer it was built to find out.
rc=0
[ "$UP" -ge 3 ] || { echo "FAIL: only $UP subject(s) came up; the G1 series is not a series"; rc=1; }
[ "$STATS" -ge 7 ] || { echo "FAIL: only $STATS measurement(s) completed"; rc=1; }
grep -aq "ELFSHARE_DONE" "$LOG" || { echo "FAIL: the guest never reached ELFSHARE_DONE"; rc=1; }
exit $rc
