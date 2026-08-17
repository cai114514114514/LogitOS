#!/usr/bin/env bash
# run-net-bench.sh -- LogitOS network throughput on a wire we control.
#
# READ tests/boot/netwire.py's header first; it explains why the obvious
# topology (QEMU -netdev user) CANNOT measure a TCP window, and what this one
# does instead.  Short version: SLIRP terminates the guest's connection, so
# under `-netdev user` the guest's TCP peer is one emulated hop away at
# sub-millisecond RTT and no window is ever the limit.  Here the guest's frames
# cross a host-side wire we own BEFORE they reach SLIRP, so the delay is on the
# guest's own connection and the bandwidth-delay product is real.
#
#   QEMU-A (LogitOS)                netwire.py               QEMU-B (no board)
#   -netdev socket,connect=:P1 <--> delay/loss/  <--> -netdev socket,listen=:P2
#                                   instrument       -netdev user (SLIRP)
#                                                     both on hub 0
#                                                              |
#                                                       host HTTP server
#
# QEMU-B runs `-machine none`: no board, no vCPU, nothing emulated -- only the
# main loop, in which a QEMU hub forwards frames between its socket port and
# its SLIRP port.  So SLIRP -- a real TCP stack with window scaling and
# congestion control -- becomes the guest's peer at whatever RTT we ask for.
# No root, no TAP, no tc.
#
# (`-S` on a real machine type does NOT work here: with the vCPU halted QEMU
# does not service the net layer and the far side answers nothing.  Measured:
# an ARP for 10.0.2.2 gets 0 bytes back under `-S`, 68 bytes under
# `-machine none`.)
#
# Every number is a TCG number: the guest is fully emulated.  They are only
# meaningful as a comparison on one host between two builds or two drivers,
# and the harness prints the topology with every result for that reason.
#
# Usage:
#   run-net-bench.sh <iso> <disk.img> [options]
#     --nic DEV[:DRV]   QEMU device to attach (default e1000); repeatable
#     --delay MS        one-way wire delay; RTT added is 2x this (default 0)
#     --loss PCT        uniform random frame loss on the wire (default 0)
#     --reps N          fetches per boot (default 3)
#     --bytes N         body size (default 917504, under http.c's 1 MiB cap)
#     --label TEXT      tag for the printed table
#     --json FILE       write the raw per-run reports here
#     --baseline        also run one arm on plain -netdev user, for reference
#     --logdir DIR      keep the per-arm serial log, wire report and QEMU-B log
#                       here (default build/net-bench; NET_BENCH_LOGDIR overrides)
#     --trace           ask netwire for a per-packet timeline into --logdir
#
# WHY --logdir IS ON BY DEFAULT.  Everything this harness runs in produces two
# guest-side lines that answer questions the wire cannot:
#
#   [http] get rc=.. status=.. t=..ms      c/kernel/gui/wm.c:2216
#   [net] rx path: frames .. irq .. softirq .. inline .. poll ..
#                                          c/net/core/net.c:154
#
# The first says whether the wire's goodput describes the WHOLE fetch or a
# burst inside a fetch that is mostly stall -- a different problem with a
# different fix.  The second says which context actually drains the ring.  Both
# were being written to a mktemp directory and deleted by the EXIT trap on every
# successful run, so the only way to see them was to make the run fail.  They
# are now copied out before the trap fires.
#
# WARNING about --trace: netwire writes one line per frame INSIDE its forwarding
# loop, which lowers the wire's own ceiling.  Read a traced run for SHAPE
# (window over time, inter-arrival spacing) and never quote a Mbit/s from one.
set -u

ISO="${1:?usage: run-net-bench.sh <iso> <disk.img> [options]}"; shift
DISK="${1:?usage: run-net-bench.sh <iso> <disk.img> [options]}"; shift

