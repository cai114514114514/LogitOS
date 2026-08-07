#!/usr/bin/env bash
# Does the monotonic clock stay monotonic across cores?
#
# THE PROPERTY. Not "the clock advances" -- that is run-time-test.sh. This is:
# a read on core B that happens AFTER a read on core A never returns a smaller
# number. It is a separate test because it has a separate failure mode with a
# separate cause: the TSC is per-CPU, and on real hardware the counters on
# different cores are not started at the same instant. A kernel that reads its
# own core's TSC and subtracts hands the caller a NEGATIVE interval, which every
# timeout in this tree would read as an enormous positive one.
#
# HOW. The kernel spawns one probe thread per core at ~10 s of uptime; each
# reads time_mono_ns() a hundred thousand times, yielding often so the scheduler
# migrates it, and records the core it is on plus any regression it observes.
# The last one to finish prints the totals. Two numbers matter:
#   observed-backsteps  regressions that reached a CALLER   -- must be 0
#   clamped             regressions the clock absorbed      -- reported, not
#                       asserted: nonzero means this machine's TSCs really are
#                       skewed and the clamp is doing its job
#
# Also runs the uniprocessor path (-smp 1), because the fallback logic and the
# probe's thread-count arithmetic are different there and nothing else covers it.

set -u

ISO="${1:?usage: run-time-smp-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-time-smp-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

LOG="$(mktemp)"
QPID=""
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

boot() {                       # boot() <ncpu>  -> fills $LOG
    : >"$LOG"
    { sleep 30; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
        -boot d -snapshot -m 512M -smp "$1" -accel tcg,thread=multi -rtc base=localtime \
        -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
        >"$LOG" 2>/dev/null &
    QPID=$!
    for _ in $(seq 1 600); do
        grep -aq "\[time\] smp-mono" "$LOG" && break
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""
}

fail() { echo "FAIL: $1"; echo "----- serial (time lines) -----"; grep -a "\[time\]" "$LOG"; echo "-------------------------------"; exit 1; }

field() { echo "$1" | sed -n "s/.*$2=\([0-9a-fx]*\).*/\1/p"; }

check() {                      # check() <ncpu> <min distinct cores>
    local n="$1" wantcores="$2" line back reads seen ncores
    boot "$n"
    # No '^': the kernel shares the serial console with /bin/sh, so a line can
    # arrive with a "/ $ " prompt already on it.
    line="$(grep -a '\[time\] smp-mono' "$LOG" | tail -1 | tr -d '\r')"
    [ -n "$line" ] || fail "-smp $n: the monotonicity probe never reported"

    reads="$(field "$line" reads)"
    back="$(field  "$line" observed-backsteps)"
    seen="$(field  "$line" seen)"
    [ -n "$reads" ] && [ "$reads" -gt 0 ] 2>/dev/null \
        || fail "-smp $n: the probe made ${reads:-no} clock reads, so nothing was tested"
    [ "$back" = "0" ] \
        || fail "-smp $n: the monotonic clock WENT BACKWARDS $back times across $reads reads"

    # popcount of the core bitmap: how many distinct cores actually read it.
    ncores=0
    local v=$((0x${seen#0x}))
    while [ "$v" -gt 0 ]; do ncores=$(( ncores + (v & 1) )); v=$(( v >> 1 )); done
    [ "$ncores" -ge "$wantcores" ] \
        || fail "-smp $n: only $ncores distinct core(s) read the clock (want >= $wantcores); a cross-core claim needs cross-core reads"

    echo "  -smp $n: $reads reads from $ncores core(s), 0 regressions -- $line"
}

check 4 2      # the real claim: several cores, no regression
check 1 1      # and the uniprocessor path still works

echo "PASS: the monotonic clock never went backwards, on 4 cores or on 1"
exit 0
