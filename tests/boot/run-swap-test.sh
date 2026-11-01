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
# c/kernel/mm/reclaim/swap.c -- it takes any registered block device that is not the
# root and is blank -- so SWAPBUS=ahci still works and is worth keeping for
# testing the slow path.
#
# The image is zeroed before every run. c/kernel/mm/reclaim/swap.c refuses any device
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
# fill phase formerly used a ring-3 bytecode interpreter writing a pattern into
# every word of every page under TCG, and on a slow machine `mid` did not reach the
# watermark inside 300 s -- which fails as "the workload did not finish",
# indistinguishable at a glance from reclaim being broken. It is not the same
# thing at all, so the budget is overridable rather than something to discover
# by reading this file: SWAP_WAIT=900 make test-swap.
WAIT="${SWAP_WAIT:-$WAIT}"
# 150 seconds for bootwait plus 60 for the post-workload fork/exec and audit
# commands below. The old fixed 600-second outer cap silently cut SWAP_WAIT=900
# short. This is a host harness deadline, not a hardware timing measurement.
RUN_WAIT=$((WAIT + 210))

QEMU="${QEMU:-qemu-system-x86_64}"
SWAPIMG="${SWAPIMG:-$(dirname "$DISK")/swap.img}"
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
case "$SWAPBUS" in
    nvme|ahci) ;;
    *)
        echo "FAIL: SWAPBUS must be nvme or ahci"
        exit 1
        ;;
esac
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

# Wait for the native workload to finish before asking the shell to fork again.
# MEMPRESS-DONE is after verification AND unmap in mempress.as; an app fault is
# terminal too, but cannot satisfy MEMPRESS-OK below. The old unconditional
# sleep delayed short runs and could type follow-up probes into a long run.
# UART input echo is never completion evidence. A deadline still lets a hung
# workload fail the original liveness assertions instead of hanging this gate.
wait_for_pressure()
{
    local deadline=$((SECONDS + $1))
    while true; do
        if grep -aEq $'^MEMPRESS-DONE\r?$|^\\[fault\\] app exception: .* -- terminating app' "$LOG"; then
            return 0
        fi
        if [ "$SECONDS" -ge "$deadline" ]; then
            return 1
        fi
        # Poll the serial log four times a second: the workload runs for minutes, so
        # this adds at most 250 ms to a result while keeping the host loop idle.
        sleep 0.25
    done
}

# The shell script fed to the serial console. mempress prints its own markers;
# the mmctl calls afterwards make the kernel dump its counters and run both
# audits, so the assertions below are about numbers the kernel keeps rather than
# about the absence of a crash. Correction: the old echo-generated mmaudit/mmrep
# were AS2 expressions; that engine is gone. The A3 probes are host-compiled by
# tests/astyped.mk and packed into /usr/as/bin alongside mempress. Never ask the
# device launcher to build a pressure workload while memory is exhausted.
{
  # The AHCI capture printed "ogitOS shell -- type 'help'": UART output lost
  # the first banner byte. Its complete ready suffix is enough to identify the
  # same shell milestone without spending the whole boot timeout on that byte.
  logit_wait_for_shell "$LOG" 150 "shell -- type 'help'"
  printf 'echo MEM_BEFORE\n'
  sleep 1
  printf '/usr/as/bin/mempress.aex %s\n' "$SIZE"
  wait_for_pressure "$WAIT"
  printf 'echo MEM_AFTER\n'
  sleep 2
  # Still alive? A fork+exec after the pressure is the real liveness test: it
  # needs an address space, a stack and a page fault each, so a kernel that
  # survived by luck fails here.
  printf 'uname\n'
  sleep 4
  printf 'echo MEM_ALIVE\n'
  sleep 2
  printf '/usr/as/bin/mmaudit.aex\n'
  sleep 8
  printf '/usr/as/bin/mmrep.aex\n'
  sleep 8
  printf 'echo MEM_DONE\n'
  sleep 3
  printf 'exit\n'
  sleep 2
} | $QEMU -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      $SWAPDEV \
      -m "${RAM}M" ${SWAP_SMP:--smp 4 -accel tcg,thread=multi} \
      -vga none -device virtio-gpu-pci \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>&1 &
