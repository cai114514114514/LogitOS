#!/usr/bin/env bash
# On-device reclaim + swap test: run LogitOS on a machine that is too small for
# the work it is asked to do, and require it to keep going and to keep the bytes.
#
# ---------------------------------------------------------------------------
# MANUFACTURING THE PRESSURE, AND WHY IT HAS TO BE MANUFACTURED
#
# Nothing on this machine is short of memory. The full desktop -- nine apps, the
# browser on a real page, networking live -- peaks at 229 MiB of 511, and the
# frame allocator's audit is clean. A reclaim path tested on that system would
# never run, and a reclaim path that has never run is not a reclaim path.
#
# So the machine is made small instead of the workload made huge: the same
# kernel, the same disk, the same programs, booted with a fraction of the RAM
# and given a ring-3 program (/usr/as/examples/mempress.as) that maps more
# memory than physically exists and writes to every page of it. Physical memory
# genuinely runs out, in the middle of ordinary page faults, with the window
# manager and the shell running underneath. That is the condition reclaim exists
# for, and it is reproducible from a single command.
#
# ---------------------------------------------------------------------------
# THE SWAP DEVICE
#
# A blank raw image on a SEPARATE CONTROLLER from the virtio-blk disk the
# filesystem is mounted on. That separation is deliberate and is what makes swap
# I/O safe to serialise on its own: c/drivers/block/ahci.c documents that logitfs
# relies on the big kernel lock never being dropped mid-operation, and swap never
# touches logitfs or the device it lives on.
#
# NVMe by default rather than AHCI, for a measured reason: AHCI here is a
# submit-and-poll command per 4 KiB, and a thrashing run moves tens of thousands
# of pages, which took longer than the harness was willing to wait. NVMe is a
# real DMA submission queue and the same workload finishes. Both are accepted by
# c/kernel/mm/swap.c -- it takes any registered block device that is not the
# root and is blank -- so SWAPBUS=ahci still works and is worth keeping for
# testing the slow path.
#
# The image is zeroed before every run. c/kernel/mm/swap.c refuses any device
# whose first sector is neither blank nor its own header, so a disk with
# anything at all on it is left alone -- which is the behaviour to preserve, and
# the reason this script creates its own image rather than pointing swap at
# something that already exists.
#
# ---------------------------------------------------------------------------
# MODES
#
#   swap     (default) with the swap device. The pressure must be survived and
#            every page must come back byte-identical.
#   noswap   THE NEGATIVE CONTROL. Same kernel, same RAM, same program, no swap
#            device. The pages under pressure hold data, so the cheap
#            drop-the-page tier cannot take them; without somewhere to put them
#            the machine MUST fail to complete the workload. If this mode
#            passes, the "swap" run proved nothing -- it would mean the pressure
#            was never real.
#
# Usage: run-swap-test.sh <iso> <disk.img> [swap|noswap] [ram-MiB] [size-name]
set -u

# Wait for /bin/sh to exist before typing at it, rather than sleeping a
# guessed number of seconds. See tests/boot/bootwait.sh for why a longer
# sleep is the same bug with a bigger number.
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-swap-test.sh <iso> <disk.img> [swap|noswap] [ram] [size]}"
DISK="${2:?usage: run-swap-test.sh <iso> <disk.img> [swap|noswap] [ram] [size]}"
MODE="${3:-swap}"
RAM="${4:-192}"
SIZE="${5:-mid}"

# How long to give the workload. Every DATA page that gets evicted is a 4 KiB
# write now and a 4 KiB read later on a polled AHCI disk -- and with no swap
# cache, a page that is evicted, read back and evicted again is written twice.
# That is the thrashing cost, it is real, and it is why this scales with the
# workload instead of being one number that is either too short for the big case
# or wastes minutes on the small one.
case "$SIZE" in
    small) WAIT=60  ;;
    mid)   WAIT=300 ;;
    big)   WAIT=600 ;;
    huge)  WAIT=900 ;;
    *)     WAIT=300 ;;
esac
# ...but the numbers above are a HOST speed as much as a workload size. The
# fill phase is a ring-3 bytecode interpreter writing a pattern into every word
# of every page under TCG, and on a slow machine `mid` does not reach the
# watermark inside 300 s -- which fails as "the workload did not finish",
# indistinguishable at a glance from reclaim being broken. It is not the same
# thing at all, so the budget is overridable rather than something to discover
# by reading this file: SWAP_WAIT=900 make test-swap.
WAIT="${SWAP_WAIT:-$WAIT}"

QEMU="${QEMU:-qemu-system-x86_64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SWAPIMG="${SWAPIMG:-$ROOT/build/swap.img}"
SWAPMIB="${SWAPMIB:-384}"
LOG="${SWAP_LOG:-$(mktemp)}"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