QEMU="${QEMU:-qemu-system-x86_64}"
NICS=()
DELAY=0
LOSS=0
REPS=3
SIZE=917504
LABEL="${NET_BENCH_LABEL:-net}"
JSON=""
BASELINE=0
LOGDIR="${NET_BENCH_LOGDIR:-}"
TRACE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --nic)      NICS+=("$2"); shift 2 ;;
        --delay)    DELAY="$2"; shift 2 ;;
        --loss)     LOSS="$2"; shift 2 ;;
        --reps)     REPS="$2"; shift 2 ;;
        --bytes)    SIZE="$2"; shift 2 ;;
        --label)    LABEL="$2"; shift 2 ;;
        --json)     JSON="$2"; shift 2 ;;
        --baseline) BASELINE=1; shift ;;
        --logdir)   LOGDIR="$2"; shift 2 ;;
        --trace)    TRACE=1; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ ${#NICS[@]} -gt 0 ] || NICS=("e1000:e1000")

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
[ -n "$LOGDIR" ] || LOGDIR="$ROOT/build/net-bench"
mkdir -p "$LOGDIR" || { echo "FAIL: cannot create logdir $LOGDIR" >&2; exit 2; }
TMP="$(mktemp -d)"
PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null
    done
    for p in "${PIDS[@]:-}"; do
        [ -n "$p" ] && wait "$p" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT

# ---------------------------------------------------------------- host origin
# One HTTP server for the whole run.  It is reached through SLIRP's 10.0.2.2
# alias, i.e. from QEMU-B's point of view it is "the internet".
python3 - "$TMP" "$TMP/port" "$SIZE" <<'PY' &
import http.server, pathlib, sys
root      = pathlib.Path(sys.argv[1])
port_file = pathlib.Path(sys.argv[2])
size      = int(sys.argv[3])
(root / "probe.bin").write_bytes(bytes((i * 31 + 7) & 0xff for i in range(size)))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, _f, *_a): pass
srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0),
        lambda *a, **kw: Quiet(*a, directory=str(root), **kw))
port_file.write_text(str(srv.server_port), encoding="ascii")
srv.serve_forever()
PY
HTTPD=$!; PIDS+=("$HTTPD")

for _ in $(seq 1 100); do
    [ -s "$TMP/port" ] && break
    kill -0 "$HTTPD" 2>/dev/null || { echo "FAIL: host HTTP server exited"; exit 1; }
    sleep 0.05
done
[ -s "$TMP/port" ] || { echo "FAIL: host HTTP server did not publish a port"; exit 1; }
PORT="$(cat "$TMP/port")"

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'; }

echo "=== netwire ceiling (this host, now) ==="
python3 "$HERE/netwire.py" --selftest | sed 's/^/  /'
echo
echo "=== LogitOS network benchmark ==="
echo "  topology : QEMU-A(guest) -> netwire(delay=${DELAY}ms/way, loss=${LOSS}%) -> QEMU-B(hub: socket+SLIRP) -> host httpd"
echo "  emulation: TCG (-accel tcg,thread=multi -smp 4 -m 512M); every number below is a TCG number"
echo "  body     : $SIZE bytes x $REPS fetches per boot"
echo

