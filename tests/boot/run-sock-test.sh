#!/usr/bin/env bash
# Non-blocking sockets, end to end: four connections open AT THE SAME TIME.
#
# Serves a deterministic 32 KiB file from the host, boots LogitOS, and runs
# /bin/socktest -- which opens four sockets before polling any of them, queues a
# request on each, and reports two things a serial implementation cannot fake:
#
#   * SOCKTEST_OVERLAP: the last connection came up BEFORE the first one
#     finished, so all four were live at the same instant.
#   * SOCKTEST_SWITCHES: consecutive events came from different sockets more
#     often than the n-1 a strictly sequential run would produce.
#
# The guest does the asserting (it has the timestamps); this script serves the
# bytes, drives the shell, and greps for the verdict.
#
# Kernel sockets cannot be host-tested -- they are a syscall over a real TCP
# stack over a real NIC -- so this is where the socket layer is proved at all.

set -u

ISO="${1:?usage: run-sock-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-sock-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
NSOCK="${NSOCK:-4}"
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

# Threading matters: four guest connections arrive at once and a single-threaded
# server would serialise them on the HOST, hiding the very property under test.
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
# file.locking=off alongside -snapshot: QEMU otherwise takes a write lock on the
# disk image and locks out every other harness (and `make run`) on this tree.
{ sleep 5; printf 'socktest 10.0.2.2 %s /probe.bin %s\n' "$PORT" "$NSOCK"; sleep 40; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 600); do
    if grep -aq "SOCKTEST_OK" "$LOG"; then
        grep -a "SOCKTEST_OVERLAP\|SOCKTEST_SWITCHES\|SOCKTEST_OK" "$LOG"
        echo "PASS: $NSOCK connections were live simultaneously and interleaved"
        exit 0
    fi
    if grep -aq "SOCKTEST_FAIL" "$LOG"; then
        break
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: no SOCKTEST_OK verdict"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
