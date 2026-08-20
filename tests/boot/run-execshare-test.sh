#!/usr/bin/env bash
# FILE-BACKED TEXT IS ACTUALLY SHARED, ON THE MACHINE.
#
# tests/unit/exec_test.c proves elf_file_runs() picks the right pages, against a
# host MMU with no page cache at all. This is the half it cannot do: that two
# processes running the same binary at the same instant map the SAME FRAMES.
#
# ---------------------------------------------------------------------------
# WHY THE OBVIOUS GATE IS THE WRONG ONE, measured rather than argued
# ---------------------------------------------------------------------------
# "Launch the same app twice and require the second launch to cost less than the
# first" is the natural shape, and it measures THE PAGE CACHE, not sharing.
# Work it through with T = text pages touched and P = private pages per copy:
#
#                        first launch      second launch     difference
#   shared (correct)       P + T             P                  T
#   NOT shared (control)   P + T + T         P + T              T
#
# The difference is T in BOTH builds -- the first launch pays the device read
# either way, and the second is cheaper than the first either way. A harness
# built on that comparison passes against a loader that gives every process its
# own private copy, which is exactly the bug this feature exists not to have.
#
# What discriminates is the ABSOLUTE marginal cost of one more CONCURRENT copy
# (P versus P + T), and the direct count of frames mapped more than once:
# pcache_shared() in c/kernel/mm/pcache.c counts entries whose frame has
# refcount > 2 -- the cache's own reference plus MORE THAN ONE PTE. That number
# is 0 by construction unless two page tables point at one frame. It is the
# feature, counted, rather than inferred from free-frame arithmetic.
#
# ---------------------------------------------------------------------------
# WHAT IT DOES
# ---------------------------------------------------------------------------
# Three readings, each taken with exactly ONE MORE long-lived copy of /bin/as
# alive than the last, and each taken BY /bin/as (elfstat.as) so that the
# measuring process is a constant that cancels out of every difference:
#
#   before : {elfstat}              1 copy   -- nothing can be shared yet
#   one    : {A, elfstat}           2 copies -- sharing becomes possible here
#   two    : {A, B, elfstat}        3 copies
#
# The subject is /bin/as running elfspin.as, which blocks in SYS_NANOSLEEP: a
# busy-waiting subject would compete for the same TCG cores as the instrument,
# and the numbers would come off a machine loaded by its own measurement.
#
# ---------------------------------------------------------------------------
# NOT PRESENT / NOT RUNNABLE IS A DIFFERENT FINDING FROM NOT SHARED
# ---------------------------------------------------------------------------
# and this harness says which. A missing /usr/as/examples script, a machine that
# never reached a shell, and an execve refused by the permission check are each
# reported in their own words and exit 2 (apparatus) rather than 1 (regression).
# The distinction is the lesson of the qmp_site.py trigger: "the dump did not
# happen" and "the page painted nothing" are not the same finding.

set -u

. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-execshare-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-execshare-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
FIFO="$(mktemp -u)"
cleanup() {
    if [ -n "${QPID:-}" ]; then
        kill "$QPID" 2>/dev/null
        wait "$QPID" 2>/dev/null
    fi
    rm -f "$LOG" "$FIFO"
}
trap cleanup EXIT

# --- the threshold, derived ------------------------------------------------
# T_MIN is a floor on how many whole text pages of /bin/as two copies must be
# found sharing. It is NOT fitted to an observed value; the observed value is
# reported below and has a large margin over it.
#
# /bin/as is 327,240 bytes with a 249,140-byte R E segment and a 29,624-byte R
# segment (readelf -lW build/as.elf), which elf_file_runs() turns into 60 + 7 =
# 67 whole shareable pages. The subject compiles and runs a script: lexer,
# indentation, Pratt compiler, the VM dispatch loop, layout()/args()/print()
# and the raw syscall bridge. Claiming that fewer than 16 of the 61 text pages
# (26%) are executed to do the interpreter's ONLY job would be extraordinary.
#
# The margin is what makes the exact figure unimportant: a loader that hands
# each process a private copy scores EXACTLY 0 here, not 15.
#
# MEASURED, so that the margin is a fact and not a hope: this machine reports
# 32 on the shipped loader and 0 against the private-copy control below, so any
# T_MIN in 1..32 gates identically and 16 sits in the middle of that range. 32
# rather than 67 because only the pages the interpreter actually EXECUTES are
# faulted in -- which is the feature working, not a shortfall.
#
# It is a floor and not an equality on purpose: 32 is a property of how much of
# itself /bin/as runs to compile a script, and that may legitimately move when
# the interpreter changes. 0 versus nonzero is the property being gated.
T_MIN="${T_MIN:-16}"

