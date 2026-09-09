#!/usr/bin/env bash
# js_sem_os -- run the js_sem cases ON THE MACHINE and print what the guest
# engine produced, in the same framing the host runner and node produce.
#
# WHY THIS EXISTS AND WHY NO FINDING IS REPORTED WITHOUT IT: the host gates in
# this tree compile the same QuickJS sources with different defines and against
# a different libc. Measuring the host binary and reporting it as the browser
# is the single most likely way this line produces a wrong answer. The guest
# binary is the one the browser links: same $(ENGINE_OBJ), same JS_CF
# (-DLOGIT_OS -DCONFIG_STACK_CHECK -DNDEBUG), mini-libc's arena allocator,
# -msse2, under TCG.
#
#   js_sem_os.sh <iso> <disk.img> <guest-case-path>...
#
# The disk must already carry /bin/jssemci and the cases. Modelled on
# tests/unit/js_bench_os.sh, including the FIFO feeder: in a `{...} | qemu`
# pipeline the feeder's trailing sleep holds a write end of this script's
# stdout open long after the run has finished.
set -u

ISO="${1:?usage: js_sem_os.sh <iso> <disk> <case>...}"
DISK="${2:?usage: js_sem_os.sh <iso> <disk> <case>...}"
shift 2
CASES="$*"
[ -n "$CASES" ] || { echo "FAIL: no cases given"; exit 1; }
QEMU="${QEMU:-qemu-system-x86_64}"
OUT="${OUT:-build-jssem/ci/guest.out}"

LOG="$(mktemp)"
FIFO="$(mktemp -u)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${FEED:-}" ] && kill "$FEED" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$FIFO"
}
trap cleanup EXIT

mkfifo "$FIFO"
{ sleep 5; printf '/bin/jssemci %s\nexit\n' "$CASES"; sleep 600; } > "$FIFO" &
FEED=$!
"$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    <"$FIFO" >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 6000); do
    grep -aq "JSSEM-DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

cp "$LOG" build-jssem/ci/guest-serial.log 2>/dev/null

if ! grep -aq "JSSEM-DONE" "$LOG"; then
    echo "FAIL: /bin/jssemci never finished"
    tail -40 "$LOG"
    rm -f "$LOG"; exit 1
fi

# The framing lines only. Anchored: "## ", "| ", "! " at start of line, plus
# the terminator. Nothing the kernel prints on serial starts with those.
tr -d '\r' < "$LOG" | grep -aE '^(## |\| |! |JSSEM-DONE)' > "$OUT"
rm -f "$LOG"
echo "guest output: $OUT ($(wc -l < "$OUT" | tr -d ' ') lines)"
