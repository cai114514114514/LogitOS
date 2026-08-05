#!/usr/bin/env bash
# End-to-end DHCP test: boot Aether under QEMU/SLIRP, assert that the DHCP
# client bound the SLIRP-issued lease (10.0.2.15) during net_init, then fetch
# a deterministic 32 KiB file over the leased address to prove the network is
# fully usable with the DHCP-provided config (ip/gw/dns).

set -u

ISO="${1:?usage: run-dhcp-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-dhcp-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"
PORTFILE="$TMP/port"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    [ -n "${HPID:-}" ] && kill "$HPID" 2>/dev/null
    [ -n "${HPID:-}" ] && wait "$HPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

python3 - "$TMP" "$PORTFILE" <<'PY' &
import http.server
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
port_file = pathlib.Path(sys.argv[2])
(root / "probe.bin").write_bytes(bytes((i * 31 + 7) & 0xff for i in range(32768)))

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

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 12; printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"; sleep 10; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0 \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

BOUND=0
for _ in $(seq 1 300); do
    if grep -aq "http bytes 32768 fnv1a" "$LOG"; then
        if grep -aq "\[dhcp\] bound 10.0.2.15" "$LOG"; then
            echo "PASS: DHCP client bound SLIRP lease 10.0.2.15"
            echo "PASS: guest fetched complete 32768-byte HTTP body using DHCP config"
            exit 0
        fi
        echo "FAIL: HTTP body fetched but DHCP lease marker '[dhcp] bound 10.0.2.15' missing"
        echo "----- serial output -----"
        cat "$LOG"
        echo "-------------------------"
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

if grep -aq "\[dhcp\] bound 10.0.2.15" "$LOG"; then
    echo "FAIL: DHCP lease bound, but complete TCP/HTTP body marker not seen"
else
    echo "FAIL: DHCP lease marker '[dhcp] bound 10.0.2.15' not seen during boot"
fi
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
