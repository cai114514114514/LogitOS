#!/usr/bin/env bash
# test-jssem-os -- run the semantics cases ON LogitOS and print the transcript.
#
# Modelled on tests/unit/js_bench_os.sh, including the FIFO keystroke feeder
# and why it is a FIFO rather than a pipeline.
#
# This is the run that counts. /bin/jssem is linked from $(ENGINE_OBJ) -- the
# same object files build/browser.elf links -- so it is the browser's engine
# and not the host's. The output is written to $JSSEM_GUEST_LOG (default a
# temp file, printed at the end) so it can be diffed against the node oracle
# transcript by hand or by run-objweak.sh's node half.
set -u

ISO="${1:?usage: run-guest.sh <iso> <disk.img> <guest-case-path>...}"
DISK="${2:?usage: run-guest.sh <iso> <disk.img> <guest-case-path>...}"
shift 2
CASES="$*"
[ -n "$CASES" ] || { echo "FAIL: no cases given"; exit 1; }
QEMU="${QEMU:-qemu-system-x86_64}"
OUT="${JSSEM_GUEST_LOG:-$(mktemp)}"

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
{ sleep 4; printf '/bin/jssem %s\nexit\n' "$CASES"; sleep 600; } > "$FIFO" &
FEED=$!
"$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    <"$FIFO" >"$LOG" 2>/dev/null &
QPID=$!

# Wait for the sentinel the probe prints after its last case.
for i in $(seq 1 600); do
    grep -q 'JSSEM-DONE' "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done

cp "$LOG" "$OUT"
if ! grep -q 'JSSEM-DONE' "$OUT"; then
    echo "FAIL: guest never printed JSSEM-DONE -- transcript at $OUT"
    tail -30 "$OUT"
    exit 1
fi
echo "guest transcript: $OUT"
grep -c '^=== BEGIN ' "$OUT" | sed 's/^/cases run: /'
