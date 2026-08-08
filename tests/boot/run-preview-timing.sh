#!/usr/bin/env bash
# Does the animation player honour the delays it decoded?  Measured on the
# machine, over the serial console.
#
# WHY THIS EXISTS SEPARATELY FROM THE IMAGE LINE'S TESTS
# make test-img-anim already asserts, on the host, that the GIF and APNG
# decoders report the right delay for every frame. That says nothing about
# whether anything WAITS. A player that decodes 120/400/70/900 ms perfectly and
# then shows four frames as fast as it can loops passes every decoder test in
# the tree and is useless as a viewer. So this measures the PLAYER: one loop of
# tests/fixtures/image/anim.gif -- 120+400+70+900 = 1490 ms declared -- against
# the wall clock.
#
# WHAT IT ACCEPTS, AND WHY IT IS NOT SYMMETRIC
#   lower bound   elapsed >= declared. This is the whole claim, and it is what
#                 the negative control breaks.
#   upper bound   declared + paint + slack, where `paint` is the time the
#                 player itself reports spending in clear/blit/flush. A frame
#                 cannot be shown before it has been painted, and on this
#                 machine painting a 40x28 image scaled to fill a 760x530
#                 window costs a large fraction of a 120 ms delay. Bounding the
#                 overrun by a MEASURED cost rather than a guessed constant is
#                 what keeps "the player is correct" and "the machine is slow"
#                 as separate statements.
#
# Usage: run-preview-timing.sh <iso> <disk.img> [play|negctl]
#   play    /bin/previewplay    -- the shipped loop; the bounds must HOLD
#   negctl  /bin/previewnegctl  -- built -DPREVIEW_NO_ANIM_TIMING, i.e. the same
#                                  player with the wait removed; the bounds must
#                                  FAIL, or they were never measuring anything

set -u

ISO="${1:?usage: run-preview-timing.sh <iso> <disk.img> [play|negctl]}"
DISK="${2:?usage: run-preview-timing.sh <iso> <disk.img> [play|negctl]}"
MODE="${3:-play}"
QEMU="${QEMU:-qemu-system-x86_64}"

case "$MODE" in
  play)   BIN=previewplay ;;
  negctl) BIN=previewnegctl ;;
  *) echo "unknown mode $MODE"; exit 2 ;;
esac

WORK="$(mktemp -d)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# A PRIVATE COPY OF THE IMAGE AND THE DISK. `-snapshot` keeps the guest's
# writes out of build/; it does not keep another line's `make` out of the
# guest, and this tree has several running at once. A disk rewritten under a
# QEMU that reads it lazily comes back as short directory listings and files
# that read as somebody else's blocks -- which looks exactly like a filesystem
# bug and is not one.
cp "$ISO" "$WORK/logit.iso"
cp "$DISK" "$WORK/disk.img"
LOG="$WORK/serial.log"

{ sleep 8; printf '%s /media/img/anim.gif\nexit\n' "$BIN"; sleep 25; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$WORK/logit.iso" \
    -drive file="$WORK/disk.img",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 600); do
    grep -aq "preview: anim " "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.2
done
sleep 1

LINE="$(grep -a "preview: anim " "$LOG" | tail -1)"
if [ -z "$LINE" ]; then
    echo "FAIL: /bin/$BIN never reported a loop"
    echo "----- serial tail -----"; tail -30 "$LOG"; echo "-----------------------"
    exit 1
fi
echo "$LINE"

get() { echo "$LINE" | sed -n "s/.*$1=\([0-9-]*\).*/\1/p"; }
FRAMES="$(get frames)"; DECL="$(get declared_ms)"
ELAPSED="$(get elapsed_ms)"; PAINT="$(get paint_ms)"
: "${FRAMES:=0}" "${DECL:=0}" "${ELAPSED:=0}" "${PAINT:=0}"

if [ "$DECL" != 1490 ] || [ "$FRAMES" != 4 ]; then
    echo "FAIL: anim.gif should decode to 4 frames declaring 1490 ms;"
    echo "      got $FRAMES frames / $DECL ms -- the fixture or the decoder moved."
    exit 1
fi

HI=$((DECL + PAINT + 150))
echo "     declared ${DECL}ms  elapsed ${ELAPSED}ms  painting ${PAINT}ms  (bounds ${DECL}..${HI})"

ok=1
[ "$ELAPSED" -ge "$DECL" ] || ok=0
[ "$ELAPSED" -le "$HI" ]   || ok=0

if [ "$MODE" = play ]; then
    if [ "$ok" = 1 ]; then
        echo "PASS: the player waited out every frame's declared delay"
        exit 0
    fi
    echo "FAIL: one loop of anim.gif took ${ELAPSED}ms against ${DECL}ms declared"
    exit 1
fi

# negctl: the same assertion, run against the player with the wait removed.
if [ "$ok" = 1 ]; then
    echo "FAIL: with the frame-delay wait compiled out the timing bounds STILL"
    echo "      held (${ELAPSED}ms in ${DECL}..${HI}). They are not measuring the"
    echo "      player, so the positive result proves nothing."
    exit 1
fi
echo "PASS (negative control): the wait is gone -> one loop finished in ${ELAPSED}ms"
echo "      against ${DECL}ms declared, and the assertion that passes for"
echo "      /bin/previewplay fails here."
exit 0