QPID=$!

RUN_DEADLINE=$((SECONDS + RUN_WAIT))
while [ "$SECONDS" -lt "$RUN_DEADLINE" ]; do
    # The serial shell echoes input before executing it. An echoed
    # "echo MEM_DONE" must not terminate QEMU before the final command ran.
    # The UART uses CRLF, so accept the output with or without its CR byte.
    grep -aEq $'^MEM_DONE\r?$' "$LOG" && break
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

# Read an exact counter key from the requested stats row. Substring matching
# would confuse swapin with another counter whose name ends in the same text.
stat_of() {
    awk -v key="$1=" -v row="$2" '/\[mmstat\]/ {
        count++
        if (count == row) {
            for (i = 1; i <= NF; i++) {
                if (index($i, key) == 1) {
                    print substr($i, length(key) + 1)
                }
            }
        }
    }' "$LOG"
}

BOOTED=$(grep -ac "LOGIT_BOOT_OK" "$LOG")
[ "${BOOTED:-0}" -gt 0 ] || bad "the kernel did not boot at ${RAM} MiB"

# ---------------------------------------------------------------- mode: swap
if [ "$MODE" = "swap" ]; then
    # The old ^[swap] anchor assumed every kernel message began a fresh line.
    # In the AHCI serial run the shell printed its unterminated "/ $ " prompt
    # before swap initialization, hiding a real 98,303-slot device. Accept only
    # that exact optional prompt, on BOTH device and catastrophic-root checks;
    # arbitrary preceding text must not turn an echoed command into evidence.
    grep -aEq '^(/ \$ )?\[swap\] '"$SWAPNAME"': [0-9]+ slots' "$LOG" \
        || bad "swap did not come up on $SWAPNAME"
    # It must NOT have picked the root disk. This is the one mistake in the whole
    # line that destroys data rather than losing it, so it is asserted on the
    # device name and not merely trusted to the rules in swap.c.
    grep -aEq '^(/ \$ )?\[swap\] vblk0' "$LOG" \
        && bad "swap selected the ROOT filesystem device -- catastrophic"

    if ! grep -aq "MEMPRESS-OK" "$LOG"; then
        # 2026-11-01 QEMU diagnosis, not verified on X79-VG4 hardware:
        # DATA readback faults at 0x50000c38 (verify_data's movq (%rax),%rax).
        # GDB stopped in reclaim_swapin at its failed second reserve allocation
        # (reclaim.c:701), before swap_read_page, with sw_errors=0 and 22,487
        # reserve hits. Successful disk transfers therefore do not settle this
        # failure: mm must provide a frame to restore. Keep the full pressure
        # and byte-check assertions; shrinking the workload would hide it.
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
    grep -a "^\[swap\] on demand: CPU polling" "$LOG" | tail -1

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
    # Absence of success without a started pressure process is a broken
    # apparatus, not the no-swap control. Keep the original containment checks.
    grep -aq '^MEMPRESS-START ' "$LOG" \
        || bad "the no-swap workload never started"
    grep -aq "\[swap\] no eligible device" "$LOG" \
        || bad "swap was expected to be absent but came up anyway"

    if grep -aq "MEMPRESS-OK" "$LOG"; then
        bad "the workload SUCCEEDED without a swap device."
        echo "      That means the pressure never actually needed swap, so the"
        echo "      positive run does not prove swap works. Raise the workload"
        echo "      (5th argument) or lower the RAM (4th)."
    else
        say "PASS (control): without swap the workload could not complete, as required"
        grep -a "MEMPRESS" "$LOG" | tail -2 | awk '{ print "      " $0 }'
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
