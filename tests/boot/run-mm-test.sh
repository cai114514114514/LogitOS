#!/usr/bin/env bash
# On-device memory-management test. Boots LogitOS, drives the serial shell, and
# asserts four things that cannot be checked on the host:
#
#   1. ACCOUNTING. The boot report's numbers are self-consistent
#      (free + used == total) and there are no invariant violations.
#   2. LEAK DISCIPLINE. /bin/sh forks and execs for every command, so a batch of
#      commands is a fork/exec/exit stress test. The free-frame count is read
#      over two consecutive batches: a leak of even ONE frame per fork shows up
#      as a drift of the batch size. This is the check the whole refcount
#      change exists to survive -- a copy-on-write bug that leaks one frame per
#      fork is invisible in a single test and fatal in an hour.
#   3. FAULT CONTAINMENT. A ring-3 process that genuinely faults must still die
#      ALONE. The test makes one do so (a null write from a one-line
#      AetherScript program) and then requires the shell to keep answering AND
#      to fork+exec again -- i.e. the kernel and the desktop survived. That
#      behaviour exists today and this change goes straight through it, so it
#      is asserted rather than assumed.
#   4. THE FORK COST. Prints pages shared / pages copied / cycles per fork, the
#      numbers the copy-on-write work is judged on. With the page-fault hook
#      not yet wired (c/kernel/mm/mm.h MM_COW_DEFAULT) the kernel reports
#      cow=off and every page is copied; once it is wired the same harness
#      additionally requires "copied" to collapse.
#
# Usage:  sh tests/boot/run-mm-test.sh <iso> <disk.img>
# Makefile equivalent (not wired -- the Makefile is owned by another line):
#     test-mm-os: $(ISO) $(DISK)
#         @bash tests/boot/run-mm-test.sh $(ISO) $(DISK)
set -u

ISO="${1:?usage: run-mm-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-mm-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${MM_LOG:-$(mktemp)}"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# Three batches of fork+exec commands. The first is a warm-up whose numbers are
# thrown away (first exec of each binary, kheap arenas, file buffers); the leak
# comparison is between the second and the third, both of which run against a
# settled system, so the only difference between them is what the forks cost.
batch() { i=0; while [ $i -lt 80 ]; do printf 'true\n'; i=$((i+1)); done; }

{
  sleep 6
  batch;  printf 'echo MM_MARK1\n'; sleep 26
  batch;  printf 'echo MM_MARK2\n'; sleep 26
  batch;  printf 'echo MM_MARK3\n'; sleep 26
  # A genuine fault, in ring 3: write one byte to address 0. /bin/as reaches
  # raw memory (poke8) and raw syscalls, so this needs no new binary on the
  # disk. The app must die and nothing else may.
  printf 'echo poke8(0, 1) > /mmfault.as\n'; sleep 2
  printf '/bin/as /mmfault.as\n'; sleep 8
  printf 'echo MM_ALIVE_AFTER_FAULT\n'; sleep 2
  printf 'uname\n'; sleep 6
  printf 'echo MM_DONE\n'; sleep 4
  printf 'rm /mmfault.as\nexit\n'; sleep 2
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 2000); do
    grep -aq "MM_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

fail=0
say() { echo "$1"; }
bad() { echo "FAIL: $1"; fail=1; }

echo "----- [mm] boot report -----"
grep -a "\[mm\] boot:" "$LOG"
echo "----------------------------"

# --- 1. accounting -------------------------------------------------------
BOOT="$(grep -a '\[mm\] boot:' "$LOG" | head -1)"
if [ -z "$BOOT" ]; then
    bad "no [mm] boot: accounting line on the serial console"
else
    TOTAL=$(echo "$BOOT" | sed -n 's/.* \([0-9]*\) frames total.*/\1/p')
    FREE=$(echo  "$BOOT" | sed -n 's/.*MiB), \([0-9]*\) free.*/\1/p')
    USED=$(echo  "$BOOT" | sed -n 's/.* \([0-9]*\) used.*/\1/p')
    BUGS=$(echo  "$BOOT" | sed -n 's/.* \([0-9]*\) bugs.*/\1/p')
    if [ "$((FREE + USED))" -ne "$TOTAL" ]; then
        bad "boot accounting does not add up: $FREE free + $USED used != $TOTAL total"
    else
        say "PASS: boot accounting adds up ($FREE free + $USED used = $TOTAL frames)"
    fi
    [ "$BUGS" = "0" ] || bad "the allocator reported $BUGS invariant violations at boot"
fi