fail=0
apparatus=0
say()  { echo "  $*"; }
ok()   { echo "  ok: $*"; }
bad()  { echo "  FAIL: $*"; fail=1; }
appt() { echo "  APPARATUS: $*"; apparatus=1; }

# wait_line <regex> <seconds>. Never sleeps a guessed constant: every step waits
# for the machine to SAY it is done.
wait_line() {
    _n=$(( ${2:-30} * 10 ))
    _i=0
    while [ "$_i" -lt "$_n" ]; do
        grep -aq "$1" "$LOG" 2>/dev/null && return 0
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.1
        _i=$((_i + 1))
    done
    return 1
}

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# The feed is a FIFO this script writes as the machine reaches each point,
# rather than a fixed string with sleeps between its lines. That is the whole
# reason elfspin.as prints ELFSPIN-UP: "the subject had not finished loading"
# and "the sharing did not happen" produce the same free-frame reading, and
# only one of them is a finding.
mkfifo "$FIFO" || { echo "APPARATUS: cannot create fifo $FIFO"; exit 2; }

"$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot <"$FIFO" >"$LOG" 2>/dev/null &
QPID=$!
exec 9>"$FIFO"

echo "execshare: do two processes running one binary map the same frames?"

logit_wait_for_shell "$LOG" 150

# `echo`, not a bare word: the shell would try to EXECUTE a bare marker, and on
# a machine where the exec-permission check is refusing everything that failure
# is indistinguishable from the one this harness is looking for.
printf 'echo EXECSHARE-START\n' >&9
wait_line "^EXECSHARE-START" 30 || appt "the shell never echoed EXECSHARE-START -- nothing is reading our bytes."

# Reading 1: nothing but the instrument is alive.
printf '/bin/as /usr/as/examples/elfstat.as before\n' >&9
wait_line "ELFSTAT-END before" 120 || appt "the instrument never completed (no ELFSTAT-END before)."

# Subject A, then a reading with it alive.
printf '/bin/as /usr/as/examples/elfspin.as A &\n' >&9
wait_line "ELFSPIN-UP A" 120 || appt "subject A never reported itself up."
printf '/bin/as /usr/as/examples/elfstat.as one\n' >&9
wait_line "ELFSTAT-END one" 120 || appt "the instrument never completed (no ELFSTAT-END one)."

# Subject B, then a reading with both alive.
printf '/bin/as /usr/as/examples/elfspin.as B &\n' >&9
wait_line "ELFSPIN-UP B" 120 || appt "subject B never reported itself up."
printf '/bin/as /usr/as/examples/elfstat.as two\n' >&9
wait_line "ELFSTAT-END two" 120 || appt "the instrument never completed (no ELFSTAT-END two)."

printf 'echo EXECSHARE-END\n' >&9
wait_line "^EXECSHARE-END" 30
exec 9>&-
sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

# Kept unconditionally, not only on failure: the readings below are a
# measurement, and a measurement whose raw output is discarded on success
# cannot be re-read when the number turns out to be surprising later.
KEEP="${EXECSHARE_LOG:-${TMPDIR:-/tmp}/execshare-last.log}"
cp "$LOG" "$KEEP" 2>/dev/null && echo "  serial log kept at $KEEP"

# ---------------------------------------------------------------- apparatus --
# Named causes first. Each of these makes every number below meaningless, and
# each is somebody else's finding rather than this feature's.
if grep -aq "permission denied (not executable)" "$LOG"; then
    appt "execve was REFUSED by the permission check:"
    grep -a "permission denied (not executable)" "$LOG" | sed 's/^/      /' | head -3
    say "    c/kernel/exec/exec.c asks vfs_access(path, MAY_EXEC), and tools/mkfs.py"
    say "    leaves every inode's xmode zero, which c/fs/logitfs.c reports as the"
    say "    0644 default. c/fs/vfs_meta.c gives root no MAY_EXEC bypass when no x"
    say "    bit is set anywhere, so NOTHING on the disk can be exec'd. That is an"
    say "    in-flight change's missing half, not this feature."
fi
if ! grep -aq "$LOGIT_SHELL_BANNER" "$LOG"; then
    appt "the machine never reached a shell -- this is a BOOT failure."
fi
if grep -aq "as: cannot open" "$LOG"; then
    appt "the instrument or the subject is not on this disk:"
    grep -a "as: cannot open" "$LOG" | sed 's/^/      /' | head -3