fail=0
say() { echo "$1"; }
bad() { echo "FAIL: $1"; fail=1; }

# A blank image every time: swap must claim a device it recognises as free, and
# a leftover header from a previous run would let a bug in the "is this disk
# safe to use" check pass unnoticed.
mkdir -p "$(dirname "$SWAPIMG")"
dd if=/dev/zero of="$SWAPIMG" bs=1M count="$SWAPMIB" status=none

SWAPBUS="${SWAPBUS:-nvme}"
SWAPDEV=""
SWAPNAME=""
if [ "$MODE" = "swap" ]; then
    if [ "$SWAPBUS" = "ahci" ]; then
        SWAPDEV="-device ich9-ahci,id=ahci0 \
                 -drive file=$SWAPIMG,format=raw,if=none,id=swp,file.locking=off \
                 -device ide-hd,drive=swp,bus=ahci0.0"
        SWAPNAME="ahci0"
    else
        SWAPDEV="-drive file=$SWAPIMG,format=raw,if=none,id=swp,file.locking=off \
                 -device nvme,drive=swp,serial=logitswap"
        SWAPNAME="nvme0"
    fi
fi

NET="-netdev user,id=n0 -device e1000,netdev=n0"

echo "=== mode=$MODE  ram=${RAM}MiB  workload=$SIZE  swap=${SWAPMIB}MiB ==="

# The shell script fed to the serial console. mempress prints its own markers;
# the mmctl calls afterwards make the kernel dump its counters and run both
# audits, so the assertions below are about numbers the kernel keeps rather than
# about the absence of a crash.
{
  logit_wait_for_shell "$LOG" 150
  printf 'echo MEM_BEFORE\n';                                     sleep 1
  printf 'as /usr/as/examples/mempress.as %s\n' "$SIZE";           sleep "$WAIT"
  printf 'echo MEM_AFTER\n';                                      sleep 2
  # Still alive? A fork+exec after the pressure is the real liveness test: it
  # needs an address space, a stack and a page fault each, so a kernel that
  # survived by luck fails here.
  printf 'uname\n';                                               sleep 4
  printf 'echo MEM_ALIVE\n';                                      sleep 2
  printf 'echo print(syscall(94,0,2,0)) > /mmaudit.as\n';         sleep 2
  printf 'as /mmaudit.as\n';                                      sleep 8
  printf 'echo print(syscall(94,0,1,0)) > /mmrep.as\n';           sleep 2
  printf 'as /mmrep.as\n';                                        sleep 8
  printf 'echo MEM_DONE\n';                                       sleep 3
  printf 'rm /mmaudit.as\nrm /mmrep.as\nexit\n';                  sleep 2
} | $QEMU -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      $SWAPDEV \
      -m "${RAM}M" ${SWAP_SMP:--smp 4 -accel tcg,thread=multi} \
      -vga none -device virtio-gpu-pci \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 2400); do
    grep -aq "MEM_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.25
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "----- swap bring-up -----"
grep -a -e '^\[swap\]' -e '^\[rmap\]' -e '^\[reclaim\]' "$LOG" | head -8
echo "----- mempress -----"
grep -a "MEMPRESS" "$LOG" | head -12
echo "----- kernel counters -----"
grep -a "\[mmstat\]" "$LOG" | tail -2
echo "-------------------------"

stat_of() {   # stat_of <key> <which-line: 1=first 2=last>
    grep -a "\[mmstat\]" "$LOG" | sed -n "${2}p" | sed -n "s/.*[^_a-z]$1=\([0-9-]*\).*/\1/p"
}

BOOTED=$(grep -ac "LOGIT_BOOT_OK" "$LOG")
[ "${BOOTED:-0}" -gt 0 ] || bad "the kernel did not boot at ${RAM} MiB"

