#!/usr/bin/env bash
# Headless MSE test: boot LogitOS and play a DASH-shaped segmented stream
# through the real Media Source Extensions engine, on the machine.
#
# WHAT THIS ADDS OVER make test-mse. The host gate runs the same C with a
# simulated clock and a simulated sound card, on glibc. This runs it with
# mini-libc's arena allocator, clang -ffreestanding -msse2, a 32 KiB stack, a
# real monotonic clock, a real sound card and the segments read off LogitFS one
# at a time -- and all of those have broken a decoder that was bit-exact on the
# host before.
#
# WHAT IS REQUIRED, and every one of them is a number the program prints:
#   - the codec answer table is IDENTICAL to the host's, av01 included. A build
#     where a #define wandered would say something different here.
#   - addSourceBuffer(av01) is refused on the device too.
#   - every picture in the stream is decoded, in presentation order.
#   - A/V drift stays inside the stated bound.
#   - the demuxer re-parse count stays proportional to the segment count, which
#     is the difference between the lazy re-open and a quadratic one.
#
# Portable: no `timeout` dependency, polls the log like the other boot tests.

set -u

ISO="${1:?usage: run-mse-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-mse-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
DRIFT_MAX_MS="${DRIFT_MAX_MS:-120}"

# THE AUDIO BACKEND IS NOT A DETAIL. Audio is the master clock -- video is paced
# against the frames the CARD HAS PLAYED -- so a backend whose play cursor does
# not advance freezes the picture for ever, and the failure looks exactly like a
# decoder that stopped. `-audiodev none` does that on this machine: the first
# run stalled after five pictures with the clock parked at 400 ms. The `wav`
# backend is timer-driven and consumes at the real rate, which is why
# tests/audio.mk uses it, and it leaves a file of what the guest actually played.
WAV="$(mktemp -u).wav"
AUDIODEV="${QEMU_AUDIODEV:--audiodev wav,id=snd0,path=$WAV,out.frequency=48000,out.channels=2,out.format=s16}"

LOG="$(mktemp)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$WAV"
}
trap cleanup EXIT

# A sound card, because the whole synchronisation policy rests on the card's own
# play cursor being the master clock. Without one the engine falls back to the
# monotonic clock and this test would be measuring something easier.
{ sleep 4; printf '/bin/msecheck /media/mse\nexit\n'; sleep 420; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    $AUDIODEV -device intel-hda -device hda-output,audiodev=snd0 \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 4400); do
    grep -aq "MSE-DONE" "$LOG" && break
    grep -aq "MSE-FAIL" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

show_tail() {
    echo "----- serial output (tail) -----"
    tail -50 "$LOG"
    echo "--------------------------------"
}

if grep -aq "MSE-FAIL" "$LOG"; then
    echo "FAIL: $(grep -a 'MSE-FAIL' "$LOG" | head -1 | tr -d '\r')"
    show_tail
    exit 1
fi
if ! grep -aq "MSE-DONE" "$LOG"; then
    echo "FAIL: /bin/msecheck did not finish"
    show_tail
    exit 1
fi

fail=0
say() { printf '  %s\n' "$1"; }

# --- 1. the codec answer table, against the truth -------------------------
# Written out here rather than diffed against a generated file on purpose: this
# list IS the claim, and a test that compares the device against a file the
# device's own build produced compares nothing.
check_type() {
    want="$1"; type="$2"
    got="$(grep -aF "MSE-TYPE " "$LOG" | grep -aF "$type" | head -1 | awk '{print $2}' | tr -d '\r')"
    if [ "$got" != "$want" ]; then
        echo "FAIL: isTypeSupported($type) said '$got', must be '$want'"
        fail=1
    else
        say "isTypeSupported($type) = $got"
    fi
}
check_type yes 'avc1.640033'
check_type yes 'avc1.4d401e'
check_type yes 'avc1.42E01E'
check_type yes 'hvc1.1.6.L120.90'
check_type yes 'mp4a.40.2'
check_type yes 'mp4a.40.34'
check_type no  'av01.0.08M.08'
check_type no  'vp09.00.10.08'
check_type no  'vp9,opus'
check_type no  'codecs="opus"'
check_type no  'mp4a.40.5'
check_type no  'avc1.6E0033'

