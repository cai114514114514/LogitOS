#!/usr/bin/env bash
# run-net-ab.sh -- paired A/B network throughput, because unpaired numbers on
# this host are noise.
#
# Read tests/boot/netwire.py and tests/boot/run-net-bench.sh first; this uses
# the same wire and the same topology, one per arm.  What it adds is the only
# thing that makes a difference believable here.
#
# WHY PAIRED.  This host runs other agents' QEMU processes.  Measured while
# writing this: load average 16.9 on 28 cores, five other qemu-system-x86
# processes at 200-300% each.  Two consecutive unpaired runs of the IDENTICAL
# build gave medians of 220, 147 and 83 Mbit/s -- a 2.6x spread from host load
# alone, which is larger than any change to the guest's network stack is going
# to be.  Quoting "before 147, after 190" from runs half an hour apart would be
# reporting the host's other tenants.
#
# So: every arm boots at the same time, and the harness issues fetches
# ROUND-ROBIN, one at a time, waiting for each to finish before starting the
# next.  Sample i of arm A and sample i of arm B are seconds apart rather than
# minutes, and only one transfer is ever in flight, so the arms are not
# competing with each other either.  The report is the per-rep RATIO's median,
# not the ratio of medians.
#
# Usage:
#   run-net-ab.sh --disk <disk.img> --arm LABEL:ISO:QEMUDEV[:DRIVER] [--arm ...]
#                 [--reps N] [--delay MS] [--loss PCT] [--bytes N] [--json F]
#
# Examples:
#   # two builds, same NIC
#   run-net-ab.sh --disk build/disk.img \
#     --arm before:build/logit-before.iso:e1000 \
#     --arm after:build/logit.iso:e1000 --reps 9
#
#   # three NICs, same build
#   run-net-ab.sh --disk build/disk.img \
#     --arm e1000:build/logit.iso:e1000 \
#     --arm virtio:build/logit.iso:virtio-net-pci:virtio-net \
#     --arm rtl8139:build/logit.iso:rtl8139 --reps 9
set -u

QEMU="${QEMU:-qemu-system-x86_64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DISK=""
ARMS=()
REPS=7
DELAY=0
LOSS=0
SIZE=917504
JSON=""

while [ $# -gt 0 ]; do
    case "$1" in
        --disk)  DISK="$2"; shift 2 ;;
        --arm)   ARMS+=("$2"); shift 2 ;;
        --reps)  REPS="$2"; shift 2 ;;
        --delay) DELAY="$2"; shift 2 ;;
        --loss)  LOSS="$2"; shift 2 ;;
        --bytes) SIZE="$2"; shift 2 ;;
        --json)  JSON="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -n "$DISK" ] || { echo "--disk is required" >&2; exit 2; }
[ ${#ARMS[@]} -ge 1 ] || { echo "at least one --arm is required" >&2; exit 2; }

TMP="$(mktemp -d)"
PIDS=()
FIFOS=()
cleanup() {
    for f in "${FIFOS[@]:-}"; do [ -n "$f" ] && exec {fd}>&- 2>/dev/null; done
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT

# ---------------------------------------------------------------- host origin
python3 - "$TMP" "$TMP/port" "$SIZE" <<'PY' &
import http.server, pathlib, sys
root, port_file, size = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3])
(root / "probe.bin").write_bytes(bytes((i * 31 + 7) & 0xff for i in range(size)))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, _f, *_a): pass
srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0),
        lambda *a, **kw: Quiet(*a, directory=str(root), **kw))
port_file.write_text(str(srv.server_port), encoding="ascii")
srv.serve_forever()
PY
PIDS+=("$!")
for _ in $(seq 1 100); do [ -s "$TMP/port" ] && break; sleep 0.05; done
[ -s "$TMP/port" ] || { echo "FAIL: host HTTP server did not start"; exit 1; }
PORT="$(cat "$TMP/port")"

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'; }

