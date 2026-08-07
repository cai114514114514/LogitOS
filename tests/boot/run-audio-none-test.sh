#!/usr/bin/env bash
# A machine with NO sound card must boot cleanly and go quiet.
#
# This is the case real hardware hits first -- most machines this kernel will
# ever run on have no HDA controller the driver can claim, and a driver that
# hangs, faults or refuses to boot without one is worse than no driver. So this
# asserts the DEGRADATION, not the absence:
#
#   1. the kernel still reaches LOGIT_BOOT_OK,
#   2. the audio layer says so explicitly ("no audio device found") rather than
#      staying silent about being silent -- on unfamiliar hardware "there is no
#      line" and "the line says none" are different diagnoses and only the
#      second is evidence,
#   3. a program that asks gets SND_E_NODEV (-1) from snd_open and EXITS 0,
#   4. the shell is still alive afterwards, which is the real proof that the
#      no-card path did not wedge anything.
#
# Two device sets, because "no card" has two shapes and only one of them is
# tested by passing no -device flag:
#   SET=none   no audio hardware at all
#   SET=other  an AC'97 present but NOT claimed by the HDA driver (class
#              0x04/0x01 vs 0x04/0x03) -- a card we cannot drive must be as
#              harmless as no card, and this is the case a class-match table
#              gets wrong by being too greedy.
set -u

ISO="${1:?usage: run-audio-none-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-audio-none-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

run_one() {
    local label="$1" devs="$2"
    local log; log="$(mktemp)"
    local qpid

    { sleep 12; printf 'sndtest info\necho SHELL-STILL-ALIVE\nexit\n'; sleep 8; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
        -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        -netdev user,id=n0 -device e1000,netdev=n0 $devs \
        -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    qpid=$!

    for _ in $(seq 1 250); do
        grep -aq "SHELL-STILL-ALIVE" "$log" && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null

    local bad=0
    echo "--- [$label] guest said ---"
    grep -aE "SNDTEST_|^\[snd\]|^\[hda\]" "$log" | sed 's/^/    /' || true

    grep -aq "LOGIT_BOOT_OK" "$log" \
        || { echo "FAIL[$label]: kernel did not reach LOGIT_BOOT_OK"; bad=1; }
    grep -aq "\[snd\] no audio device found" "$log" \
        || { echo "FAIL[$label]: the audio layer never said it found nothing"; bad=1; }
    grep -aq "SNDTEST_NODEV_OK" "$log" \
        || { echo "FAIL[$label]: snd_open did not return SND_E_NODEV"; bad=1; }
    grep -aq "SHELL-STILL-ALIVE" "$log" \
        || { echo "FAIL[$label]: the shell did not survive the audio query"; bad=1; }
    grep -aqE "PANIC|#PF|GPF" "$log" \
        && { echo "FAIL[$label]: the kernel faulted"; bad=1; }

    [ "$bad" = 0 ] && echo "ok  [$label] booted, reported no device, degraded to silence"
    [ "$bad" != 0 ] && { echo "----- serial -----"; tail -40 "$log"; }
    rm -f "$log"
    return $bad
}

fail=0
run_one none  ""              || fail=1
# An AC'97 is class 0x04 subclass 0x01. The HDA driver matches 0x04/0x03, so it
# must NOT bind here -- if it does, the machine has a driver talking HDA
# registers to an AC'97, which is exactly the failure a class match without a
# subclass produces.
run_one other "-device AC97"  || fail=1

[ "$fail" != 0 ] && exit 1
echo "PASS: no sound card, and a card we do not drive, both degrade to silence"
exit 0
