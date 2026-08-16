#!/usr/bin/env bash
# Headless decode-throughput measurement: boot LogitOS ONCE PER STREAM, run
# /bin/vidbench on it, and print every "VIDBENCH ..." line it writes to the
# serial console.
#
# ONE BOOT PER STREAM, DELIBERATELY -- found by this script, not assumed.
# Two earlier shapes were tried and both produced a stream that had already
# been shown to decode in ~4.5 s (H.265 320x240, alone) instead hanging past a
# 600 s budget:
#   (1) one process, all 8 streams as one `/bin/vidbench a.h264 b.h265 ...`
#       argv -- any H.264 stream decoded immediately before an H.265 one in
#       the SAME PROCESS made the H.265 decode never finish.
#   (2) one boot, 8 SEPARATE `/bin/vidbench <file>` process launches (a fresh
#       address space each time via execve) -- the hang reproduced anyway, so
#       it is not a mini-libc arena-fragmentation artifact of one process; it
#       is tied to something in the shared BOOT SESSION (kernel-side state, a
#       background self-test that fires on an uptime timer and contends the
#       BKL, or similar) that this benchmark does not need to root-cause to
#       report an honest number.
# A fresh QEMU boot per stream sidesteps whatever the shared-session state is,
# at the cost of ~8x the boot overhead -- worth it for a number that is
# actually the decoder's speed rather than an artifact of what ran earlier in
# the same session. This is reported as a real, reproducible finding (not
# silently engineered around): CLAUDE.md's own rule is that a refusal, or in
# this case a lengths-gone-to, prints why.
#
# Usage: run-vidbench-test.sh <iso> <disk.img> [stream-path-on-disk ...]
# With no stream arguments, runs the whole 8-stream matrix (one boot each).
# Prints one "VIDBENCH ..." line per stream and a final summary; exits
# nonzero only if EVERY stream failed to produce a line (total failure, as
# opposed to one slow/refused stream, which is reported and does not fail the
# script -- see c/apps/video/vidbench.c's own comment on refusing loudly).

set -u

ISO="${1:?usage: run-vidbench-test.sh <iso> <disk.img> [streams...]}"
DISK="${2:?usage: run-vidbench-test.sh <iso> <disk.img> [streams...]}"
shift 2
QEMU="${QEMU:-qemu-system-x86_64}"
# Budget per stream, in seconds. Generous: 1080p H.264/H.265 under TCG is the
# slow case this exists to measure, and this now also has to absorb a full
# boot each time. Override with VIDBENCH_POLL_S=<seconds>.
POLL_S="${VIDBENCH_POLL_S:-120}"

if [ "$#" -eq 0 ]; then
    set -- /media/vidbench/h264-320x240.h264 /media/vidbench/h265-320x240.h265 \
           /media/vidbench/h264-640x360.h264 /media/vidbench/h265-640x360.h265 \
           /media/vidbench/h264-1280x720.h264 /media/vidbench/h265-1280x720.h265 \
           /media/vidbench/h264-1920x1080.h264 /media/vidbench/h265-1920x1080.h265
fi

ok=0
fail=0
for stream in "$@"; do
    LOG="$(mktemp)"
    { sleep 4; printf '/bin/vidbench %s\nexit\n' "$stream"; sleep "$POLL_S"; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
        -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
        -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
        >"$LOG" 2>/dev/null &
    QPID=$!

    STEPS=$(( POLL_S * 10 ))
    for _ in $(seq 1 "$STEPS"); do
        grep -aq "VIDBENCH-DONE" "$LOG" && break
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    kill "$QPID" 2>/dev/null
    wait "$QPID" 2>/dev/null

    LINE="$(grep -a "^VIDBENCH " "$LOG" | tail -1 | tr -d '\r')"
    if [ -n "$LINE" ]; then
        echo "$LINE"
        ok=$((ok + 1))
    else
        ERRLINE="$(grep -a "^VIDBENCH-ERR" "$LOG" | tail -1 | tr -d '\r')"
        if [ -n "$ERRLINE" ]; then
            echo "$ERRLINE"
            ok=$((ok + 1))
        else
            echo "VIDBENCH-TIMEOUT $stream: no result within ${POLL_S}s"
            fail=$((fail + 1))
        fi
    fi
    rm -f "$LOG"
done

echo "--- vidbench guest summary: $ok reported, $fail timed out (of $#) ---"
[ "$ok" -gt 0 ]
