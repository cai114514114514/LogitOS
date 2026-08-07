#!/usr/bin/env bash
# Real-Internet HTTPS smoke test: boot Logit under QEMU/SLIRP and fetch a
# live site (default zh.wikipedia.org) through the full guest path:
# e1000 -> DHCP -> DNS -> TCP (RTT estimator, fast retransmit, close machine)
# -> TLS 1.3 -> HTTP. Asserts the `net get` marker line with a nonzero byte
# count. Requires outbound Internet from the host; TCG timing is relaxed.

set -u

ISO="${1:?usage: run-https-smoke.sh <iso> <disk.img> [url]}"
DISK="${2:?usage: run-https-smoke.sh <iso> <disk.img> [url]}"
URL="${3:-https://zh.wikipedia.org/}"
QEMU="${QEMU:-qemu-system-x86_64}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 15; printf 'net get %s\n' "$URL"; sleep 120; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 1200); do
    if grep -aq "http bytes [1-9]" "$LOG"; then
        grep -a "http bytes" "$LOG" | tail -1
        echo "PASS: live HTTPS fetch over the full guest TCP/TLS path"
        exit 0
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

echo "FAIL: no successful 'http bytes' marker from live HTTPS fetch of $URL"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
