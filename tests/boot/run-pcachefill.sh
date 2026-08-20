#!/usr/bin/env bash
# THE PAGE CACHE'S CEILING, ON A RUNNING MACHINE, WITH THE POOL DELIBERATELY
# LOADED -- and the two counters that used to make its failure invisible.
#
# ===========================================================================
# WHAT WAS WRONG, AND WHY NOTHING CAUGHT IT
#
# c/kernel/mm/pcache.c's pool is a fixed number of entries. When every entry is
# taken and every page in them is MAPPED, the old code fell through, handed the
# new page back with NO CACHE ENTRY BEHIND IT, and returned. fault.c's do_file()
# then took its own reference on that frame and installed one PTE -- so the
# frame ended up with rmap_count 1, pcache_holds 0 and pmm_refcount 2. It fails
# reclaim's eligibility test forever, and when the process exits the PTE's
# reference is dropped and the allocation reference is not. ONE 4 KiB FRAME LOST
# PER FAULT PAST THE CEILING.
#
# Nothing caught it because from every direction that a test normally looks,
# NOTHING IS WRONG: the fault succeeds, the process runs, the bytes are right,
# and the only trace is a free-frame count that never comes back. There was no
# counter for it and no console line. This harness exists to make that state
# reachable on purpose and to assert it does not happen.
#
# ===========================================================================
# THE THRESHOLD, AND WHERE IT COMES FROM
#
# The workload has to be big enough to distinguish the pool this kernel has now
# from the pool it had before. That number is not a judgement: THE OLD POOL WAS
# 4096 ENTRIES (`#define PCACHE_MAXPAGE 4096`, clamped at init to
# total_frames/16, which on the 512 MiB boot this harness uses did not bite).
# So:
#
#   PCFILL_MIN_PAGES = 4096   -- the size of the pool that shipped
#
# A run that maps FEWER distinct file pages than that cannot tell the two
# builds apart, whatever it reports, and this harness FAILS rather than passing
# on it. That is the answer to "what does it do when the workload is absent":
# it says the workload is absent, names the number it got, and exits non-zero.
# It does not fall back to asserting something smaller.
#
# The second threshold is not a number at all. The pool's size is DERIVED from
# RAM (pcache.h: total_frames / PCACHE_FRAME_SHARE), so this harness recomputes
# it from the machine's OWN frame count as printed by pmm_report(), and requires
# the kernel's announced slot count to equal it. A pool that silently came up
# at some other size -- an allocation that half-failed, a clamp nobody meant --
# would otherwise look exactly like a pool that is simply large.
#
# ===========================================================================
# WHAT IT DRIVES
#
# /usr/as/examples/pcachefill.as maps every large file on the disk read-only
# through SYS_MMAP_FILE, touches every page of each, KEEPS them all mapped, and
# maps the first three of them a SECOND time through a second open so that some
# pages carry two PTEs and a cache entry at once. Then, with all of it still
# live, it forces a reclaim pass and dumps the kernel's counters.
#
# That last part is the point of doing this on a machine rather than on the
# host: reclaim's safety rule is that three independently maintained numbers
# agree (rmap_count + pcache_holds == pmm_refcount), and the doubly-mapped
# pages are the only ones where a wrong term would not cancel -- at one PTE,
# 1 + 1 == 2 is satisfied by several wrong readings.
#
# ===========================================================================
# WHAT IT LOOKS LIKE WHEN IT WORKS, AND WHEN IT DOES NOT -- both measured
# 2026-08-20 on the same disk image, 512 MiB, 4 cores, TCG. The only difference
# between the two runs is the kernel (-DPCACHE_LEGACY_POOL; see pcache.h):
#
#                          shipped            legacy control
#   pool                   65518 slots        4096 slots
#   workload               4775 pages         4775 pages   (identical)
#   peak                   4782 (7%)          4096 (100%)
#   orphaned               0                  0   (the pass is compiled out)
#   UNCACHED AND LEAKED    0                  685
#   free frames before     116751             117126
#   ...after exit+reclaim  116767  (+16)      116456  (-670)
#
# -670 against a counter that says 685 is two independently produced numbers
# agreeing to within 15 frames on a desktop that is still running, which is
# what makes the counter believable rather than merely present.
#
# NOTE which assertions the control reddens: the two that matter are `uncached
# == 0` and the pool-peak-vs-workload one. It ALSO reddens the derivation check
# in section 1, necessarily -- the control's whole content is a different pool
# size -- so that failure is expected there and is not the finding.
#
# Usage:  bash tests/boot/run-pcachefill.sh <iso> <disk.img> [label]
# Makefile equivalent (NOT wired -- the Makefile is owned by another line this
# run; this is the rule to add, and its negative control):
#     test-pcachefill: $(ISO) $(DISK)
#     	@bash tests/boot/run-pcachefill.sh $(ISO) $(DISK)
set -u

. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-pcachefill.sh <iso> <disk.img> [label]}"
DISK="${2:?usage: run-pcachefill.sh <iso> <disk.img> [label]}"
LABEL="${3:-run}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${PCACHEFILL_LOG:-$(mktemp)}"
RAM="${PCACHEFILL_RAM:-512}"

# The size of the pool this file's subject shipped with, before it was sized
# from RAM. See "THE THRESHOLD" above -- this is a historical fact about the
# code, not a tuning knob, which is why it is spelled out rather than passed in.
PCFILL_MIN_PAGES=4096

# pcache.h's PCACHE_FRAME_SHARE. Duplicated here on purpose and checked against
# what the kernel PRINTS, so a change to one without the other is a failure
# rather than a silent divergence.
PCFILL_FRAME_SHARE=2

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

fail=0
bad() { echo "FAIL: $*"; fail=1; }

w() { logit_wait_for_marker "$LOG" "$1" "${2:-300}"; }

{
  logit_wait_for_shell "$LOG" 240
  # A reading BEFORE the workload, so the pages the workload adds are a
  # difference and not a total. The desktop is already up (wm_run is init), so
  # this is "boot + desktop" and includes /bin/as's own two file areas.
  printf '/bin/as /usr/as/examples/elfstat.as PCFILL_BEFORE\n'
  w "ELFSTAT-END PCFILL_BEFORE"
  printf '/bin/as /usr/as/examples/pcachefill.as\n'
  w "PCFILL-END" 600
  printf 'echo PCFILL_SHELL_ALIVE\n'
  w "PCFILL_SHELL_ALIVE" 120
  # AFTER the workload's process has EXITED, so every mapping is gone and every
  # reference it held has been dropped. This reading is corroboration for the
  # uncached counter rather than a second assertion: a frame that leaks is a
  # frame that does not come back here, and free-frame counts on a live desktop
  # move for reasons this harness does not control, so it is PRINTED next to
  # the before-reading and not turned into a threshold nobody can derive.
  # ...and force a reclaim pass first, which is what makes this reading able to
  # tell the two things apart. After the process exits its pages are still
  # CACHED -- pcache_file_put() leaves an idle entry on purpose, and pcache.c
  # argues at length that purging there would defeat the whole design -- so a
  # bare free-frame count cannot distinguish "cached and reclaimable" from
  # "leaked". A page-cache page comes back when reclaim asks for it. A leaked
  # frame never does.
  printf '/bin/as /usr/as/examples/elfstat.as PCFILL_AFTER 4096\n'
  w "ELFSTAT-END PCFILL_AFTER" 300
  printf 'echo PCACHEFILL_DONE\n'; sleep 3
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m "${RAM}M" -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      -netdev user,id=n0 -device e1000,netdev=n0 \
      -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 12000); do
    grep -aq "PCACHEFILL_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "=================== pcachefill: $LABEL  (${RAM} MiB) ==================="
echo "log: $LOG"

# STRIP THE CARRIAGE RETURNS FIRST. The guest writes to a serial console, so
# every line arrives CRLF-terminated, and a value scraped from the END of a line
# carries a \r that `[ "$x" = "0" ]` does not match -- which reads as "the audit
# reported 0 problems" and FAILS, i.e. an assertion that cannot be satisfied by
# the thing it is asserting. Found the first time this harness ran. Mid-line
# fields never showed it, which is exactly why it is done here for all of them
# rather than at each site.
LOGN="$(mktemp)"
tr -d '\r' < "$LOG" > "$LOGN"
LOG_RAW="$LOG"
LOG="$LOGN"

grep -aq "PCFILL-END" "$LOG" || {
    echo "the guest never reached PCFILL-END. The serial tail:"
    tail -25 "$LOG" | sed 's/^/   /'
    echo "FAIL: the workload did not run at all -- nothing below was measured"
    exit 1
}

