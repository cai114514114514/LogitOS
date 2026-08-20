#!/usr/bin/env bash
# THE GUARD PAGE, BEFORE AND AFTER, IN ONE BOOT.
#
# What is being measured is not "threads still work". It is that a stack
# overrun now stops AT AN ADDRESS THAT WAS PRINTED BEFORE IT HAPPENED, and that
# before this change the same overrun stopped nowhere and corrupted a live
# buffer instead. Both halves run on the same kernel, the same allocator and
# the same addresses, because both programs are on this one disk:
#
#   /bin/thrtest-noguard guard   pthread_attr_setguardsize(&a, 0)
#       must print  GUARD_TEST_NOFAULT
#       must report a nonzero count of corrupted victim bytes
#       must NOT produce a `[fault]` line
#
#   /bin/thrtest guard           pthread_attr_setguardsize(&a, 4096)
#       must NOT print GUARD_TEST_NOFAULT (it must not survive)
#       must produce `[fault] app exception: ... cr2=0x...`
#       and that cr2 must be INSIDE the `thrtest: guard [lo,hi)` range the
#       program printed before it started descending
#
# THE cr2 COMPARISON IS THE WHOLE TEST. "It faulted" is not the claim -- the
# unguarded build eventually faults too, somewhere, later, after wrecking
# something. The claim is that the fault is AT A PREDICTED ADDRESS, so the
# harness parses the predicted range out of the guest's own output rather than
# hard-coding one: the mmap window's placement is the allocator's business and
# a baked-in address would be a test of vma_reserve's first fit, not of the
# guard.
#
# Portable: no `timeout`, same shape as run-thread-test.sh.

set -u

ISO="${1:?usage: run-guard-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-guard-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# One core is enough and is the right choice: this measures one thread walking
# off one stack, and -smp would only add scheduling noise to the serial log.
{ sleep 11
  printf '/bin/thrtest-noguard guard\n'; sleep 20
  printf 'echo GUARD-SPLIT\n';           sleep 3
  printf '/bin/thrtest guard\n';         sleep 20
  printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -accel tcg -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Wait for the second half to have produced its verdict, or for QEMU to die.
for _ in $(seq 1 900); do
    grep -aq "GUARD-SPLIT" "$LOG" && grep -aq "\[fault\] app exception" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 2
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

# Split the log at the echo, so a `[fault]` from the FIRST half can never be
# credited to the second. Without this the test would pass on a build where the
# unguarded run crashed and the guarded one printed nothing.
BEFORE="$(sed -n '1,/GUARD-SPLIT/p' "$LOG")"
AFTER="$(sed -n '/GUARD-SPLIT/,$p' "$LOG")"

fail=0
say() { printf '%s\n' "$*"; }
chk() { if [ "$1" = 1 ]; then say "ok   $2"; else say "FAIL $2"; fail=1; fi; }

say "=== BEFORE: /bin/thrtest-noguard guard (guardsize 0) ==="
printf '%s\n' "$BEFORE" | grep -aE "thrtest:|\[fault\]" | sed 's/^/     /'

nofault=0;  printf '%s\n' "$BEFORE" | grep -aq "GUARD_TEST_NOFAULT" && nofault=1
chk "$nofault" "the overrun caused NO fault -- the thread walked out of its stack and returned"

faulted_before=0; printf '%s\n' "$BEFORE" | grep -aq "\[fault\] app exception" && faulted_before=1
chk "$((1 - faulted_before))" "and the kernel reported no exception at all for it"

CORRUPT="$(printf '%s\n' "$BEFORE" | grep -a "victim corrupted:" | head -1 | sed -E 's/.*victim corrupted: ([0-9]+) of.*/\1/')"
if [ -n "${CORRUPT:-}" ] && [ "$CORRUPT" -gt 0 ] 2>/dev/null; then
    chk 1 "instead it wrote over $CORRUPT bytes of the mapping below the stack"
else
    chk 0 "it should have corrupted the victim buffer (read '${CORRUPT:-<none>}')"
fi

say ""
say "=== AFTER: /bin/thrtest guard (guardsize 4096) ==="
printf '%s\n' "$AFTER" | grep -aE "thrtest:|\[fault\]" | sed 's/^/     /'

survived=0; printf '%s\n' "$AFTER" | grep -aq "GUARD_TEST_NOFAULT" && survived=1
chk "$((1 - survived))" "the overrun did NOT survive"

adj=0; printf '%s\n' "$AFTER" | grep -aq "adjacency ok" && adj=1
chk "$adj" "the victim buffer and the thread mapping really are adjacent (so both halves overran the same address)"

# The predicted range, taken from the guest's own line:
#   thrtest: guard  [0x60021000,0x60022000)
GLINE="$(printf '%s\n' "$AFTER" | grep -a "thrtest: guard  \[" | head -1)"
GLO="$(printf '%s\n' "$GLINE" | sed -E 's/.*\[(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+)\).*/\1/')"
GHI="$(printf '%s\n' "$GLINE" | sed -E 's/.*\[(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+)\).*/\2/')"

CR2="$(printf '%s\n' "$AFTER" | grep -a "\[fault\] app exception" | head -1 | \
       sed -E 's/.*cr2=(0x[0-9a-fA-F]+).*/\1/')"

if [ -z "${GLO:-}" ] || [ -z "${CR2:-}" ] || [ "$GLO" = "$GLINE" ]; then
    chk 0 "a predicted guard range and a cr2 were both printed (guard='${GLINE:-<none>}' cr2='${CR2:-<none>}')"
else
    say "     predicted guard [$GLO,$GHI)   faulting address cr2=$CR2"
    in=0
    if [ "$((CR2))" -ge "$((GLO))" ] && [ "$((CR2))" -lt "$((GHI))" ]; then in=1; fi
    chk "$in" "THE FAULT IS INSIDE THE GUARD PAGE -- cr2=$CR2 in [$GLO,$GHI)"
    say "     (offset into the guard page: $((CR2 - GLO)) bytes)"
fi

say ""
if [ "$fail" -ne 0 ]; then
    say "FAIL: guard page"
    say "----- serial output -----"
    tail -160 "$LOG"
    say "-------------------------"
    rm -f "$LOG"
    exit 1
fi
say "PASS: guard page -- no fault without it, a fault at a predicted address with it"
rm -f "$LOG"
exit 0
