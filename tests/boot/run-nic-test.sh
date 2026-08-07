#!/usr/bin/env bash
# Per-NIC end-to-end proof. Boots LogitOS with ONE specific network card
# attached and asserts, from the serial log, the whole chain that card has to
# make work:
#
#   1. the registry bound the driver we expect       ([net] NIC bound: <name>)
#   2. the card got a DHCP lease from SLIRP          ([dhcp] bound 10.0.2.15)
#   3. a real HTTP fetch delivered every byte        (http bytes 32768 ...)
#
# Link-up is not the claim. (1) alone would pass on a card that enumerates and
# then receives nothing, and (2) alone would pass on a card that can do a
# 300-byte broadcast exchange and nothing larger. (3) is the claim: 32 KiB of
# TCP, in both directions, through this card's descriptor rings.
#
# Usage: run-nic-test.sh <iso> <disk.img> <qemu -device string> <expected driver>
#   e.g. run-nic-test.sh build/logit.iso build/disk.img rtl8139 rtl8139
#        run-nic-test.sh build/logit.iso build/disk.img virtio-net-pci virtio-net

set -u

ISO="${1:?usage: run-nic-test.sh <iso> <disk.img> <qemu-device> <driver-name>}"
DISK="${2:?usage: run-nic-test.sh <iso> <disk.img> <qemu-device> <driver-name>}"
DEV="${3:?usage: run-nic-test.sh <iso> <disk.img> <qemu-device> <driver-name>}"
DRV="${4:?usage: run-nic-test.sh <iso> <disk.img> <qemu-device> <driver-name>}"

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

# 12 s before the fetch: DHCP negotiation takes up to ~3 s and the desktop has
# to be up enough to run /bin/sh. Recorded QEMU quirk worth remembering: the
# e1000 model defers its RX-queue flush by about a second after RX is enabled,
# so a card that "receives nothing" this early is usually the model's timing,
# not the descriptor ring.
{ sleep 12; printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"; sleep 12; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device "$DEV",netdev=n0 \
    -serial stdio -display none -no-reboot >"$LOG" 2>"$TMP/qemu.err" &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "http bytes 32768 fnv1a" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fail() {
    echo "FAIL[$DEV]: $1"
    echo "----- serial output -----"; cat "$LOG"
    if [ -s "$TMP/qemu.err" ]; then echo "----- qemu stderr -----"; cat "$TMP/qemu.err"; fi
    echo "-------------------------"
    exit 1
}

BOUND="$(grep -ao '\[net\] NIC bound: [a-z0-9-]*' "$LOG" | head -1 | sed 's/.*: //')"
[ -n "$BOUND" ] || fail "no NIC was bound at all"
[ "$BOUND" = "$DRV" ] || fail "expected driver '$DRV', the registry bound '$BOUND'"
grep -aq "\[dhcp\] bound 10.0.2.15" "$LOG" || fail "driver '$DRV' bound but got no DHCP lease"
grep -aq "http bytes 32768 fnv1a" "$LOG" || fail "DHCP lease taken but the 32768-byte HTTP body never completed"

MAC="$(grep -ao '\[net\] NIC bound: .*' "$LOG" | head -1)"
echo "PASS[$DEV]: $MAC"
echo "PASS[$DEV]: DHCP lease 10.0.2.15, and a complete 32768-byte HTTP body over $DRV"
exit 0
