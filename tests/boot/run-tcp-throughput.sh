#!/usr/bin/env bash
# TCP receive throughput over QEMU SLIRP, measured rather than asserted.
#
# This is a MEASUREMENT, not a pass/fail gate, and the number it prints is a
# TCG number: the guest is fully emulated, so the bottleneck is the host CPU
# executing translated x86, not the link. Quoting it as "LogitOS does N Mbit/s"
# would be dishonest. What it is good for is the DIFFERENCE between two builds
# of c/net/transport/tcp.c on the identical host, guest, image and file -- which
# is the only claim this path can honestly support.
#
# Method: a host HTTP server timestamps each GET as it arrives; the guest's
# `net get` prints its byte count when the body is complete; the elapsed time
# between the two is the transfer. http.c's RAW_MAX buffers the whole body
# (1 MiB), while net.c only checksums the first 128 KiB, so the per-fetch
# constant (FNV-1a over 128 KiB) is identical in both arms and independent of
# the file size -- which is why the file is sized just under RAW_MAX.
#
# WHAT THIS PATH CANNOT MEASURE, and why -- recorded so the next person does
# not spend an afternoon on it. QEMU user networking (SLIRP) is not a wire: it
# is a full host-side TCP/IP stack that TERMINATES the guest's connection and
# proxies the payload onward over a host socket. The guest's TCP peer is
# therefore SLIRP itself, one emulated hop away, with a sub-millisecond round
# trip -- whatever the far end is. Injecting delay upstream (a relaying proxy,
# a slow server) changes SLIRP's round trip to the server and never reaches the
# guest's congestion or receive window; the guest still sees a zero
# bandwidth-delay product and the numbers do not move. This was tried, with a
# relay that scheduled each chunk's delivery rather than sleeping in the data
# path, and 128 KiB still arrived in 66 ms across a nominal 50 ms RTT --
# because the guest never saw that RTT. Exercising a real window on-device
# needs a path where the guest's TCP peer is genuinely remote: a TAP interface
# with `tc netem`, which needs root, or two QEMUs joined by -netdev socket with
# a host stack on the same L2 segment. Until then the window-scaling and
# initial-window claims are carried by the host unit test, which drives the
# sender directly, and this script's job is to show the rewrite did not COST
# anything on the path we do have.
#
# Usage: run-tcp-throughput.sh <iso> <disk.img> [reps] [bytes]
# Env:   TCP_TP_LABEL=BEFORE  label for the printed table

set -u

ISO="${1:?usage: run-tcp-throughput.sh <iso> <disk.img> [reps] [bytes]}"
DISK="${2:?usage: run-tcp-throughput.sh <iso> <disk.img> [reps] [bytes]}"
REPS="${3:-3}"
SIZE="${4:-917504}"                 # 896 KiB: under http.c's 1 MiB RAW_MAX
QEMU="${QEMU:-qemu-system-x86_64}"
LABEL="${TCP_TP_LABEL:-tcp}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"
PORTFILE="$TMP/port"
REQLOG="$TMP/reqs"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    [ -n "${HPID:-}" ] && kill "$HPID" 2>/dev/null
    [ -n "${HPID:-}" ] && wait "$HPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

python3 - "$TMP" "$PORTFILE" "$REQLOG" "$SIZE" <<'PY' &
import http.server, pathlib, sys, time

root      = pathlib.Path(sys.argv[1])
port_file = pathlib.Path(sys.argv[2])
req_log   = pathlib.Path(sys.argv[3])
size      = int(sys.argv[4])
(root / "probe.bin").write_bytes(bytes((i * 31 + 7) & 0xff for i in range(size)))
req_log.write_text("", encoding="ascii")

class Timed(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        with req_log.open("a", encoding="ascii") as f:
            f.write("%.6f\n" % time.time())
        super().do_GET()
    def log_message(self, _f, *_a):
        pass

server = http.server.ThreadingHTTPServer(("0.0.0.0", 0),
    lambda *a, **kw: Timed(*a, directory=str(root), **kw))
port_file.write_text(str(server.server_port), encoding="ascii")
server.serve_forever()
PY
HPID=$!

for _ in $(seq 1 100); do
    [ -s "$PORTFILE" ] && break
    kill -0 "$HPID" 2>/dev/null || { echo "FAIL: host HTTP server exited"; exit 1; }
    sleep 0.05
done
[ -s "$PORTFILE" ] || { echo "FAIL: host HTTP server did not publish a port"; exit 1; }
PORT="$(cat "$PORTFILE")"

# One boot, REPS fetches. Each `net get` is its own connection, so each sample
# includes a fresh handshake and a fresh slow start -- which is the honest unit
# for a browser, and the case a large initial window is supposed to help.
{
    sleep 5
    for _ in $(seq 1 "$REPS"); do
        printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"
        sleep 12
    done
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
      -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      -netdev user,id=n0 -device e1000,netdev=n0 \
      -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Poll for each completion marker and stamp the moment it appears.
: > "$TMP/done"
seen=0
deadline=$(( $(date +%s) + 40 + 14 * REPS ))
while [ "$seen" -lt "$REPS" ]; do
    n=$(grep -ac "http bytes" "$LOG" 2>/dev/null); n=${n:-0}
    if [ "$n" -gt "$seen" ]; then
        while [ "$seen" -lt "$n" ]; do
            date +%s.%N >> "$TMP/done"
            seen=$((seen + 1))
        done
    fi
    [ "$(date +%s)" -gt "$deadline" ] && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.02
done

if [ "$seen" -eq 0 ]; then
    echo "FAIL: no completed fetch"
    echo "----- serial -----"; cat "$LOG"; echo "------------------"
    exit 1
fi

echo "=== TCP throughput (TCG/SLIRP; a DIFFERENCE between builds, not a link rate) ==="
awk -v size="$SIZE" -v label="$LABEL" '
    FNR == NR { start[FNR] = $1; ns = FNR; next }
              { end[FNR]   = $1; ne = FNR }
    END {
        best = 0; n = (ns < ne ? ns : ne);
        for (i = 1; i <= n; i++) {
            dt = end[i] - start[i];
            if (dt <= 0) continue;
            mbps = (size * 8.0 / dt) / 1e6;
            printf "  %-10s sample %d: %8.3f s  %7.2f Mbit/s\n", label, i, dt, mbps;
            if (mbps > best) best = mbps;
        }
        printf "  %-10s BEST  : %7.2f Mbit/s over %d bytes, %d sample(s)\n",
               label, best, size, n;
    }' "$REQLOG" "$TMP/done"

grep -a "http bytes" "$LOG" | tail -1
exit 0