# ------------------------------------------------------------------- one arm
# $1 = qemu -device string, $2 = expected driver name, $3 = "wire"|"slirp"
run_arm() {
    local dev="$1" drv="$2" mode="$3"
    local log="$TMP/$drv.$mode.log" rep="$TMP/$drv.$mode.json"
    local tr="$TMP/$drv.$mode.trace"
    local qb="" wirepid="" p1="" p2=""
    local netargs

    if [ "$mode" = "wire" ]; then
        p1="$(free_port)"; p2="$(free_port)"
        # QEMU-B: `-machine none` -- no board, no vCPU, nothing emulated. Only
        # the main loop runs, and hub 0 joins the socket port to SLIRP inside
        # it. (`-S` on a normal machine does NOT work: with the vCPU halted
        # QEMU never services the net layer, and the far side answers nothing.
        # Verified: an ARP for 10.0.2.2 gets 0 bytes back under -S and a
        # 68-byte reply under -machine none.)
        "$QEMU" -machine none -nodefaults -display none -monitor none \
            -netdev socket,id=s,listen=127.0.0.1:$p2 \
            -netdev user,id=u \
            -netdev hubport,id=h0,hubid=0,netdev=s \
            -netdev hubport,id=h1,hubid=0,netdev=u \
            >"$TMP/qemub.log" 2>&1 &
        qb=$!; PIDS+=("$qb")
        python3 "$HERE/netwire.py" --listen 127.0.0.1:$p1 --connect 127.0.0.1:$p2 \
            --delay-ms "$DELAY" --loss-pct "$LOSS" --report "$rep" \
            $([ "$TRACE" = 1 ] && printf -- '--trace %s' "$tr") \
            >"$TMP/wire.log" 2>&1 &
        wirepid=$!; PIDS+=("$wirepid")
        sleep 1
        netargs="-netdev socket,id=n0,connect=127.0.0.1:$p1 -device $dev,netdev=n0"
    else
        netargs="-netdev user,id=n0 -device $dev,netdev=n0"
    fi

    # The guest is a full desktop: it composites, ticks and polls whether or not
    # anything is on the wire, so raw process CPU would be mostly floor. Sample
    # /proc/<qemu>/stat across an IDLE window first, then across the transfer
    # window, and report the DIFFERENCE -- host CPU seconds attributable to
    # moving the bytes.
    # PER: seconds between fetches. A 50 ms RTT stretches a slow start over
    # many more round trips, so this has to scale with the injected delay or
    # the later fetches are still in flight when the feed ends.
    local per=$(( 14 + ${DELAY%.*} / 2 ))
    {
        sleep 18
        for _ in $(seq 1 "$REPS"); do
            printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"
            sleep "$per"
        done
        # Hold stdin open until the harness says the arm is done. When the feed's
        # stdout closes, QEMU sees EOF on the serial console and exits -- which
        # silently truncated runs before the last fetch had finished and made the
        # CPU sample read a dead process. The sentinel (rather than a long sleep)
        # is because this subshell also inherits the harness's stderr, so a
        # lingering one holds the caller's pipe open long after the run.
        while [ ! -e "$TMP/$drv.$mode.stop" ]; do sleep 0.5; done
    } | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
        -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $netargs -serial stdio -display none -no-reboot >"$log" 2>"$TMP/$drv.$mode.err" &
    local qa=$!; PIDS+=("$qa")

    cpu_secs() {   # utime+stime of $1 (all threads) in seconds, or "" if gone
        awk -v hz="$(getconf CLK_TCK)" '{print ($14+$15)/hz}' "/proc/$1/stat" 2>/dev/null
    }
    sleep 13; local idle0; idle0="$(cpu_secs "$qa")"; local it0; it0="$(date +%s.%N)"
    sleep 4;  local idle1; idle1="$(cpu_secs "$qa")"; local it1; it1="$(date +%s.%N)"
    local busy0; busy0="$idle1"; local bt0; bt0="$it1"

    local deadline=$(( $(date +%s) + 40 + (per + 2) * REPS ))
    while :; do
        local n; n=$(grep -ac "http bytes" "$log" 2>/dev/null); n=${n:-0}
        [ "$n" -ge "$REPS" ] && break
        [ "$(date +%s)" -gt "$deadline" ] && break
        kill -0 "$qa" 2>/dev/null || break
        sleep 0.2
    done
    local busy1; busy1="$(cpu_secs "$qa")"; local bt1; bt1="$(date +%s.%N)"
    printf '%s %s %s %s %s %s\n' "$idle0" "$idle1" "$it0" "$it1" "$busy1" "$bt1" \
        > "$TMP/$drv.$mode.cpu"
    printf '%s %s\n' "$busy0" "$bt0" >> "$TMP/$drv.$mode.cpu"
    # A THIRD LINE USED TO GO HERE -- the relay's own CPU, idle-corrected the
    # same way the guest's is, printed as a duty cycle against payload flight
    # time with "WIRE-LIMITED, distrust every rate above" past 0.8. It is
    # deliberately NOT here, and the reason is worth more than the number was:
    # the numerator was CPU over the whole transfer WINDOW (fetches plus the
    # ~14 s gaps between them) and the denominator was only the milliseconds a
    # payload was in flight, so the idle correction had to be exactly right for
    # the ratio to mean anything -- and the idle window is sampled before
    # QEMU-A has connected, when netwire's select loop is a different loop.
    # An adversarial pass refused it. A rate that says "distrust every rate
    # above" is the last one that may itself be unverified, so it is gone
    # rather than annotated. --selftest's out-of-band ceiling, measured with no
    # QEMU competing, is what remains and it was never in question; and
    # relay_self_latency_ms below is a DIRECT measurement, not a ratio.
    sleep 1
    : > "$TMP/$drv.$mode.stop"
    kill "$qa" 2>/dev/null; wait "$qa" 2>/dev/null
    if [ -n "$wirepid" ]; then
        sleep 1; kill -INT "$wirepid" 2>/dev/null
        for _ in $(seq 1 40); do [ -s "$rep" ] && break; sleep 0.1; done
        kill "$wirepid" 2>/dev/null; wait "$wirepid" 2>/dev/null
    fi
    [ -n "$qb" ] && { kill "$qb" 2>/dev/null; wait "$qb" 2>/dev/null; }

    # Copy the evidence out BEFORE any `return` -- the EXIT trap rm -rf's $TMP,
    # so on a successful run these files used to exist only inside the harness.
    cp -f "$log"  "$LOGDIR/$drv.$mode.serial.log" 2>/dev/null
    cp -f "$rep"  "$LOGDIR/$drv.$mode.wire.json"  2>/dev/null
    cp -f "$tr"   "$LOGDIR/$drv.$mode.trace"      2>/dev/null
    cp -f "$TMP/$drv.$mode.err" "$LOGDIR/$drv.$mode.qemu.err" 2>/dev/null
    [ -n "$qb" ] && cp -f "$TMP/qemub.log" "$LOGDIR/$drv.$mode.qemub.log" 2>/dev/null
    [ -n "$wirepid" ] && cp -f "$TMP/wire.log" "$LOGDIR/$drv.$mode.wire.log" 2>/dev/null

    local bound done_n
    bound="$(grep -ao '\[net\] NIC bound: [a-z0-9-]*' "$log" | head -1 | sed 's/.*: //')"
    done_n=$(grep -ac "http bytes" "$log" 2>/dev/null); done_n=${done_n:-0}
    printf '%-12s %-6s ' "$drv" "$mode"
    if [ "$done_n" -eq 0 ]; then
        printf 'NO COMPLETED FETCH (bound=%s)\n' "${bound:-none}"
        echo "----- serial tail -----"; tail -25 "$log"; echo "-----------------------"
        return 1
    fi
    if [ -n "$bound" ] && [ "$bound" != "$drv" ]; then
        printf 'WRONG DRIVER: expected %s, bound %s\n' "$drv" "$bound"
        return 1
    fi
    echo
    python3 - "$rep" "$done_n" "$TMP/$drv.$mode.cpu" "$SIZE" "$DELAY" <<'PY'
import json, os, sys, statistics
rep_path, n, cpu_path, size = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
delay = float(sys.argv[5])

# --- host CPU attributable to the transfer -------------------------------
idle_rate = busy_rate = bytes_per_cpu = wire_cores = None
try:
    lines = open(cpu_path).read().split("\n")
    i0, i1, it0, it1, b1, bt1 = (float(x) for x in lines[0].split())
    b0, bt0 = (float(x) for x in lines[1].split())
    idle_rate = (i1 - i0) / (it1 - it0)          # host cores busy while idle
    busy_rate = (b1 - b0) / (bt1 - bt0)          # host cores busy incl. fetches
    extra_cpu = (b1 - b0) - idle_rate * (bt1 - bt0)
    if n:
        bytes_per_cpu = size * n / extra_cpu if extra_cpu > 0 else None
except Exception:
    pass

cpu_txt = "idle=%.2f cores busy=%.2f cores" % (idle_rate, busy_rate) \
          if idle_rate is not None else "cpu=n/a"
if bytes_per_cpu:
    cpu_txt += "  %.1f MB per extra host-CPU-second" % (bytes_per_cpu / 1e6)
if not os.path.exists(rep_path) or os.path.getsize(rep_path) == 0:
    print("    fetches=%d  (no wire in this arm)  %s" % (n, cpu_txt))
    sys.exit(0)

rep = json.load(open(rep_path))
# Inbound flows carrying the body; take everything over a tenth of the body so a
# short/failed fetch is visible rather than silently excluded.
big = [f for f in rep["flows"] if f["bytes"] > size // 10 and f["mbit_s"]]
rates = sorted(f["mbit_s"] for f in big)
rt   = [f["handshake_rtt_ms"] for f in rep["flows"] if f["handshake_rtt_ms"]]
retr = sum(f["retrans"] for f in big); segs = sum(f["segments"] for f in big)
ooo  = sum(f["ooo"] for f in big)
ws   = [f["wscale"] for f in rep["flows"] if f["wscale"] is not None]
if not rates:
    print("    fetches=%d but no large inbound flow crossed the wire" % n)
    sys.exit(0)
spread = "%.1f-%.1f" % (rates[0], rates[-1])
print("    goodput  median %7.1f Mbit/s   spread %-14s over %d flow(s)"
      % (statistics.median(rates), spread, len(rates)))
# The wire sits at the guest's end, so it sees SYN at t and the SYN/ACK one
# one-way delay later; the guest sees two. Report what the GUEST experiences.
guest_rtt = (statistics.median(rt) + delay) if rt else None
print("    wire     guest rtt %s ms   retrans %d/%d seg   ooo %d   wscale %s"
      % (("%.2f" % guest_rtt) if guest_rtt is not None else "n/a",
         retr, segs, ooo, (max(ws) if ws else "none")))
print("    frames   in %d / out %d   (%.2f KiB per inbound frame)"
      % (rep["frames_net_to_guest"], rep["frames_guest_to_net"],
         rep["octets_net_to_guest"] / 1024.0 / max(1, rep["frames_net_to_guest"])))
print("    host     %s" % cpu_txt)
# "Is the WIRE the thing being measured?" is the right question and the
# CPU-duty answer to it was withdrawn -- see the note beside the sampling in
# the shell above. What answers it here instead is the direct measurement
# below, which needs no idle correction and no window to divide by.
#
# The relay's own contribution to every latency it reports, measured rather
# than argued: select-return to socket-handoff, per loop iteration. Compare it
# against rx_to_ack_turnaround before believing that number is the guest's.
sl = rep.get("relay_self_latency_ms")
ta = rep.get("rx_to_ack_turnaround")
if sl:
    print("    relay    self-latency median %.3f ms  p90 %.3f  p99 %.3f  max %.3f "
          "(%.1f frames/iteration)"
          % (sl["median"], sl["p90"], sl["p99"], sl["max"],
             sl["frames_per_iteration"]))
    if ta:
        print("    turnarnd rx->ack median %.3f ms  p90 %.3f  max %.3f   "
              "-> relay accounts for %.0f%% of the median"
              % (ta["median_ms"], ta["p90_ms"], ta["max_ms"],
                 100.0 * sl["median"] / ta["median_ms"] if ta["median_ms"] else 0))
PY
    # The guest's own account of the same transfers. `t=` is the fetch's total
    # wall time as the guest measures it (10 ms resolution, TIMER_HZ 100); the
    # rx-path line says which context drained the ring. Both come from the log
    # this harness used to delete.
    local hl rl
    hl="$(grep -ao '\[http\] get rc=[-0-9]* status=[-0-9]* t=[0-9]*ms' "$log" | tr '\n' ' ')"
    # tr -d '\r' first: the serial log is CRLF, and `[^\r]` in a BRE bracket is
    # "not a backslash or an r", which truncates the line at "f" of "frames".
    rl="$(tr -d '\r' < "$log" | grep -ao '\[net\] rx path: .*' | tail -3 | sed 's/^/             /')"
    [ -n "$hl" ] && printf '    guest    %s\n' "$hl"
    [ -n "$rl" ] && printf '    rx path\n%s\n' "$rl"
    printf '    logs     %s/%s.%s.*\n' "$LOGDIR" "$drv" "$mode"
    return 0
}

RC=0
for spec in "${NICS[@]}"; do
    dev="${spec%%:*}"; drv="${spec##*:}"; [ "$dev" = "$drv" ] && drv="$dev"
    run_arm "$dev" "$drv" wire || RC=1
    if [ "$BASELINE" = 1 ]; then
        run_arm "$dev" "$drv" slirp || RC=1
    fi
done

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
exit $RC
