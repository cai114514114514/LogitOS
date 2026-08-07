#!/usr/bin/env bash
# Audio on device, asserted against the samples QEMU actually played.
#
# WHY THIS SHAPE. Sound is the hardest thing in this tree to assert on: the
# obvious test ("the driver did not crash and the log says it is playing")
# passes against a DMA engine wired to nothing, against a mixer that overwrites
# instead of summing, and against a buffer that repeats its last period forever.
# So the instrument is chosen before the claim: QEMU's `wav` audiodev writes
# what the guest played to a FILE, and this script checks that file sample by
# sample. Every assertion below is a property of the captured audio.
#
#   ramp      the signal encodes its own frame index, so the checker recovers
#             the index of every frame and requires it to advance by exactly
#             one. Catches drops, repeats, reordering and a wrong rate.
#   mix       two streams, the second starting AND stopping mid-run: the
#             capture must show 8000, then 5000, then 8000.
#   underrun  the guest stops writing for longer than the whole buffer: the
#             capture must go SILENT and then recover.
#
# The guest is driven through /bin/sh on the serial console, exactly like
# run-shell-test.sh, so what is exercised is the real syscall ABI and not a
# kernel-internal back door.
set -u

ISO="${1:?usage: run-audio-wav-test.sh <iso> <disk.img> <mode>}"
DISK="${2:?usage: run-audio-wav-test.sh <iso> <disk.img> <mode>}"
MODE="${3:-ramp}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
WAV="$(mktemp -u).wav"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$WAV"
}
trap cleanup EXIT

case "$MODE" in
    ramp)     CMD="sndtest ramp 1000"; RUNSECS=26 ;;
    mix)      CMD="sndtest mix";       RUNSECS=28 ;;
    underrun) CMD="sndtest underrun";  RUNSECS=28 ;;
    *) echo "unknown mode '$MODE'" >&2; exit 2 ;;
esac

# The capture backend is pinned to the rate/format the card runs at, so nothing
# in the host path resamples and the bytes in the file are the bytes the guest
# produced. mixing-engine is left on (it is the default and the only path some
# QEMU builds have); it applies unity gain, so the checker still tolerates a
# sample or two of rounding rather than demanding bit-exactness it cannot
# guarantee across QEMU versions.
AUDIODEV="-audiodev wav,id=snd0,path=$WAV,out.frequency=48000,out.channels=2,out.format=s16"
# hda-output rather than hda-duplex: the wav backend is output-only and a duplex
# codec makes QEMU log a failed capture voice on every run.
SNDDEV="${QEMU_AUDIO:--device intel-hda -device hda-output,audiodev=snd0}"

{ sleep 12; printf '%s\nexit\n' "$CMD"; sleep $((RUNSECS - 12)); } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    $AUDIODEV $SNDDEV \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Wait for the program to finish, then let the DMA drain into the capture.
for _ in $(seq 1 $((RUNSECS * 10))); do
    grep -aqE "SNDTEST_(RAMP_DONE|MIX_DONE|UNDERRUN_DONE|FAIL|NODEV)" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 2

# QEMU finalises the WAV header (the RIFF/data sizes) on exit, so it must be
# shut down cleanly before the file is read. A killed -9 QEMU leaves a header
# claiming zero bytes and the checker would report an empty capture.
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null
QPID=

fail=0
echo "--- guest said ---"
grep -aE "SNDTEST_|^\[snd\]|^\[hda\]" "$LOG" | sed 's/^/    /' || true
echo "------------------"

if ! grep -aq "LOGIT_BOOT_OK" "$LOG"; then
    echo "FAIL: kernel did not reach LOGIT_BOOT_OK"; fail=1
fi
if grep -aq "SNDTEST_FAIL" "$LOG"; then
    echo "FAIL: sndtest reported a failure"; fail=1
fi
if grep -aq "SNDTEST_NODEV" "$LOG"; then
    echo "FAIL: the guest found no audio device, but one was attached"; fail=1
fi
# The boot line is the "130 roots, 0 skipped" of audio: it must name a real
# codec, not merely say audio is present.
if ! grep -aqE "^\[snd\] hda: codec" "$LOG"; then
    echo "FAIL: no [snd] boot line naming a codec"; fail=1
fi

if [ ! -s "$WAV" ]; then
    echo "FAIL: no WAV was captured at all"
    exit 1
fi

python3 tests/boot/audio_check.py "$WAV" "$MODE" || fail=1

[ "$fail" != 0 ] && { echo "----- serial -----"; tail -60 "$LOG"; exit 1; }
echo "PASS: audio '$MODE' verified against the captured WAV"
exit 0