echo "=== netwire ceiling (this host, now) ==="
python3 "$HERE/netwire.py" --selftest | sed 's/^/  /'
echo
echo "=== paired A/B: $(uptime | sed 's/.*load average/load/') ==="
echo "  topology : per arm, QEMU-A(guest) -> netwire(delay=${DELAY}ms/way, loss=${LOSS}%) -> QEMU-B(hub: socket+SLIRP) -> host httpd"
echo "  emulation: TCG (-accel tcg,thread=multi -smp 4 -m 512M); TCG numbers, comparison only"
echo "  protocol : all arms booted together, fetches issued ROUND-ROBIN one at a time"
echo "  body     : $SIZE bytes x $REPS reps per arm"
echo

# ------------------------------------------------------------------ boot arms
NAMES=(); LOGS=(); REPORTS=(); INFDS=()
for spec in "${ARMS[@]}"; do
    IFS=: read -r label iso dev drv <<<"$spec"
    [ -n "${drv:-}" ] || drv="$dev"
    NAMES+=("$label"); LOGS+=("$TMP/$label.log"); REPORTS+=("$TMP/$label.json")
    echo "$drv" > "$TMP/$label.drv"

    p1="$(free_port)"; p2="$(free_port)"
    "$QEMU" -machine none -nodefaults -display none -monitor none \
        -netdev socket,id=s,listen=127.0.0.1:$p2 -netdev user,id=u \
        -netdev hubport,id=h0,hubid=0,netdev=s \
        -netdev hubport,id=h1,hubid=0,netdev=u >"$TMP/$label.qb" 2>&1 &
    PIDS+=("$!")
    python3 "$HERE/netwire.py" --listen 127.0.0.1:$p1 --connect 127.0.0.1:$p2 \
        --delay-ms "$DELAY" --loss-pct "$LOSS" --report "$TMP/$label.json" \
        >"$TMP/$label.wire" 2>&1 &
    echo "$!" > "$TMP/$label.wirepid"; PIDS+=("$!")
    sleep 0.5

    # A fifo, not a pipeline: the harness has to write a command mid-run and
    # keep the console open, which `{ ...; } | qemu` cannot do.
    mkfifo "$TMP/$label.in"
    exec {fd}<>"$TMP/$label.in"
    INFDS+=("$fd")
    "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$iso" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
        -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        -netdev socket,id=n0,connect=127.0.0.1:$p1 -device "$dev",netdev=n0 \
        -serial stdio -display none -no-reboot \
        <"$TMP/$label.in" >"$TMP/$label.log" 2>"$TMP/$label.err" &
    echo "$!" > "$TMP/$label.qapid"; PIDS+=("$!")
done

