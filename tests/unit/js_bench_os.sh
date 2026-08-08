#!/usr/bin/env bash
# bench-js-os -- compile real page bundles ON LogitOS and print the numbers.
#
# The host build of js_bench is glibc on Linux with a general-purpose malloc.
# The target build is clang -ffreestanding against mini-libc, allocating from a
# fixed arena, with SSE turned on by boot code rather than by the ABI, reading
# its input through virtio-blk and LogitFS, under TCG. Every one of those can
# change a compile rate, and the guest is the only place the answer counts.
#
# Lives in tests/unit/ rather than tests/boot/ only because this line owns
# tests/unit/js_* and does not own tests/boot/. It is a boot harness in every
# other respect and is modelled on tests/boot/run-video-test.sh.
#
# Read the MEDIAN. This runs under TCG on a host that is also running other
# agents' QEMU; the minimum is the least-disturbed sample and the maximum is
# somebody else's build. The spread is printed for exactly that reason.

set -u

ISO="${1:?usage: js_bench_os.sh <iso> <disk.img> <iters> <guest-fixture-path>...}"
DISK="${2:?usage: js_bench_os.sh <iso> <disk.img> <iters> <guest-fixture-path>...}"
ITERS="${3:-5}"
shift 3
FIXTURES="$*"
[ -n "$FIXTURES" ] || { echo "FAIL: no fixtures given"; exit 1; }
QEMU="${QEMU:-qemu-system-x86_64}"

LOG="$(mktemp)"
FIFO="$(mktemp -u)"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${FEED:-}" ] && kill "$FEED" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$FIFO"
}
trap cleanup EXIT

echo "bench-js-os: $ITERS iterations, TCG, fixtures: $FIXTURES"

# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
# The big fixture is 1.55 MB and gets compiled $ITERS times from a fresh
# runtime each time; under TCG that is minutes, not seconds.
#
# The keystroke feeder goes through a FIFO rather than a `{ ... } | qemu`
# pipeline, and cleanup kills it. In a pipeline the feeder's trailing sleep
# keeps a write end of THIS SCRIPT'S stdout open long after the benchmark has
# printed its last line, so anything reading our output -- `make | tail`, a
# harness that greps us -- blocks for the whole sleep instead of the whole
# benchmark. That turned a three-minute measurement into a fifteen-minute one
# and is worth the two extra lines.
mkfifo "$FIFO"
{ sleep 4; printf '/bin/jsbench -n %s %s\nexit\n' "$ITERS" "$FIXTURES"; sleep 900; } > "$FIFO" &
FEED=$!
"$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    <"$FIFO" >"$LOG" 2>/dev/null &
QPID=$!

# Poll for up to ~15 min. Compiling is the slow part, not booting.
for _ in $(seq 1 9000); do
    grep -aq "JSBENCH-DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

if ! grep -aq "JSBENCH-DONE" "$LOG"; then
    echo "FAIL: /bin/jsbench never finished"
    echo "----- serial output (tail) -----"
    tail -40 "$LOG"
    echo "--------------------------------"
    exit 1
fi

echo "----- on-device (TCG) -----"
grep -a "JSBENCH" "$LOG" | tr -d '\r'
echo "---------------------------"

if grep -aq "JSBENCH-FAIL" "$LOG"; then
    echo "FAIL: a fixture did not compile ON THE MACHINE even though it does on"
    echo "      the host -- that is a target-build difference (mini-libc, the"
    echo "      arena, or SSE), not a language-coverage gap."
    exit 1
fi
STATUS="$(grep -a 'JSBENCH-DONE' "$LOG" | tail -1 | tr -d '\r' | awk '{print $2}')"
[ "$STATUS" = "0" ] || { echo "FAIL: jsbench reported $STATUS failures"; exit 1; }
echo "PASS: every fixture compiled on LogitOS"
exit 0
