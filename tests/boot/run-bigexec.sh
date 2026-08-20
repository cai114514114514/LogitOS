#!/usr/bin/env bash
# HOW BIG A PROGRAM THIS MACHINE CAN LOAD -- on the machine, twice per boot.
#
# WHAT IS BEING MEASURED. The loader used to materialise the whole file in one
# contiguous kmalloc, and the ceiling that produced was not "out of memory": it
# was kheap's grow() DOUBLING an arena and asking pmm_alloc_contig() -- a linear
# first-fit with no fallback -- for the next power of two above the file, in one
# physical run. Measured 2026-08-20 on the pre-change loader: a 128 MiB file
# took a 256 MiB arena, and a 256 MiB file was refused with 456 MiB free.
#
# WHY TWO PHASES, and why phase 2 is the one that counts. "First-fit with no
# fallback" means a load that succeeds on a machine that has just booted says
# nothing about the same load after memory has been used and released. So the
# identical three binaries are run again after a churn phase -- 112 MiB of page
# cache taken and dropped by the first three loads, an AetherScript program that
# allocates and collects, and several process create/exit cycles. Anything that
# only works on a pristine machine fails the second half.
#
# WHAT COUNTS AS OK, and it is not "it did not hang":
#   - the program prints the byte count it TOUCHED, having read the last byte,
#     the middle and the first (see tests/unit/bigexec_pad.c). A header that
#     parsed and a first page that mapped are not the claim.
#   - it prints sum=315, the sum of three bytes the generator planted at those
#     three positions. A load that maps the right number of pages holding the
#     wrong bytes -- a file-backed run trimmed by a page, a segment read from
#     the wrong offset -- prints a different number rather than crashing, and a
#     crash is the failure mode that is easy to notice anyway.
#   - the loader must not print a refusal and the kernel must not print an OOM.
#
# ONE COMMAND AT A TIME, AND WAIT FOR THE PROMPT, NOT FOR THE OUTPUT.
#
# Every other harness here pipes its whole script into QEMU at once, which works
# because every command in them is quick. These are not: a 64 MiB load is
# minutes under TCG, and bytes typed while the guest is not reading are DROPPED
# -- there is no flow control on this serial line. Measured, not feared: a
# burst-fed run of this exact script lost `/bin/pad16` completely and delivered
# `/bin/pad64` as `pad64`, so the log said "command not found" for a program
# that is on the disk. Same shape CLAUDE.md records for qmp_site.py feeding four
# scancodes into a PS/2 controller with a one-byte buffer.
#
# The second version waited for each command's OUTPUT before sending the next,
# and that was still wrong -- for a reason worth writing down, because it looks
# correct. A program prints "BIGEXEC ok" and then EXITS; between the print and
# the shell posting its next read there is a whole process teardown, and a line
# typed into that window is dropped exactly as before. The churn phase lost its
# first command that way, which is how this was found.
#
# So the signal is the SHELL PROMPT: `/ $ ` with no newline after it is written
# immediately before sh reads, and is therefore the only thing on this line that
# means "typing now will be received".

set -u
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-bigexec.sh <iso> <disk.img>}"
DISK="${2:?usage: run-bigexec.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
RAM="${RAM:-512M}"
LOG="${BIGEXEC_LOG:-build/bigexec/bigexec.log}"
mkdir -p "$(dirname "$LOG")"
rm -f "$LOG"; touch "$LOG"

# Wait until `pattern` has appeared `n` times, or give up. Counting rather than
# matching is what lets the same program be run twice in one boot and still be
# waited on: "it printed" is true from the first run onward and would let phase
# 2's command be typed into a guest still busy with phase 1.
count_of() { grep -ac -- "$1" "$LOG" 2>/dev/null || echo 0; }

# READY IS COUNTED, NOT LOOKED FOR AT THE END OF THE FILE.
#
# The obvious test is "the log ends in `/ $ `", and it was the first version.
# It hangs, and the case that hangs it is the interesting one: when the control
# refuses a load, the KERNEL prints after the prompt --
#
#     [execve] /bin/pad64: aex_load failed
#     / $ [proc] kill: pid 1 exiting          <- prompt, then two kernel lines
#     [wm] win 0 gone
#
# -- so the tail is no longer the prompt, and a harness waiting for it waits
# forever on a machine that is perfectly ready. Observed, not imagined; it cost
# a 20-minute run.
#
# So: count the prompts. sh writes exactly one `/ $ ` each time it is about to
# read, whether or not anything is printed after it, so "one more prompt than
# when I last typed" means the previous command is finished AND the shell is
# listening. That is the same thing the end-of-file test was reaching for,
# expressed as a monotonic count instead of a snapshot.
LASTP=0
prompts() { grep -aoF -- '/ $ ' "$LOG" 2>/dev/null | wc -l; }
send() {  # send <cmd> <wait-for-ready-seconds>
    local i n=$(( ${2:-60} * 10 ))
    for ((i = 0; i < n; i++)); do [ "$(prompts)" -gt "$LASTP" ] && break; sleep 0.1; done
    LASTP=$(prompts)
    sleep 1                      # the prompt is written before the read is posted
    printf '%s\n' "$1"
}
wait_count() {  # wait_count <pattern> <n> <timeout-s>
    local i n=$(( $3 * 10 ))
    for ((i = 0; i < n; i++)); do
        [ "$(count_of "$1")" -ge "$2" ] && return 0
        sleep 0.1
    done
    return 1
}