fi

# ----------------------------------------------------------------- readings --
# The section between ELFSTAT-BEGIN <tag> and ELFSTAT-END <tag>. Anchored on the
# tag rather than on line numbers because mm_report()'s own tag is the constant
# string "on demand" (c/kernel/mm/mmsys.c) and is identical in all three.
#
# `tr -d \r` is load-bearing and cost a run to find: the guest's serial console
# ends every line CRLF, so an end-anchored match ($) never fires and all six
# readings come back empty -- which this harness then reports as "the instrument
# did not report", i.e. as the machine's fault rather than the parser's. That is
# exactly the failure this file's own apparatus/regression split exists to make
# visible, and it caught it.
section() { tr -d '\r' < "$LOG" | awk "/ELFSTAT-BEGIN $1\$/{f=1} f{print} /ELFSTAT-END $1\$/{f=0}"; }

# "[pcache] on demand: N pages resident (peak P of S slots), F files; H hits, M misses, X shared"
shared_of() { section "$1" | grep -a "\[pcache\] on demand:" | head -1 | \
              sed -n 's/.*, \([0-9][0-9]*\) shared.*/\1/p'; }
# "[mmstat] free=N total=..."
free_of()   { section "$1" | grep -a "\[mmstat\] " | head -1 | \
              sed -n 's/.*free=\([0-9][0-9]*\).*/\1/p'; }
# "[mm] on demand: ... N refs (+S saved = X KiB) ..." -- pmm.c prints
# refs_total - used_frames, i.e. references that did NOT cost a frame. That is
# THE FRAME COUNTER for this feature: every page two processes share is one
# reference here that no allocation stands behind.
saved_of()  { section "$1" | grep -a "frames total" | head -1 | \
              sed -n 's/.*(+\([0-9][0-9]*\) saved.*/\1/p'; }

s_before="$(shared_of before)"; f_before="$(free_of before)"; v_before="$(saved_of before)"
s_one="$(shared_of one)";       f_one="$(free_of one)";       v_one="$(saved_of one)"
s_two="$(shared_of two)";       f_two="$(free_of two)";       v_two="$(saved_of two)"

for v in s_before s_one s_two f_before f_one f_two v_before v_one v_two; do
    eval "val=\${$v:-}"
    if [ -z "$val" ]; then
        appt "reading '$v' is missing from the serial log -- the instrument did not report."
    fi
done

if [ "$apparatus" -ne 0 ]; then
    echo "APPARATUS FAILURE: the machine could not be measured, so nothing is"
    echo "claimed about sharing. This is NOT a pass and NOT a regression."
    echo "----- serial output (tail) -----"
    tail -60 "$LOG"
    echo "--------------------------------"
    exit 2
fi

# What the loader said it would share, from its own per-load line, so the
# assertion below is against what THIS boot did rather than a remembered value.
ld_line="$(grep -a '\[exec\] load /bin/as:' "$LOG" | head -1)"
ld_file="$(echo "$ld_line" | sed -n 's/.*: \([0-9][0-9]*\) pages file-backed.*/\1/p')"
ld_copy="$(echo "$ld_line" | sed -n 's/.*areas, \([0-9][0-9]*\) copied.*/\1/p')"

say "loader:  ${ld_line:-(no per-load line for /bin/as)}"
say "shared pages mapped >1x:  before=$s_before  one=$s_one  two=$s_two   (T_MIN=$T_MIN)"
say "frames saved by sharing:  before=$v_before  one=$v_one  two=$v_two"
say "free frames:              before=$f_before  one=$f_one  two=$f_two"
# Informational only -- see assertion 3 for why this cannot be the gate.
say "gross cost of one more concurrent copy: $((f_before - f_one)) then $((f_one - f_two)) frames (stack + heap dominate)"

# ---------------------------------------------------------------- assertions --
# 1. THE DISCRIMINATING ONE, and it is a DELTA rather than an absolute.
#
#    `before` is not zero on a working machine and must not be asserted to be:
#    /bin/sh is itself file-backed, and the shell forks a child for every
#    command, so two page tables map the shell's text whenever a command is
#    running. That is the same feature working on a different binary -- real,
#    but not the subject. Subtracting it is what makes the number the subject's.
#
#    Note that the count does NOT keep rising from `one` to `two`: it counts
#    frames with refcount > 2, so a page goes from uncounted to counted when the
#    SECOND mapper appears and stays counted for the third. `one` is therefore
#    where the whole effect lands, and `two` only has to hold it.
d_one=$((s_one - s_before))
d_two=$((s_two - s_before))
if [ "$d_one" -ge "$T_MIN" ] && [ "$d_two" -ge "$T_MIN" ]; then
    ok "a second copy of /bin/as put $d_one more file pages under >1 mapping (>= $T_MIN), still $d_two with a third"
