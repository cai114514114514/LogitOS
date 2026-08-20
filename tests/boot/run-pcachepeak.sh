#!/usr/bin/env bash
# R5: HOW CLOSE DOES A REAL WORKLOAD GET TO THE PAGE CACHE'S CEILING?
#
# pcache.c:139-142 sizes the pool min(PCACHE_MAXPAGE=4096, total_frames/16),
# which on the standard 512 MiB boot clamps to 4096 pages = 16 MiB, SYSTEM-WIDE.
# pcache.c:546-554 is the part that makes the number matter: when every slot
# holds a mapped page, a new fault still SUCCEEDS and is handed back UNCACHED.
# That is a silent loss of the whole feature, not an error -- so the ceiling is
# only ever visible as a number, never as a failure.
#
# Until elf_load started producing file VMAs there was no consumer, so the
# ceiling had never been approached by anything. This boots the desktop and
# then executes every large binary on the disk in turn, which is the heaviest
# text-paging workload this machine can be made to do from a shell, and reads
# the peak off pcache_report().
#
# It DELIBERATELY does not force a reclaim: a forced pass drops cached pages and
# would suppress the very number being measured (in run-elfshare.sh's session
# the two forced passes took resident from 49 to 4 -- correct behaviour, wrong
# instrument for this question).
#
# Usage:  bash tests/boot/run-pcachepeak.sh <iso> <disk.img> [label]
# Makefile equivalent (NOT wired -- the Makefile is owned by another line):
#     test-pcachepeak: $(ISO) $(DISK)
#         @bash tests/boot/run-pcachepeak.sh $(ISO) $(DISK)
set -u

. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-pcachepeak.sh <iso> <disk.img> [label]}"
DISK="${2:?usage: run-pcachepeak.sh <iso> <disk.img> [label]}"
LABEL="${3:-run}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${PCACHEPEAK_LOG:-$(mktemp)}"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

w() { logit_wait_for_marker "$LOG" "$1" "${2:-150}"; }

# The large binaries that are on the disk AND terminate on their own. Each is
# named rather than globbed so that a failure to find one is visible in the log
# as that program's own error, not as a silently shorter workload.
#
# Overridable, because the GUI apps live at the ROOT of the disk and are not on
# this list: they are launched by the Dock, not by a shell, and reaching the
# Dock needs QMP mouse injection. execve'ing one anyway is still worth a run --
# it dies as soon as it asks for a window, but exec_note_load() has already
# printed what the loader did with its pages, which is the only thing being
# asked here. browser.aex is the biggest binary in the tree and is otherwise
# unmeasurable from a serial console.
BIG="${PCACHEPEAK_BINS:-/bin/libctest /bin/libctest2 /bin/libmcheck /bin/thrtest /bin/imgcheck \
     /bin/audiocheck /bin/h2check /bin/vidcheck /bin/asnative /bin/jsbench /bin/lm}"

{
  logit_wait_for_shell "$LOG" 200
  # The desktop alone first: this reading is "boot + desktop", the number R5
  # actually asks for, taken before the shell has run anything large.
  printf '/bin/as /usr/as/examples/elfstat.as DESKTOP\n';  w "ELFSTAT-END DESKTOP"

  for b in $BIG; do
      printf '%s\n' "$b" ; sleep 6
  done
  printf 'echo PEAK_BIGDONE\n';                            w "PEAK_BIGDONE" 300
  printf '/bin/as /usr/as/examples/elfstat.as AFTERBIG\n'; w "ELFSTAT-END AFTERBIG"
  printf 'echo PCACHEPEAK_DONE\n'; sleep 3
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      -netdev user,id=n0 -device e1000,netdev=n0 \
      -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 9000); do
    grep -aq "PCACHEPEAK_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "=================== pcachepeak: $LABEL ==================="
echo "log: $LOG"
echo "-- every image this session loaded, and how it was loaded"
grep -a "\[exec\] load " "$LOG" | sed 's/.*\[exec\]/   [exec]/' | tail -30
echo
echo "-- loader totals"
grep -a "\[exec\] loader:" "$LOG" | tail -1 | sed 's/.*\[exec\]/   [exec]/'
echo
echo "-- the ceiling"
grep -a "pages resident (peak" "$LOG" | sed 's/.*\[pcache\]/   [pcache]/'
echo "   ceiling line at boot:"
grep -a "\[pcache\] up:" "$LOG" | sed 's/^/   /'
echo "==========================================================="
grep -aq "PCACHEPEAK_DONE" "$LOG" || { echo "FAIL: the guest never reached PCACHEPEAK_DONE"; exit 1; }
exit 0