# Wait for every arm's shell prompt before timing anything.
echo -n "  booting"
# Generous, and scaled by the number of arms: they all boot at once, under TCG,
# on a host that is already running other agents' QEMU. Three guests booting
# together have taken over two minutes.
deadline=$(( $(date +%s) + 150 + 90 * ${#NAMES[@]} ))
for i in "${!NAMES[@]}"; do
    # Two markers, because the serial console is shared by several threads and
    # they interleave WITHIN a line: one run printed "[sched] first-run tid 5
    # (FiLnder)" -- the shell's banner landing in the middle of the scheduler's
    # word -- and a single-marker wait sat there until it timed out on a guest
    # that had been up for four minutes.
    while ! grep -aqE "LogitOS shell|first-run tid [0-9]+ \(sh\)" "${LOGS[$i]}" 2>/dev/null; do
        [ "$(date +%s)" -gt "$deadline" ] && { echo; echo "FAIL: ${NAMES[$i]} never reached a shell"; tail -20 "${LOGS[$i]}"; exit 1; }
        sleep 0.5
    done
    echo -n " ${NAMES[$i]}"
done
echo " -- all up"
sleep 3

# --------------------------------------------------------------- round-robin
timeout_per=$(( 20 + ${DELAY%.*} ))
for rep in $(seq 1 "$REPS"); do
    for i in "${!NAMES[@]}"; do
        label="${NAMES[$i]}"; log="${LOGS[$i]}"
        before=$(grep -ac "http bytes" "$log" 2>/dev/null); before=${before:-0}
        printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT" >&"${INFDS[$i]}"
        t0=$(date +%s)
        while :; do
            n=$(grep -ac "http bytes" "$log" 2>/dev/null); n=${n:-0}
            [ "$n" -gt "$before" ] && break
            [ $(( $(date +%s) - t0 )) -gt "$timeout_per" ] && break
            sleep 0.05
        done
        sleep 0.4          # let the FIN settle before the next arm starts
    done
    echo "  rep $rep/$REPS done"
done

# --------------------------------------------------------------------- report
for i in "${!NAMES[@]}"; do
    label="${NAMES[$i]}"
    kill "$(cat "$TMP/$label.qapid")" 2>/dev/null
done
sleep 1
for i in "${!NAMES[@]}"; do
    label="${NAMES[$i]}"
    kill -TERM "$(cat "$TMP/$label.wirepid")" 2>/dev/null
done
for _ in $(seq 1 60); do
    ok=1
    for i in "${!NAMES[@]}"; do [ -s "${REPORTS[$i]}" ] || ok=0; done
    [ "$ok" = 1 ] && break
    sleep 0.2
done

echo
python3 - "$TMP" "$SIZE" "$DELAY" "${NAMES[@]}" <<'PY'
import json, os, statistics, sys
tmp, size, delay = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
names = sys.argv[4:]
series = {}
for nm in names:
    p = os.path.join(tmp, nm + ".json")
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        print("  %-12s NO WIRE REPORT" % nm); series[nm] = []
        continue
    rep = json.load(open(p))
    # Flows are in chronological order; each large inbound flow is one fetch.
    # netwire lists flows in the order it first saw them, i.e. chronological, so
    # index i is rep i and pairs with index i of the other arm.
    big = [f for f in rep["flows"] if f["bytes"] > size // 2 and f["mbit_s"]]
    rates = [f["mbit_s"] for f in big]
    series[nm] = rates
    rt = [f["handshake_rtt_ms"] for f in rep["flows"] if f["handshake_rtt_ms"]]
    retr = sum(f["retrans"] for f in big); segs = sum(f["segments"] for f in big)
    drv = open(os.path.join(tmp, nm + ".drv")).read().strip()
    bound = ""
    for line in open(os.path.join(tmp, nm + ".log"), errors="ignore"):
        if "NIC bound:" in line:
            bound = line.split("NIC bound:")[1].split()[0]; break
    warn = "" if (not bound or bound == drv) else "  !! bound %s, wanted %s" % (bound, drv)
    if not rates:
        print("  %-12s no completed transfer%s" % (nm, warn)); continue
    q = statistics.quantiles(rates, n=4) if len(rates) >= 4 else [min(rates)]*3
    print("  %-12s n=%-3d median %7.1f  IQR %6.1f-%-6.1f  min %6.1f max %6.1f  "
          "rtt %5.2f ms  retrans %d/%d%s"
          % (nm, len(rates), statistics.median(rates), q[0], q[2],
             min(rates), max(rates),
             (statistics.median(rt) + delay) if rt else float("nan"),
             retr, segs, warn))
    t = rep.get("rx_to_ack_turnaround")
    if t:
        # The latency the guest owns: segment on the wire -> guest's ack of it.
        print("  %-12s   rx->ack turnaround n=%d  median %.3f ms  p90 %.3f  p99 %.3f  max %.3f"
              % ("", t["n"], t["median_ms"], t["p90_ms"], t["p99_ms"], t["max_ms"]))

if len(names) >= 2:
    base = names[0]
    print("\n  paired ratio vs '%s' (per-rep, contemporaneous):" % base)
    for nm in names[1:]:
        a, b = series[base], series[nm]
        k = min(len(a), len(b))
        if k < 2:
            print("    %-12s not enough paired samples" % nm); continue
        ratios = [b[i] / a[i] for i in range(k) if a[i] > 0]
        med = statistics.median(ratios)
        wins = sum(1 for r in ratios if r > 1.0)
        print("    %-12s median ratio %.3f  (range %.2f-%.2f)  faster in %d/%d reps"
              % (nm, med, min(ratios), max(ratios), wins, len(ratios)))
PY

if [ -n "$JSON" ]; then
    python3 - "$TMP" "$JSON" <<'PY'
import json, pathlib, sys
out = {}
for p in sorted(pathlib.Path(sys.argv[1]).glob("*.json")):
    try: out[p.stem] = json.load(open(p))
    except Exception: pass
pathlib.Path(sys.argv[2]).write_text(json.dumps(out, indent=2))
PY
    echo "  raw wire reports -> $JSON"
fi
exit 0
