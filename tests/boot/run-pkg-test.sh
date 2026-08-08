#!/usr/bin/env bash
# The trust model, on the real machine.
#
# Three packages are on the disk image, all made by tools/lpk.c -- the same C
# the kernel verifies with -- and each one isolates a different verdict:
#
#   /pkg/hello.lpk     signed by the built-in development key   -> ACCEPTED
#   /pkg/tampered.lpk  the same package, one payload byte changed, signature
#                      untouched                                 -> REFUSED (digest)
#   /pkg/foreign.lpk   a PERFECTLY VALID signature by a key this machine does
#                      not hold                                  -> REFUSED (untrusted)
#
# That third one is the whole test. "Refuses a corrupted file" is a checksum;
# "refuses an intact file from a stranger" is a trust decision, and it is the
# one a verifier is most likely to conflate. It is checked both ways: refused
# normally, accepted under --any, so the failure is provably about TRUST and not
# about the bytes.
#
#   run-pkg-test.sh <iso> <disk.img>

set -u
ISO="${1:?usage: run-pkg-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-pkg-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 4; printf '/bin/pkgverify --roots\n/bin/pkgverify /pkg/hello.lpk\n/bin/pkgverify /pkg/tampered.lpk\n/bin/pkgverify /pkg/foreign.lpk\n/bin/pkgverify --any /pkg/foreign.lpk\n/bin/pkgverify --extract /tmp/out.bin /pkg/hello.lpk\nexit\n'; sleep 6; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 300); do
    n=$(grep -ac '^PKG_' "$LOG" 2>/dev/null); n=${n:-0}
    [ "$n" -ge 6 ] && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
tr -d '\r' < "$LOG" > "$LOG.c"; mv "$LOG.c" "$LOG"

fail=0
need() { if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

grep -a '^PKG_' "$LOG" | sed 's/^/     /'
echo

L1=$(grep -a '^PKG_' "$LOG" | sed -n 1p)   # hello
L2=$(grep -a '^PKG_' "$LOG" | sed -n 2p)   # tampered
L3=$(grep -a '^PKG_' "$LOG" | sed -n 3p)   # foreign
L4=$(grep -a '^PKG_' "$LOG" | sed -n 4p)   # foreign --any
L5=$(grep -a '^PKG_' "$LOG" | sed -n 5p)   # hello --extract (PKG_OK)
L6=$(grep -a '^PKG_' "$LOG" | sed -n 6p)   # PKG_EXTRACTED

if [ -z "$L4" ]; then
    echo "FAIL: /bin/pkgverify did not run (no PKG_ lines)"
    echo "----- serial -----"; cat "$LOG"; echo "------------------"
    exit 1
fi

need "the trusted signer list is compiled in" \
     "$(grep -aq 'logitos-dev' "$LOG" && echo 0 || echo 1)"
need "a package signed by the built-in key is ACCEPTED" \
     "$(echo "$L1" | grep -q '^PKG_OK .*signer=logitos-dev' && echo 0 || echo 1)"
need "a tampered payload is REFUSED (digest)" \
     "$(echo "$L2" | grep -q '^PKG_REFUSED -5' && echo 0 || echo 1)"
need "an intact package from an UNTRUSTED key is REFUSED" \
     "$(echo "$L3" | grep -q '^PKG_REFUSED -7' && echo 0 || echo 1)"
need "...and --any accepts the same bytes, so it was the TRUST that failed" \
     "$(echo "$L4" | grep -q '^PKG_OK .*signer=untrusted:' && echo 0 || echo 1)"
need "--extract wrote the payload only after the signature checked out" \
     "$(echo "$L6" | grep -q '^PKG_EXTRACTED ' && echo 0 || echo 1)"

if [ "$fail" = 0 ]; then echo; echo "PASS: the machine can check what it downloaded"; exit 0; fi
echo; echo "----- serial -----"; cat "$LOG"; echo "------------------"
exit 1
