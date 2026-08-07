#!/usr/bin/env bash
# run-net-rx-test.sh -- assert WHERE the receive path runs.
#
# The NIC ISR used to drain the whole receive ring itself: eth_input ->
# ip_input -> tcp_input for up to a ring's worth of frames, inside the
# interrupt and before the EOI. It now acks the device and hands the drain to
# SOFTIRQ_NET, which interrupts.c runs at the tail of the same interrupt.
#
# It is also the negative control for something much larger that the counters
# found: THE NIC INTERRUPT WAS NEVER FIRING AT ALL. net_init() unmasks the
# card's RX cause before smp_init() routes its I/O APIC entry, DHCP asserts
# INTx in that window, the entry is routed EDGE-triggered behind an already-
# asserted line, and nothing but the ISR ever read ICR to deassert it -- so no
# further edge was ever produced. The card looked interrupt-driven and was
# polled, for the whole boot. See e1000_rx_drain() for the fix (ack in the
# drain, not only in the ISR).
#
# The test boots, pulls a body big enough to make the receive path do real
# work, and reads the one line net.c prints under traffic:
#
#     [net] rx path: frames N irq N softirq N inline N poll N
#
#   irq      NIC interrupts that reached net_rx_schedule()
#   softirq  drains that ran on SOFTIRQ_NET               <- the deferred path
#   inline   drains the ISR did itself because a raise was still owed (a NIC
#            interrupt nested inside a kernel sti window; see net.c)
#   poll     drains from net_poll(), the WM-loop backstop
#
# Two assertions, both of which FAIL on the previous build:
#   irq > 0                     the card raises interrupts at all
#                               (before: irq 0, measured over 554 frames)
#   softirq + inline > poll     and those interrupts, not the WM-loop
#                               backstop, are what carries the traffic
#                               (before: 0 + 0 vs poll 13)
#
# Usage: run-net-rx-test.sh <iso> <disk.img> [qemu-device]
set -u

ISO="${1:?usage: run-net-rx-test.sh <iso> <disk.img> [qemu-device]}"
DISK="${2:?usage: run-net-rx-test.sh <iso> <disk.img> [qemu-device]}"
DEV="${3:-e1000}"
QEMU="${QEMU:-qemu-system-x86_64}"
SIZE=917504

TMP="$(mktemp -d)"
LOG="$TMP/serial.log"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${HPID:-}" ] && kill "$HPID" 2>/dev/null
    sleep 0.2
    [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null
    [ -n "${HPID:-}" ] && kill -9 "$HPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

python3 - "$TMP" "$TMP/port" "$SIZE" <<'PY' &
import http.server, pathlib, sys
root, pf, size = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3])
(root / "probe.bin").write_bytes(bytes((i * 31 + 7) & 0xff for i in range(size)))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, _f, *_a): pass
srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0),
        lambda *a, **kw: Quiet(*a, directory=str(root), **kw))
pf.write_text(str(srv.server_port), encoding="ascii")
srv.serve_forever()
PY
HPID=$!
for _ in $(seq 1 100); do [ -s "$TMP/port" ] && break; sleep 0.05; done
[ -s "$TMP/port" ] || { echo "FAIL: host HTTP server did not start"; exit 1; }
PORT="$(cat "$TMP/port")"

{ sleep 14; printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"; sleep 40; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device "$DEV",netdev=n0 \
    -serial stdio -display none -no-reboot >"$LOG" 2>"$TMP/qemu.err" &
QPID=$!

for _ in $(seq 1 600); do
    grep -aq "\[net\] rx path:" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fail() {
    echo "FAIL[$DEV]: $1"
    echo "----- serial tail -----"; tail -30 "$LOG"; echo "-----------------------"
    exit 1
}

# `net get` prints what it read into its own 128 KiB buffer, not the body size;
# what matters here is that a real fetch completed and the frames crossed the NIC.
grep -aq "http bytes " "$LOG" || fail "the fetch never completed"
LINE="$(grep -a "\[net\] rx path:" "$LOG" | tail -1)"
[ -n "$LINE" ] || fail "net.c never reported the receive path (no '[net] rx path:' line)"

read -r FRAMES IRQ SOFT INLINE POLL <<<"$(echo "$LINE" | sed -n \
    's/.*frames \([0-9]*\) irq \([0-9]*\) softirq \([0-9]*\) inline \([0-9]*\) poll \([0-9]*\).*/\1 \2 \3 \4 \5/p')"
[ -n "${POLL:-}" ] || fail "could not parse: $LINE"

echo "  $LINE"
[ "$FRAMES" -ge 500 ] || fail "only $FRAMES frames received; the body did not cross the NIC"
[ "$IRQ" -gt 0 ] || fail \
    "the NIC raised ZERO interrupts across $FRAMES frames -- INTx is stuck asserted and the edge-triggered I/O APIC entry sees no further edges; RX ran on the net_poll backstop for the whole boot"
[ $((SOFT + INLINE)) -gt "$POLL" ] || fail \
    "interrupt-driven drains ($SOFT softirq + $INLINE inline) did not outnumber the $POLL net_poll backstop drains -- the receive path is still the WM loop"

echo "PASS[$DEV]: $IRQ NIC interrupts carried $FRAMES frames -- $SOFT on SOFTIRQ_NET, $INLINE inline (nested), $POLL on the net_poll backstop"
exit 0