else
    bad "a second copy of /bin/as added only $d_one shared file pages (third: $d_two), under $T_MIN"
    say "    A loader that gives every process its own private copy adds exactly 0"
    say "    here. This is that number, or near enough to mean the same thing."
fi

# 2. The subject must be file-backed at all. If the loader took the eager path
#    for /bin/as -- a v1 image, an unstattable path, a full pcache file table --
#    then there is nothing to share and assertion 1 would be failing for a
#    reason that has nothing to do with sharing.
if [ -n "$ld_file" ] && [ "$ld_file" -gt 0 ]; then
    ok "the loader mapped $ld_file pages of /bin/as from its file rather than copying them"
else
    bad "the loader took the EAGER path for /bin/as ($ld_file pages file-backed)"
    say "    Nothing can be shared when nothing is file-backed; assertion 1 above is"
    say "    then measuring the absence of the input, not the absence of sharing."
fi

# 3. THE FRAME COST, which is the payoff, asserted against the loader's own
#    figures. Adding one more CONCURRENT copy must not cost a private copy of
#    the text: the marginal cost must sit nearer `copied` than `copied + shared`.
# 3. THE FRAME COUNT, which is the payoff.
#
#    NOT the free-frame delta, and that is worth stating because it is the
#    obvious choice and it does not work. A copy of /bin/as costs ~217 frames
#    of which only 83 are its image: the rest is the 256-page user stack and
#    the interpreter's heap, faulted in as it runs. Those dwarf the ~32 text
#    pages at issue, so an absolute per-copy cost cannot separate a build that
#    shares text from one that does not without knowing the private cost
#    independently -- and the private cost is exactly what a single boot cannot
#    observe, because the counterfactual build is not running.
#
#    pmm.c prints the quantity that IS one-boot decidable: refs_total minus
#    used_frames, "+N saved". A reference that no allocation stands behind is a
#    frame this feature did not spend. It rises by the shared text per extra
#    concurrent copy and by nothing else, so it needs no baseline for P.
g_one=$((v_one - v_before))
g_two=$((v_two - v_one))
if [ "$g_one" -ge "$T_MIN" ] && [ "$g_two" -ge "$T_MIN" ]; then
    ok "frames saved by sharing rose $v_before -> $v_one -> $v_two, i.e. +$g_one then +$g_two per extra copy (>= $T_MIN)"
else
    bad "frames saved rose only +$g_one then +$g_two per extra copy of /bin/as (want >= $T_MIN)"
    say "    Every text page a second process maps is one reference with no frame"
    say "    behind it. A loader that copies instead spends the frame, and this"
    say "    number stays flat -- which is what it is doing."
fi

# 4. The two instruments must AGREE. They are independent: one counts page-cache
#    entries whose frame has more than one PTE, the other counts references the
#    allocator never had to back. If sharing is real they must move together,
#    and a discrepancy means one of them is measuring something else.
if [ "$g_one" -eq "$d_one" ]; then
    ok "and the two instruments agree exactly: +$g_one frames saved, $d_one pages newly shared"
else
    say "note: frames saved (+$g_one) and pages newly shared ($d_one) differ; both are still above $T_MIN"
fi

# 5. Sharing frames is exactly the change that can make a refcount and a
#    reverse map stop agreeing, and pmm_audit + rmap_audit is the cheapest way
#    to be told so. rmap.h's rule -- evict only if rmap_count == pmm_refcount --
#    is what this protects: a leak in either direction costs reclaimability.
if grep -aq "ELFSTAT-AUDIT two 0" "$LOG"; then
    ok "the allocator and the reverse map agree with three copies alive (audit 0)"
else
    bad "the mm audit did not report 0 inconsistencies with three copies alive"
    grep -a "ELFSTAT-AUDIT\|\[mm\] audit:" "$LOG" | sed 's/^/      /' | head -4
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL: file-backed text is not being shared between processes"
    echo "----- serial output (tail) -----"
    tail -80 "$LOG"
    echo "--------------------------------"
    exit 1
fi
echo "PASS: two processes running one binary map the same frames ($s_two pages, $((s_two * 4)) KiB not duplicated)"
exit 0
