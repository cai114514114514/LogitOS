#!/usr/bin/env bash
# End-to-end TCP test: serve a deterministic 32 KiB file on the host, boot
# Aether under QEMU/SLIRP, fetch it through the guest's real e1000 -> IPv4 ->
# TCP -> HTTP path, and assert that the full body reached a ring-3 CLI process.

set -u

ISO="${1:?usage: run-net-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-net-test.sh <iso> <disk.img>}"
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
{ sleep 4; printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"; sleep 8; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0 \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 300); do
    if grep -aq "http bytes 32768 fnv1a" "$LOG"; then
        echo "PASS: guest fetched complete 32768-byte HTTP body over e1000/IPv4/TCP"
        exit 0
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: complete TCP/HTTP body marker not seen"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