# --- 2. the refusal reaches addSourceBuffer -------------------------------
SB="$(grep -a 'MSE-SB' "$LOG" | tail -1 | tr -d '\r')"
say "$SB"
case "$SB" in
  *"video=1 audio=1 av1=0 av1err=-1"*) say "addSourceBuffer(av01) refused with NotSupportedError" ;;
  *) echo "FAIL: addSourceBuffer did not behave: $SB"; fail=1 ;;
esac

# --- 3. the stream played -------------------------------------------------
PLAY="$(grep -a 'MSE-PLAY' "$LOG" | tail -1 | tr -d '\r')"
SYNC="$(grep -a 'MSE-SYNC' "$LOG" | tail -1 | tr -d '\r')"
BUF="$(grep -a 'MSE-BUF' "$LOG" | tail -1 | tr -d '\r')"
APP="$(grep -a 'MSE-APPEND' "$LOG" | tail -1 | tr -d '\r')"
say "$PLAY"
say "$SYNC"
say "$BUF"
say "$APP"

field() { echo "$1" | tr ' ' '\n' | grep -a "^$2=" | head -1 | cut -d= -f2; }

DEC="$(field "$PLAY" decoded)"
SHOWN="$(field "$PLAY" shown)"
BACK="$(field "$PLAY" backwards)"
BLITS="$(field "$PLAY" blits)"
SEGS="$(field "$PLAY" segments)"
DRIFT="$(field "$SYNC" drift_mean_ms)"
AUD="$(field "$SYNC" audio_frames)"
REPARSE="$(field "$APP" reparses)"
APPENDS="$(field "$APP" appends)"
ENDED="$(field "$APP" ended)"
SIZE="$(field "$BUF" size)"

req() {  # req <name> <value> <test-expr...>
    if [ "$3" = "-ge" ] && [ "$2" -ge "$4" ] 2>/dev/null; then say "$1: $2 (>= $4)"; return; fi
    if [ "$3" = "-eq" ] && [ "$2" = "$4" ]; then say "$1: $2"; return; fi
    if [ "$3" = "-le" ] && [ "$2" -le "$4" ] 2>/dev/null; then say "$1: $2 (<= $4)"; return; fi
    echo "FAIL: $1 is '$2', needs $3 $4"
    fail=1
}

# 60 pictures is what the fixture holds; anything less means access units were
# fed to nothing, which is precisely the bug the feed discipline exists for.
req "pictures decoded" "${DEC:-0}" -eq 60
req "pictures shown"   "${SHOWN:-0}" -ge 12
req "blits"            "${BLITS:-0}" -ge 12
req "out-of-order presentations" "${BACK:-1}" -eq 0
req "audio frames written" "${AUD:-0}" -ge 150000
req "appends" "${APPENDS:-0}" -ge 11
req "ended" "${ENDED:-0}" -eq 1
# Lazy re-parse: proportional to segments (9 of them + a couple of forced ones),
# not to appends or pump steps.
req "demuxer re-parses" "${REPARSE:-999}" -le 30
[ "$SEGS" = "4+5" ] && say "segments appended: $SEGS" || { echo "FAIL: segments=$SEGS, want 4+5"; fail=1; }
[ "$SIZE" = "128x96" ] && say "picture size: $SIZE" || { echo "FAIL: size=$SIZE"; fail=1; }

# Drift, measured by avclock itself. The bound is looser than the host's 5 ms
# because this machine really is slow -- a from-scratch H.264 decoder under TCG
# is nowhere near real time -- and the policy for that case is to drop video and
# keep audio whole, which the counters above already checked.
AD="${DRIFT:-999}"
AD="${AD#-}"
if [ "$AD" -le "$DRIFT_MAX_MS" ] 2>/dev/null; then
    say "mean A/V drift ${DRIFT} ms (bound ${DRIFT_MAX_MS} ms)"
else
    echo "FAIL: mean A/V drift ${DRIFT} ms exceeds ${DRIFT_MAX_MS} ms"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    show_tail
    exit 1
fi
echo "PASS: a DASH-shaped segmented stream played through MediaSource on LogitOS"
exit 0