# --- 1. the pool's size is DERIVED, so recompute it from this machine -------
UP=$(grep -a "\[pcache\] up:" "$LOG" | tail -1)
SLOTS=$(printf '%s\n' "$UP" | sed -n 's/.*up: \([0-9]*\) page slots.*/\1/p')
TOTAL=$(grep -a "frames total" "$LOG" | tail -1 | sed -n 's/.*: \([0-9]*\) frames total.*/\1/p')
echo "-- the pool"
echo "   $UP"
if [ -z "${SLOTS:-}" ]; then
    bad "the kernel never announced a page-cache pool ([pcache] up: is missing)"
elif [ -z "${TOTAL:-}" ]; then
    echo "   NOTE: no 'frames total' line in this log, so the derivation could not be"
    echo "         re-checked against the machine. The slot count itself is still asserted."
else
    WANT=$(( TOTAL / PCFILL_FRAME_SHARE ))
    echo "   machine has $TOTAL frames; pcache.h says total/$PCFILL_FRAME_SHARE = $WANT slots"
    [ "$SLOTS" = "$WANT" ] || bad "the pool is $SLOTS slots, the derivation says $WANT --" \
        "either pcache_init() clamped and did not say so, or PCACHE_FRAME_SHARE moved" \
        "without this harness"
fi

# --- 2. did the workload actually happen? ----------------------------------
MAPPED=$(grep -a "^PCFILL-MAPPED" "$LOG" | tail -1)
FILES=$(printf '%s\n' "$MAPPED" | awk '{print $2}')
PAGES=$(printf '%s\n' "$MAPPED" | awk '{print $3}')
SHARED=$(printf '%s\n' "$MAPPED" | awk '{print $4}')
REFUSED=$(printf '%s\n' "$MAPPED" | awk '{print $5}')
echo "-- the workload"
echo "   $MAPPED"
grep -a "^PCFILL-MAP " "$LOG" | tail -6 | sed 's/^/      /'
: "${FILES:=0}" "${PAGES:=0}" "${SHARED:=0}" "${REFUSED:=0}"
echo "   $FILES files mapped and fully touched, $PAGES distinct pages,"
echo "   $SHARED of them mapped TWICE, $REFUSED paths refused (missing, or out of areas/slots)"

if [ "$PAGES" -le "$PCFILL_MIN_PAGES" ]; then
    bad "THE WORKLOAD IS ABSENT, not the bug: $PAGES pages is not more than the" \
        "$PCFILL_MIN_PAGES the old pool held, so this run cannot tell the two builds" \
        "apart. Nothing below is evidence. Most likely the disk image no longer" \
        "carries the files pcachefill.as names, or VMA_MAXAREA ran out sooner."
fi
[ "$SHARED" -gt 0 ] || bad "no page was mapped twice, so the doubly-referenced case" \
    "-- the only one where a wrong term in reclaim's sum would not cancel -- never ran"

# --- 3. the ceiling counters -----------------------------------------------
CEIL=$(grep -a "slots used at peak" "$LOG" | tail -1)
echo "-- the ceiling"
echo "   $CEIL"
PEAK=$(printf '%s\n' "$CEIL" | sed -n 's/.*pool \([0-9]*\)\/.*/\1/p')
ORPH=$(printf '%s\n' "$CEIL" | sed -n 's/.*; \([0-9]*\) entries orphaned.*/\1/p')
UNC=$(printf '%s\n' "$CEIL" | sed -n 's/.*, \([0-9]*\) pages handed back UNCACHED.*/\1/p')
: "${PEAK:=-1}" "${ORPH:=-1}" "${UNC:=-1}"

[ "$UNC" = "0" ] || bad "$UNC page(s) were handed back UNCACHED -- each one is a leaked" \
    "4 KiB frame and a page-cache entry that never existed. This is the defect the" \
    "orphan pass exists to make impossible; a nonzero value here is a bug in" \
    "c/kernel/mm/pcache.c, not a capacity report"
[ "$ORPH" = "0" ] || bad "$ORPH entr(ies) were orphaned: the pool ($SLOTS slots) was full of" \
    "pages something maps, with only $PAGES pages of workload. That is a capacity" \
    "finding -- the pool is too small for this machine -- and the sizing in pcache.h" \
    "is what to change"