# The driver runs on the left of the pipe, so it can watch the log QEMU is
# writing on the right -- the same trick bootwait.sh uses.
drive() {
    logit_wait_for_shell "$LOG" 240
    local k=0
    for phase in 1 CHURN 2; do
        case "$phase" in
        1|2)
            for n in 16 32 64; do
                send "/bin/pad$n" "${PADWAIT:-1200}"
            done
            send "echo BIGEXEC-PHASE-$phase-DONE" "${PADWAIT:-1200}"
            wait_count "BIGEXEC-PHASE-$phase-DONE" 2 120 || return 0
            ;;
        CHURN)
            # Memory taken and released by something that is not the loader, so
            # phase 2 does not run on the free list phase 1 happened to leave.
            for c in '/bin/as /usr/as/examples/gc.as' \
                     '/bin/as /usr/as/examples/dict.as' \
                     '/bin/as /usr/as/examples/strings.as' \
                     'ls /bin | wc' \
                     'cat /docs/readme.txt | wc'; do
                send "$c" 300
                k=$(( k + 1 ))
                send "echo CHURN-$k" 300
                # 2: the shell echoes the command line AND the command prints.
                # Requiring both is what makes this a delivery confirmation
                # rather than an echo test.
                wait_count "CHURN-$k" 2 300 || return 0
            done
            send 'echo BIGEXEC-CHURN-DONE' 300
            wait_count 'BIGEXEC-CHURN-DONE' 2 120 || return 0
            ;;
        esac
    done
    send 'echo BIGEXEC-END' 300
    wait_count 'BIGEXEC-END' 2 120
    send 'exit' 60
    sleep 3
}

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# The pipeline runs in the BACKGROUND and QEMU is killed by this script, because
# `exit` from /bin/sh ends the shell, not the machine -- a foreground pipeline
# would wait forever for a QEMU that has no reason to stop. $! on a backgrounded
# pipeline is its last element, which is QEMU: the same shape every other
# harness here uses.
drive | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m "$RAM" -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; return 0; }
trap cleanup EXIT
# Two occurrences, not one: the first is the shell ECHOING `echo BIGEXEC-END`,
# which happens the instant the line is delivered and says nothing about the run
# having finished. The second is the command's output.
for _ in $(seq 1 ${WAIT:-40000}); do
    [ "$(count_of 'BIGEXEC-END')" -ge 2 ] && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

# --- the assertions --------------------------------------------------------
# Counted, not merely matched: each pad runs TWICE, so "the 64 MiB one loaded"
# is a claim about two lines. One occurrence means phase 2 did not happen, which
# is the half of this test that is worth having.
fail=0
for n in 16 32 64; do
    want=$((n * 1024 * 1024))
    got=$(count_of "BIGEXEC ok bytes=$want sum=315")
    if [ "$got" -eq 2 ]; then
        echo "  ok: ${n} MiB loaded and its last byte read -- twice (fresh + after churn)"
    else
        echo "  FAIL: ${n} MiB: expected 2 clean runs, got $got"
        fail=1
    fi
done

if grep -aq "BIGEXEC-CHURN-DONE" "$LOG"; then
    echo "  ok: the churn phase ran, so phase 2 is not a repeat of a pristine machine"
else
    echo "  FAIL: the churn phase never completed"
    fail=1
fi

if grep -aq "\[elf\] refused\|\[aex\] refused" "$LOG"; then
    echo "  FAIL: the loader refused an image:"
    grep -a "refused" "$LOG" | sed 's/^/    /'
    fail=1
else
    echo "  ok: the loader refused nothing"
fi

if grep -aq "\[oom\]" "$LOG"; then
    echo "  FAIL: the kernel reported an out-of-memory:"
    grep -a "\[oom\]" "$LOG" | sed 's/^/    /'
    fail=1
else
    echo "  ok: no kernel OOM -- nothing on this path asked for a big contiguous block"
fi

# THE NUMBERS, printed whether or not it passed. A gate that only says PASS
# leaves the next person re-running it to find out what it measured.
echo "  --- what the kernel said ---"
grep -a "\[aex\] /bin/pad" "$LOG" | sed 's/^/    /' || true
grep -a "\[exec\] load /bin/pad" "$LOG" | sed 's/^/    /' || true
echo "    heap growths during the whole run: $(count_of '\[kheap\] grow')"
grep -a "\[kheap\] grow" "$LOG" | tail -3 | sed 's/^/    /' || true
grep -a "\[exec\] loader:" "$LOG" | tail -1 | sed 's/^/    /' || true

if [ "$fail" -ne 0 ]; then
    echo "FAIL: test-bigexec  (full log: $LOG)"
    tail -80 "$LOG"
    exit 1
fi
echo "PASS: 16/32/64 MiB programs load on a fresh machine and on a churned one"
exit 0
