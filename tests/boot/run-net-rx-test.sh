#!/usr/bin/env bash
# run-net-rx-test.sh -- assert that nothing in the receive path waits for a
# timer tick.
#
# WHAT THIS IS THE CONTROL FOR
# ============================
# net_init() unmasks the card's RX cause, smp.c routes its I/O APIC entry to
# vector 65, the boot log says so -- and the card raised ZERO interrupts:
#
#     [net] rx path: frames 554 irq 0 softirq 0 inline 0 poll 13
#
# Reading ICR is what deasserts the e1000's INTx line, and that entry is routed
# EDGE-triggered on purpose (smp.c records why: QEMU's TCG IOAPIC never clears a
# level RTE's remote-IRR, so a level line storms). An edge-triggered line that
# is already asserted produces no further edges. net_init() runs before
# smp_init(), DHCP asserts INTx in that window, the routing goes in behind an
# already-low line, and the only reader of ICR was the ISR that could no longer
# run. The receive path was the window manager's 100 Hz poll, on every boot.
#
# WHY THE ASSERTION IS NOT "irq > 0"
# ==================================
# It was, and it is not sound. The fix reads ICR at the top of e1000_rx_drain(),
# which also runs from net_poll() -- so a poll that lands between the card
# asserting and the CPU taking the interrupt CONSUMES the cause and does the
# drain itself. That is fine (whoever notices first does the work, and both are
# fast) but it makes the interrupt count a race. Measured on one build, three
# consecutive runs: irq 34, irq 0, irq 0. An assertion on it would be a flaky
# gate on an implementation detail.
#
# So this gates the PROPERTY instead, measured from outside the guest by
# tests/boot/netwire.py: rx->ack turnaround, the time between a segment
# crossing the wire and the guest emitting an ack that covers it. That is the
# latency the guest owns, it does not care which context did the drain, and a
# receive path that only runs when a 100 Hz timer says so cannot have a tail
# below 10 ms. Measured over ~5700 segments per build:
#
#     before   median 1.86 ms   p90 2.50   p99 9.81   max 10.26  <- the tick
#     after    median 0.80 ms   p90 1.61   p99 2.65   max  3.30
#
# The threshold is 6 ms on the MAXIMUM: half the tick, twice the worst observed
# on a good build, and the before-build fails it on every run because a tick is
# 10 ms and it waited a whole one.
#
# Usage: run-net-rx-test.sh <iso> <disk.img> [qemu-device]
# Env:   NET_RX_MAX_MS   turnaround ceiling in ms (default 6)
set -u

ISO="${1:?usage: run-net-rx-test.sh <iso> <disk.img> [qemu-device]}"
DISK="${2:?usage: run-net-rx-test.sh <iso> <disk.img> [qemu-device]}"
DEV="${3:-e1000}"
QEMU="${QEMU:-qemu-system-x86_64}"
MAXMS="${NET_RX_MAX_MS:-6}"
SIZE=917504
HERE="$(cd "$(dirname "$0")" && pwd)"

TMP="$(mktemp -d)"
LOG="$TMP/serial.log"
PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done
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
PIDS+=("$!")
for _ in $(seq 1 100); do [ -s "$TMP/port" ] && break; sleep 0.05; done
[ -s "$TMP/port" ] || { echo "FAIL: host HTTP server did not start"; exit 1; }
PORT="$(cat "$TMP/port")"

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'; }
P1="$(free_port)"; P2="$(free_port)"

# The wire, and the peer behind it. See netwire.py's header for the topology and
# for why `-machine none` (not `-S`) is what makes QEMU-B forward anything.
"$QEMU" -machine none -nodefaults -display none -monitor none \
    -netdev socket,id=s,listen=127.0.0.1:$P2 -netdev user,id=u \
    -netdev hubport,id=h0,hubid=0,netdev=s \
    -netdev hubport,id=h1,hubid=0,netdev=u >"$TMP/qb.log" 2>&1 &
PIDS+=("$!")
python3 "$HERE/netwire.py" --listen 127.0.0.1:$P1 --connect 127.0.0.1:$P2 \
    --report "$TMP/wire.json" >"$TMP/wire.log" 2>&1 &
WIRE=$!; PIDS+=("$WIRE")
sleep 1

# FIVE fetches, not one. The tail this gates lives in the GAPS -- the connect,
# and the moments a stream stalls, where the blocking loop halts and something
# has to wake it. A single bulk fetch spends its whole life with data already in
# flight (measured: net_idle is not called once during one), so it exercises the
# drain and never the wake, and BOTH builds pass it.
{ sleep 16
  for _ in 1 2 3 4 5; do
      printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"
      sleep 12
  done
  sleep 20; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev socket,id=n0,connect=127.0.0.1:$P1 -device "$DEV",netdev=n0 \
    -serial stdio -display none -no-reboot >"$LOG" 2>"$TMP/qemu.err" &
QPID=$!; PIDS+=("$QPID")

for _ in $(seq 1 1400); do
    [ "$(grep -ac 'http bytes ' "$LOG" 2>/dev/null)" -ge 5 ] && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 1
kill "$QPID" 2>/dev/null
kill -TERM "$WIRE" 2>/dev/null
for _ in $(seq 1 60); do [ -s "$TMP/wire.json" ] && break; sleep 0.2; done

fail() {
    echo "FAIL[$DEV]: $1"
    echo "----- serial tail -----"; tail -25 "$LOG"; echo "-----------------------"
    exit 1
}

grep -aq "http bytes " "$LOG" || fail "the fetch never completed"
[ -s "$TMP/wire.json" ] || fail "netwire produced no report"

# Diagnostics: which context actually drained, from the guest's own counters.
DIAG="$(grep -a '\[net\] rx path:' "$LOG" | tail -1)"
[ -n "$DIAG" ] && echo "  ${DIAG#*\[net\] }"

python3 - "$TMP/wire.json" "$MAXMS" <<'PY'
import json, sys
rep = json.load(open(sys.argv[1]))
limit = float(sys.argv[2])
t = rep.get("rx_to_ack_turnaround")
if not t:
    print("FAIL: netwire saw no inbound segments to time"); sys.exit(1)
print("  rx->ack turnaround n=%d  median %.3f ms  p90 %.3f  p99 %.3f  max %.3f"
      % (t["n"], t["median_ms"], t["p90_ms"], t["p99_ms"], t["max_ms"]))
if t["n"] < 1000:
    print("FAIL: only %d segments timed; the body did not cross the wire" % t["n"])
    sys.exit(1)
if t["max_ms"] >= limit:
    print("FAIL: the guest took %.3f ms to acknowledge a segment (limit %.1f ms)."
          % (t["max_ms"], limit))
    print("      A receive path that is noticed by a 100 Hz timer rather than by")
    print("      the packet cannot have a tail below one tick, and one tick is")
    print("      10 ms. If the host is heavily loaded, re-run before believing it.")
    sys.exit(1)
print("PASS: nothing in the receive path waited for a tick "
      "(worst acknowledgement %.3f ms over %d segments, limit %.1f ms)"
      % (t["max_ms"], t["n"], limit))
PY
