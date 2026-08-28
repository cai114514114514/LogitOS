#!/usr/bin/env bash
# WHAT THE DEVICE MODEL ACTUALLY DOES WITH THE STATISTICS BLOCK.
#
# tests/nic.mk has documented this script's invocation since the probe knob
# landed; the script itself was never committed, so the one question the probe
# exists to answer has never been asked on a running machine. This is that
# script.
#
#   make eb-mpc/logit.iso eb-mpc/disk.img E1000_PROBE=1 BUILD=eb-mpc
#   bash tests/boot/run-e1000-mpc-probe.sh eb-mpc/logit.iso eb-mpc/disk.img
#
# The kernel half (c/drivers/net/e1000.c, e1000_probe_read) reads each
# statistics register TWICE IN A ROW with nothing in between and prints both.
# A read-to-clear register answers <n> then 0. A sticky one answers <n> twice
# -- and a sticky one under e1000_stats.h's `*sw += hw` is re-added to the
# software total once per sample period FOREVER, which is a drop counter that
# climbs by a constant with no traffic and no interrupts at all.
#
# THE CONTROL IS GPRC AND IT IS ASSERTED, NOT PRINTED. GPRC is read-to-clear
# in QEMU's model (hw/net/e1000.c: [GPRC] = mac_read_clr4). If this run cannot
# show `gprc a!=0 b=0` on at least one line, the probe has no discriminating
# power on this boot -- no traffic arrived, or the MMIO window is wrong, or
# reg_read got optimised away -- and every other column below is meaningless.
# That case EXITS NON-ZERO. A probe that reports "mpc a == b" without having
# shown that it can tell a clearing register from a sticky one is exactly the
# control CLAUDE.md rule 5 is about: it reads like evidence and is not.
#
# WATCH THE CONTROL FAIL: run this against a build WITHOUT E1000_PROBE=1. No
# [e1000-probe] line is emitted at all and the script exits 1 saying so.

set -u

ISO="${1:?usage: run-e1000-mpc-probe.sh <iso> <disk.img>}"
DISK="${2:?usage: run-e1000-mpc-probe.sh <iso> <disk.img>}"

QEMU="${QEMU:-qemu-system-x86_64}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"
PORTFILE="$TMP/port"

# Bytes to serve. Big enough that the receive burst outruns the 64-descriptor
# RX ring under TCG, which is the only way MPC becomes non-zero at all: the
# QEMU model increments it in exactly one place (e1000_receiver_overrun).
BYTES="${PROBE_BYTES:-4194304}"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    [ -n "${HPID:-}" ] && kill "$HPID" 2>/dev/null
    [ -n "${HPID:-}" ] && wait "$HPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

python3 - "$TMP" "$PORTFILE" "$BYTES" <<'PY' &
import http.server
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
port_file = pathlib.Path(sys.argv[2])
nbytes = int(sys.argv[3])
(root / "probe.bin").write_bytes(bytes((i * 31 + 7) & 0xff for i in range(nbytes)))

class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

server = http.server.ThreadingHTTPServer(("0.0.0.0", 0),
    lambda *args, **kwargs: Quiet(*args, directory=str(root), **kwargs))
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

# 12 s to reach a shell (DHCP is ~3 s), then the fetch, then 25 s of IDLE with
# the probe still sampling once a second. The idle tail is the load-bearing
# half: it is where a sticky counter keeps climbing with rx flat and irq (+0),
# which is the shape the live browsing session reported.
{ sleep 12; printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"; sleep 45; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial stdio -display none -no-reboot >"$LOG" 2>"$TMP/qemu.err" &
QPID=$!

for _ in $(seq 1 800); do
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null

[ -n "${PROBE_KEEP_LOG:-}" ] && cp "$LOG" "$PROBE_KEEP_LOG"

echo "===== [e1000-probe] lines (non-zero gprc/mpc/torl only) ====="
grep -a "e1000-probe" "$LOG" | grep -av "gprc a=0 b=0 | mpc a=0 b=0 | rnbc a=0 b=0 | torl=0" || true
echo "===== [e1000] stats lines ====="
grep -a "\[e1000\] stats" "$LOG" || true
echo "==============================="

NPROBE="$(grep -ac "e1000-probe" "$LOG" || true)"
if [ "${NPROBE:-0}" -eq 0 ]; then
    echo "FAIL: no [e1000-probe] line in the log."
    echo "      The ISO was almost certainly NOT built with E1000_PROBE=1."
    echo "      Settle it with: make <iso> E1000_PROBE=1 BUILD=<dir>"
    exit 1
fi

# ---- THE CONTROL ----------------------------------------------------------
# At least one line must show gprc a != 0 AND gprc b == 0. That is a register
# this run WATCHED clear. Without it nothing below discriminates.
CTRL="$(grep -a "e1000-probe" "$LOG" | \
        sed -nE 's/.*gprc a=([0-9]+) b=([0-9]+).*/\1 \2/p' | \
        awk '$1 != 0 && $2 == 0 { n++ } END { print n+0 }')"
echo "control: lines showing gprc a!=0 and b=0 : $CTRL"
if [ "$CTRL" -eq 0 ]; then
    echo "FAIL: the control never fired. No line showed a register clearing on"
    echo "      read, so this boot cannot tell a read-to-clear register from a"
    echo "      sticky one, and no conclusion about mpc/rnbc may be drawn."
    exit 1
fi

# ---- THE MEASUREMENT ------------------------------------------------------
echo "mpc  lines with a==b and a!=0 (STICKY -- re-added every sample) : $(
    grep -a e1000-probe "$LOG" | sed -nE 's/.*mpc a=([0-9]+) b=([0-9]+).*/\1 \2/p' |
    awk '$1 == $2 && $1 != 0 { n++ } END { print n+0 }')"
echo "mpc  lines with a!=0 and b==0 (clears on read)                  : $(
    grep -a e1000-probe "$LOG" | sed -nE 's/.*mpc a=([0-9]+) b=([0-9]+).*/\1 \2/p' |
    awk '$1 != 0 && $2 == 0 { n++ } END { print n+0 }')"
echo "rnbc lines with a!=0 (register is readable at all)              : $(
    grep -a e1000-probe "$LOG" | sed -nE 's/.*rnbc a=([0-9]+) b=([0-9]+).*/\1 \2/p' |
    awk '$1 != 0 { n++ } END { print n+0 }')"
echo "gorc lines with gorcl!=0 or gorch!=0                            : $(
    grep -a e1000-probe "$LOG" | sed -nE 's/.*gorcl=([0-9]+) gorch=([0-9]+).*/\1 \2/p' |
    awk '$1 != 0 || $2 != 0 { n++ } END { print n+0 }')"
echo "colc lines with colc!=0                                         : $(
    grep -a e1000-probe "$LOG" | sed -nE 's/.*colc=([0-9]+).*/\1/p' |
    awk '$1 != 0 { n++ } END { print n+0 }')"

echo "OK: probe ran, control fired on $CTRL line(s). Read the table above."
exit 0