# --- 2. leak discipline --------------------------------------------------
# The metric is the MODAL free-frame count of each batch -- the value that
# recurs most often -- not the last sample and not the maximum.
#
# The kernel samples inside proc_waitpid, immediately after a reaped child's
# address space is freed, so the steady-state value repeats once per command
# and is by far the most common reading. Anything else (a GUI app that happened
# to be alive, a sample taken while a second process was still up) appears once
# or twice and is discarded by taking the mode. That is what makes two batches
# comparable at all: one stray sample is ~256 frames off, six times the size of
# the leak being looked for, and would bury it as either a false pass or a
# false failure. A leak of one frame per fork moves the mode by the batch size.
read -r F1 N1 F2 N2 MAXBUGS <<EOF
$(awk '
  function parse(  i) {
      for (i = 1; i <= NF; i++) {
          if ($(i+1) ~ /^forks,/)                      n = $i + 0;
          if ($(i+1) == "frames" && $(i+2) == "free,") f = $i + 0;
          if ($(i+1) ~ /^bugs,/)                       b = $i + 0;
      }
  }
  /\[mm\] fork:/ {
      parse();
      if (b > maxb) maxb = b;
      if (win == 1) c1[f]++;
      if (win == 2) c2[f]++;
      lastn = n;
  }
  /MM_MARK1/ { win = 1; n1 = lastn }
  /MM_MARK2/ { win = 2; n2 = lastn }
  /MM_MARK3/ { win = 3; n2 = lastn }
  END {
      for (k in c1) if (c1[k] > b1) { b1 = c1[k]; m1 = k + 0 }
      for (k in c2) if (c2[k] > b2) { b2 = c2[k]; m2 = k + 0 }
      printf "%d %d %d %d %d\n", m1, n1, m2, n2, maxb
  }
' "$LOG")
EOF

echo "----- fork accounting (last 6 samples) -----"
grep -a "\[mm\] fork:" "$LOG" | sed 's/.*\[mm\] fork:/[mm] fork:/' | tail -6
echo "--------------------------------------------"

if [ "${N1:-0}" -eq 0 ] || [ "${N2:-0}" -le "${N1:-0}" ]; then
    bad "no fork activity observed between the markers (n1=${N1:-?} n2=${N2:-?})"
else
    FORKS=$((N2 - N1))
    DRIFT=$((F1 - F2))
    say "between the markers: $FORKS forks, modal free frames $F1 -> $F2 (drift $DRIFT)"
    LIMIT=$((FORKS / 4))
    [ "$LIMIT" -lt 8 ] && LIMIT=8
    # Only a POSITIVE drift is a leak -- free memory going DOWN as forks happen.
    # A negative drift is more memory free at the end than at the start, which
    # is the system settling further, not memory being lost.
    if [ "$DRIFT" -gt "$LIMIT" ]; then
        bad "free-frame count fell by $DRIFT over $FORKS forks (limit $LIMIT) -- a leak"
    else
        say "PASS: $FORKS forks did not lose frames (drift $DRIFT, limit $LIMIT)"
    fi
    [ "${MAXBUGS:-0}" = "0" ] || bad "the allocator reported ${MAXBUGS} invariant violations"
fi

# --- 3. fault containment ------------------------------------------------
if ! grep -aq "app exception" "$LOG"; then
    bad "the deliberate ring-3 fault did not happen (containment was not exercised)"
elif ! grep -aq "MM_ALIVE_AFTER_FAULT" "$LOG"; then
    bad "the shell did not answer after a ring-3 fault -- containment REGRESSED"
elif ! grep -aq "LogitOS x86_64" "$LOG"; then
    bad "a fork+exec after the fault did not work -- the kernel did not survive intact"
else
    say "PASS: a ring-3 fault killed only the faulting app; shell + kernel survived"
    grep -a "app exception" "$LOG" | sed 's/.*\[fault\]/[fault]/' | head -1
fi

# --- 4. the fork cost ----------------------------------------------------
LAST="$(grep -a '\[mm\] fork:' "$LOG" | tail -1)"
if [ -n "$LAST" ]; then
    SHARED=$(echo "$LAST" | sed -n 's/.* \([0-9]*\) pages shared.*/\1/p')
    COPIED=$(echo "$LAST" | sed -n 's/.* \([0-9]*\) copied.*/\1/p')
    KC=$(echo     "$LAST" | sed -n 's/.* \([0-9]*\) kcycles.*/\1/p')
    NFK=$(echo    "$LAST" | sed -n 's/.* \([0-9]*\) forks.*/\1/p')
    [ "${NFK:-0}" -gt 0 ] || NFK=1
    echo "FORK COST over ${NFK} forks: $((SHARED / NFK)) pages shared per fork, $((COPIED / NFK)) copied per fork, ${KC} kcycles per fork"
    if grep -aq "cow=on" "$LOG"; then
        TOT=$((SHARED + COPIED))
        if [ "${COPIED:-1}" -gt $((TOT / 10 + 1)) ]; then
            bad "copy-on-write is on but fork still copied ${COPIED} of ${TOT} pages"
        else
            say "PASS: with copy-on-write on, fork copied ${COPIED} of ${TOT} pages"
        fi
    else
        say "NOTE: copy-on-write is OFF (page-fault hook not wired -- c/kernel/mm/mm.h)."
        say "      The numbers above are the EAGER baseline to compare against."
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "----- serial tail -----"
    tail -60 "$LOG"
    echo "-----------------------"
    [ -n "${MM_LOG:-}" ] || rm -f "$LOG"
    exit 1
fi
[ -n "${MM_LOG:-}" ] || rm -f "$LOG"
echo "PASS: mm on-device test"