if [ "$PEAK" -ge 0 ] && [ "$PAGES" -gt 0 ]; then
    [ "$PEAK" -ge "$PAGES" ] || bad "the pool peaked at $PEAK entries while the workload" \
        "mapped $PAGES distinct pages -- every touched page must be an entry, so pages" \
        "went somewhere this harness cannot see"
fi

# --- 4. reclaim's third number, under the mappings ---------------------------
RCL=$(grep -a "page-cache frames met" "$LOG" | tail -1)
echo "-- reclaim, with every mapping still live"
echo "   $RCL"
grep -a "PCFILL-RECLAIM" "$LOG" | tail -1 | sed 's/^/   /'
# The tier split, printed rather than asserted: "[N zero + M cache]" is the
# only place the two producers of tier 1 are told apart, and M was structurally
# 0 on this machine until file-backed text existed. It is reported here because
# a run where the forced pass was satisfied entirely by anonymous zero pages
# would satisfy every assertion below while proving nothing about file pages.
grep -a "dropped free \[" "$LOG" | tail -1 | sed 's/.*\[reclaim\]/   [reclaim]/'
MET=$(printf '%s\n' "$RCL" | sed -n 's/.*frames met \([0-9]*\),.*/\1/p')
UNACC=$(printf '%s\n' "$RCL" | sed -n 's/.*UNACCOUNTED-FOR REFERENCE \([0-9]*\).*/\1/p')
: "${MET:=-1}" "${UNACC:=-1}"
[ "$MET" -gt 0 ] || bad "reclaim's sweep never met a page-cache frame, so the next" \
    "assertion has no denominator and proves nothing -- the forced pass did not reach" \
    "the workload's pages"
[ "$UNACC" = "0" ] || bad "$UNACC page-cache frame(s) had a reference reclaim could not" \
    "account for (rmap_count + pcache_holds != pmm_refcount). That is precisely the" \
    "structurally-unevictable page pcache.h's refcount decision exists to prevent"

# --- 5. the machine is still sane -------------------------------------------
AUD=$(grep -a "^PCFILL-AUDIT" "$LOG" | tail -1 | awk '{print $2}')
: "${AUD:=-1}"
echo "-- audit and survival"
echo "   PCFILL-AUDIT $AUD (mm_audit bugs, 0 required)"
[ "$AUD" = "0" ] || bad "the kernel's own mm audit reported $AUD problem(s) after the fill"
grep -aq "PCFILL_SHELL_ALIVE" "$LOG" \
    || bad "the shell stopped answering after the fill -- the workload took the machine down"

# The free-frame trio: before the workload, during it (mappings live), and
# after the process exited. Printed, not asserted -- see the harness note where
# PCFILL_AFTER is typed. Under the legacy-pool control this is where the leak
# shows up as a number that never comes back.
echo "-- free frames: boot / before the workload / during it / after it exits AND a"
echo "   forced reclaim pass. The last two are the pair to read: a cached page comes"
echo "   back when reclaim asks for it, a leaked frame does not."
grep -a "frames total" "$LOG" | sed 's/.*\[mm\]/   [mm]/' | tail -4
grep -a "ELFSTAT-RECLAIM PCFILL_AFTER" "$LOG" | sed 's/^/   /'

NBUG=$(grep -ac "\[pcache\] BUG" "$LOG")
if [ "${NBUG:-0}" -gt 0 ]; then
    # Capped. Each of these is one leaked frame, so a real failure prints
    # hundreds of them and a full dump buries every other finding in this
    # report -- which is the opposite of what a failing gate should do.
    echo "   $NBUG '[pcache] BUG' line(s); first and last:"
    grep -a "\[pcache\] BUG" "$LOG" | head -1 | sed 's/^/      /'
    grep -a "\[pcache\] BUG" "$LOG" | tail -1 | sed 's/^/      /'
    bad "the page cache reported $NBUG BUG line(s) of its own (above)"
fi

echo "==========================================================="
if [ "$fail" -ne 0 ]; then
    echo "FAIL: pcachefill ($LABEL)"
    exit 1
fi
echo "PASS: pcachefill ($LABEL): $PAGES pages of file data resident at once across" \
     "$FILES files, $SHARED of them doubly mapped; pool $PEAK/$SLOTS, 0 orphaned," \
     "0 uncached, 0 unaccounted-for references"
exit 0
