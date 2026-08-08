#!/usr/bin/env bash
# Does ring 3 actually reach the kernel DRBG?
#
# Boots LogitOS, runs /bin/entropy TWICE from the serial shell, and asserts four
# things about the bytes that come back. A statistical test is not one of them --
# see the header of c/apps/coreutils/entropy.c for why the old xorshift would
# have passed any such test. The properties here are the ones it fails:
#
#   1. two reads inside one process differ
#   2. a forked child and its parent get different bytes
#   3. the SAME program run twice produces different bytes
#   4. the syscall refuses a buffer that is not the caller's
#
# ENTROPY_BREAK=1 INVERTS the verdict: it is the negative control, run against a
# disk packed with /bin/entropy built -DENTROPY_CONTROL_XORSHIFT -- the
# generator c/apps/browser/js_platform.c really used to ship, same algorithm and
# same seeding. **Assertion 2 is the discriminator** and it must go RED there.
#
# WHY 2 AND NOT 3, because this was measured rather than assumed. The first
# version of this control expected the CROSS-RUN check to fail, on the argument
# that a clock-and-address-seeded PRNG reseeds identically in two runs a
# fraction of a second apart. It does not: the seed mixes a millisecond clock
# and stack addresses, and two runs differ in both, so the old generator passes
# the cross-run check. It is a real property and a weak one -- "different from
# last time" is not unpredictability.
#
# The FORK check is the one no userland PRNG can pass: its state is a pair of
# words in the process's own memory, fork() copies them, and parent and child
# then produce the SAME stream byte for byte. That is not a statistical
# tendency, it is an identity, and it is the strongest possible statement of
# predictability -- holding one output tells you the other exactly. A kernel
# DRBG cannot fail it, because the state was never in the address space that
# was copied. Observed: with the control build, ENT_CHILD == ENT_PARENT
# exactly; with the real one they differ.
#
# Assertion 3 stays in the suite. It is simply not what the control turns.
#
#   run-entropy-test.sh <iso> <disk.img>

set -u

ISO="${1:?usage: run-entropy-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-entropy-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
BREAK="${ENTROPY_BREAK:-}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# The two runs are back to back on purpose: the control's seed is the wall clock
# in WHOLE SECONDS, so putting them in the same second is what makes its failure
# deterministic rather than a race.
{ sleep 4; printf '/bin/entropy\n/bin/entropy\nexit\n'; sleep 6; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 300); do
    n=$(grep -ac '^ENTROPY_SELF_' "$LOG" 2>/dev/null); n=${n:-0}
    if [ "$n" -ge 2 ]; then break; fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

# strip CR: the serial console line-ends with \r\n
tr -d '\r' < "$LOG" > "$LOG.clean"; mv "$LOG.clean" "$LOG"

fail=0
say() { echo "$1"; }
need() { # need <name> <condition-result 0/1>
    if [ "$2" = 0 ]; then say "ok   $1"; else say "FAIL $1"; fail=1; fi
}

A1=$(grep -a '^ENT_A '      "$LOG" | sed -n 1p | awk '{print $2}')
B1=$(grep -a '^ENT_B '      "$LOG" | sed -n 1p | awk '{print $2}')
C1=$(grep -a '^ENT_CHILD '  "$LOG" | sed -n 1p | awk '{print $2}')
P1=$(grep -a '^ENT_PARENT ' "$LOG" | sed -n 1p | awk '{print $2}')
A2=$(grep -a '^ENT_A '      "$LOG" | sed -n 2p | awk '{print $2}')
BP=$(grep -a '^ENT_BADPTR ' "$LOG" | sed -n 1p | awk '{print $2}')
ST=$(grep -a '^ENT_STRONG ' "$LOG" | sed -n 1p | awk '{print $2}')

if [ -z "$A1" ] || [ -z "$B1" ] || [ -z "$C1" ] || [ -z "$P1" ] || [ -z "$A2" ]; then
    echo "FAIL: /bin/entropy did not produce the expected lines"
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
fi

echo "run 1 read A : $A1"
echo "run 1 read B : $B1"
echo "run 1 child  : $C1"
echo "run 1 parent : $P1"
echo "run 2 read A : $A2"
echo "hw-seeded DRBG (RDSEED/RDRAND): $ST   bad-pointer return: $BP"
echo

need "each read is 32 bytes"                    "$([ ${#A1} -eq 64 ] && echo 0 || echo 1)"
need "two reads in one process differ"          "$([ "$A1" != "$B1" ] && echo 0 || echo 1)"
forkdiff=0; [ "$C1" != "$P1" ] || forkdiff=1
need "a forked child and its parent differ (state is NOT in this address space)" "$forkdiff"
need "the syscall refused a kernel address"     "$([ "${BP:-0}" -lt 0 ] && echo 0 || echo 1)"

crossrun=0; [ "$A1" != "$A2" ] || crossrun=1
need "two separate runs differ" "$crossrun"

if [ -n "$BREAK" ]; then
    # The control's verdict rests on the FORK assertion alone -- see the header.
    if [ "$forkdiff" != 0 ]; then
        echo
        echo "PASS (control): the xorshift build gave the forked child and its parent"
        echo "                BYTE-IDENTICAL output, so that assertion is a real one."
        echo "                child : $C1"
        echo "                parent: $P1"
        exit 0
    fi
    echo
    echo "CONTROL FAILED: the crippled build still passed the fork check --"
    echo "                that assertion proves nothing."
    exit 1
fi

if [ "$fail" = 0 ]; then
    echo
    echo "PASS: ring 3 reaches the kernel DRBG"
    exit 0
fi
echo
echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
exit 1