# ---------------------------------------------------------------- mode: swap
if [ "$MODE" = "swap" ]; then
    grep -aq "^\[swap\] $SWAPNAME: [0-9]* slots" "$LOG" \
        || bad "swap did not come up on $SWAPNAME"
    # It must NOT have picked the root disk. This is the one mistake in the whole
    # line that destroys data rather than losing it, so it is asserted on the
    # device name and not merely trusted to the rules in swap.c.
    grep -aq "^\[swap\] vblk0" "$LOG" \
        && bad "swap selected the ROOT filesystem device -- catastrophic"

    if ! grep -aq "MEMPRESS-OK" "$LOG"; then
        if grep -aq "MEMPRESS-CORRUPT" "$LOG"; then
            bad "PAGES CAME BACK WRONG -- this is the failure the whole line exists to prevent"
            grep -a "MEMPRESS-BAD" "$LOG" | head -6
        else
            bad "the workload did not complete (no MEMPRESS-OK)"
        fi
    else
        say "PASS: every page of the workload came back byte-identical"
    fi

    grep -aq "MEMPRESS-ZERO-VERIFIED 0" "$LOG" \
        || bad "the zero region did not come back as zeroes -- the free drop tier is wrong"

    EV=$(stat_of evicted 2); SW=$(stat_of swapped 2); DR=$(stat_of dropped 2)
    IN=$(stat_of swapin 2);  BUGS=$(stat_of bugs 2)
    SECOND=$(stat_of second 2); FAILA=$(stat_of allocfail 2)

    # THE ASSERTION THAT MAKES THE REST MEAN ANYTHING: reclaim actually ran.
    # A test that never reaches the mechanism proves nothing about it.
    if [ "${EV:-0}" -le 0 ]; then
        bad "reclaim never evicted a page -- the pressure did not reach the mechanism"
    else
        say "PASS: reclaim ran -- ${EV} frames evicted (${DR} dropped free, ${SW} written to swap)"
    fi
    [ "${SW:-0}" -gt 0 ] || bad "nothing was written to the swap device"
    [ "${IN:-0}" -gt 0 ] || bad "nothing was ever read back from the swap device"
    # BOTH tiers, not just the expensive one. A run where every eviction went to
    # the disk means the cheap tier never fired, and the cheap tier is the half
    # that costs nothing -- so it silently not working looks exactly like it
    # working, in every measure except this one.
    [ "${DR:-0}" -gt 0 ] \
        || bad "nothing was reclaimed for free -- the drop tier never fired"

    # THRASHING, REPORTED. A machine that survives by moving megabytes should
    # say how many, not merely appear to work.
    KIB=$(( (${SW:-0} + ${IN:-0}) * 4 ))
    say "THRASH: ${SW:-0} pages out + ${IN:-0} pages in = ${KIB} KiB moved; ${SECOND:-0} second chances"
    grep -a "^\[swap\] on demand: BKL held" "$LOG" | tail -1

    [ "${BUGS:-1}" = "0" ] || bad "the kernel reported ${BUGS} memory-management bugs"

    grep -aq "MEM_ALIVE" "$LOG" || bad "the shell stopped answering after the pressure"
    grep -aq "LogitOS x86_64" "$LOG" || bad "fork+exec did not work after the pressure"
    [ "$fail" -eq 0 ] && say "PASS: the machine survived and kept working"

    AUD=$(grep -a "\[mm\] audit:" "$LOG" | tail -1)
    if [ -z "$AUD" ]; then
        bad "the on-device audit did not run"
    else
        echo "  $AUD"
        echo "$AUD" | grep -q "audit: 0 inconsistencies" \
            || bad "the frame allocator and the reverse map disagree"
        echo "$AUD" | grep -q "0 reverse-map bugs" || bad "the reverse map reported bugs"
    fi
    say "(allocation refusals during the run: ${FAILA:-?})"
fi

# -------------------------------------------------------------- mode: noswap
if [ "$MODE" = "noswap" ]; then
    grep -aq "\[swap\] no eligible device" "$LOG" \
        || bad "swap was expected to be absent but came up anyway"

    if grep -aq "MEMPRESS-OK" "$LOG"; then
        bad "the workload SUCCEEDED without a swap device."
        echo "      That means the pressure never actually needed swap, so the"
        echo "      positive run does not prove swap works. Raise the workload"
        echo "      (5th argument) or lower the RAM (4th)."
    else
        say "PASS (control): without swap the workload could not complete, as required"
        grep -a "MEMPRESS" "$LOG" | tail -2 | sed 's/^/      /'
    fi

    # The control also has to show the machine SURVIVED the failure. Running out
    # of memory must kill the process that asked for it and nothing else.
    grep -aq "MEM_ALIVE" "$LOG" \
        || bad "the kernel did not survive running out of memory -- containment regressed"
    grep -aq "LogitOS x86_64" "$LOG" \
        || bad "fork+exec did not work after the out-of-memory kill"
    [ "$fail" -eq 0 ] && say "PASS (control): the kernel and the shell survived the out-of-memory"
fi

if [ "$fail" -ne 0 ]; then
    echo "----- serial tail -----"
    tail -50 "$LOG"
    echo "-----------------------"
    echo "  full log: $LOG"
    exit 1
fi
[ -n "${SWAP_LOG:-}" ] || rm -f "$LOG"
echo "PASS: on-device reclaim + swap ($MODE)"
