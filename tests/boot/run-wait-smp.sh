#!/usr/bin/env bash
# M27 blocking-core proof, ON DEVICE, under real preemption and real SMP.
#
# The kernel runs c/kernel/core/wait_selftest.c as an ordinary ring-0 thread at
# boot. It exercises the wait queues, the sleeping locks and the deferred-work
# chain against the real scheduler on real cores, and prints WAITQ_SELFTEST_OK
# only if every phase held. This harness boots at several core counts and
# requires the marker each time: this bug class does not appear on one core or
# without contention, so a single -smp 4 pass is not the whole claim.
#
# It also echoes the sleeping-versus-spinning measurement, which is the number
# the whole milestone is judged on: over the same 200 ms wait, the pre-M27
# mechanism (bkl_hlt_wait) re-tests its condition ~100 times a second, and a
# blocked thread is dispatched ZERO times.
#
# Usage: run-wait-smp.sh <iso> <disk.img> [core counts...]
set -u

ISO="${1:?usage: run-wait-smp.sh <iso> <disk.img> [smp counts]}"
DISK="${2:?usage: run-wait-smp.sh <iso> <disk.img> [smp counts]}"
shift 2
CORES="${*:-1 2 4}"
QEMU="${QEMU:-qemu-system-x86_64}"

rc=0
for N in $CORES; do
    LOG="$(mktemp)"
    "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
        -m 512M -smp "$N" -accel tcg,thread=multi \
        -vga none -device virtio-gpu-pci \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 &
    QPID=$!

    ok=0
    # ~60 s: the self-test starts after the APs are up and runs about 2 s.
    for _ in $(seq 1 600); do
        if grep -aq "WAITQ_SELFTEST_OK" "$LOG"; then ok=1; break; fi
        if grep -aq "WAITQ_SELFTEST_FAIL" "$LOG"; then ok=0; break; fi
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

    if [ "$ok" = 1 ]; then
        echo "PASS: -smp $N"
        grep -a "\[waitq\]" "$LOG" | sed 's/^/      /'
    else
        echo "FAIL: -smp $N -- no WAITQ_SELFTEST_OK"
        grep -aE "\[waitq\]|WAITQ_SELFTEST" "$LOG" | sed 's/^/      /'
        # A boot that never reached the self-test is a different failure from a
        # self-test that reported a problem; show enough to tell them apart.
        grep -aq "LOGIT_BOOT_OK" "$LOG" || echo "      (the kernel never reached LOGIT_BOOT_OK)"
        rc=1
    fi
    rm -f "$LOG"
done

[ "$rc" = 0 ] && echo "PASS: the blocking core holds on every core count tested"
exit $rc
