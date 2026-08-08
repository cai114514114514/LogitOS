#!/usr/bin/env bash
# INBOUND, PROVEN FROM THE HOST.
#
# Boots LogitOS, starts /bin/httpd on guest port 8080, and then the HOST opens
# TCP connections INTO the guest and fetches files. Every byte crosses a real
# NIC, a real IPv4 stack and a real TCP three-way handshake in the direction
# this machine could not do at all until now: the guest is the one receiving
# the SYN.
#
# WHY NOT LOOPBACK. A guest connecting to itself would exercise the same
# tcp.c on both ends and could pass with a stack no other implementation
# would talk to -- wrong option negotiation, a SYN-ACK offering window scale
# to a client that never asked, a sequence space that only agrees with itself.
# The peer here is Python's socket module talking to QEMU's SLIRP, neither of
# which is ours.
#
# ------------------------------------------------------------------------
# THE QEMU INVOCATION, written down because it costs an hour to rediscover.
#
# SLIRP ("-netdev user") is a NAT. Outbound works with no configuration, which
# is why every other network harness in this tree just says `-netdev user`. An
# INBOUND connection has nowhere to go unless a port is forwarded explicitly:
#
#     -netdev user,id=n0,hostfwd=tcp:127.0.0.1:<HOSTPORT>-10.0.2.15:8080
#
# reads as "listen on the HOST at 127.0.0.1:<HOSTPORT> and forward to
# 10.0.2.15:8080 in the guest". 10.0.2.15 is SLIRP's default DHCP lease and is
# what net.c configures (statically or via DHCP, the same address either way);
# `-:8080` is the shorthand that means the same thing and is worth avoiding in
# a test, because if the guest ever takes a different address the shorthand
# silently keeps working while pointing somewhere else.
#
# Two things that will waste time if they are not said:
#   * The host side MUST be bound somewhere reachable. 127.0.0.1 is chosen
#     deliberately so a test never opens a port to the outside world.
#   * The forward is set up by QEMU at startup, so connecting BEFORE the guest
#     is listening gives a connection that opens and then dies, not a refusal.
#     That is why this script waits for HTTPD_READY on the serial console
#     rather than retrying blindly.
# ------------------------------------------------------------------------
#
# What is asserted, and none of it is "it did not crash":
#   1. A 35 KB file comes back BYTE FOR BYTE. It is larger than the 32 KiB TCP
#      send ring, so it cannot be produced by one write -- the response is
#      segmented, windowed and ACK-driven, and a length check would not see a
#      block delivered twice.
#   2. TWO CONNECTIONS AT ONCE both get their own complete answer. This is the
#      property the host-side negative control attacks: a stack that reuses one
#      connection block serves the first client perfectly and corrupts it when
#      the second arrives.
#   3. A missing file is a 404, not a hang and not a reset.
#   4. The kernel's own counters, fetched over the network from /_stat, agree
#      with what happened.

set -u

ISO="${1:?usage: run-httpd-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-httpd-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
GUEST_PORT="${GUEST_PORT:-8080}"
SERVE="${SERVE:-/licenses/GPL-3.0-or-later.txt}"
LOCAL="${LOCAL:-LICENSES/GPL-3.0-or-later.txt}"
NREQ=5                        # httpd exits after this many, so the run ends

TMP="$(mktemp -d)"
LOG="$TMP/serial.log"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

[ -f "$LOCAL" ] || { echo "FAIL: $LOCAL missing (nothing to compare against)"; exit 1; }

# A free host port, picked by the kernel so parallel harnesses cannot collide.
HOSTPORT="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"
[ -n "$HOSTPORT" ] || { echo "FAIL: could not pick a host port"; exit 1; }

NET="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:${HOSTPORT}-10.0.2.15:${GUEST_PORT} \
     -device e1000,netdev=n0"

# file.locking=off alongside -snapshot: QEMU otherwise takes a write lock on the
# disk image and locks out every other harness (and `make run`) on this tree.
{ sleep 6; printf 'httpd %s / %s\n' "$GUEST_PORT" "$NREQ"; sleep 120; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

echo "waiting for the guest to listen on ${GUEST_PORT} (host 127.0.0.1:${HOSTPORT})..."
READY=0
for _ in $(seq 1 1200); do
    if grep -aq "HTTPD_READY" "$LOG"; then READY=1; break; fi
    if grep -aq "HTTPD_FAIL" "$LOG"; then
        echo "FAIL: the guest could not listen:"
        grep -a "HTTPD_FAIL" "$LOG"
        exit 1
    fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
[ "$READY" = 1 ] || {
    echo "FAIL: no HTTPD_READY on the serial console"
    tail -40 "$LOG"
    exit 1
}
grep -a "HTTPD_READY" "$LOG"

python3 - "$HOSTPORT" "$SERVE" "$LOCAL" <<'PY'
import socket, sys, threading

port = int(sys.argv[1])
path = sys.argv[2]
want = open(sys.argv[3], "rb").read()
fails = []

def fetch(target, timeout=90.0, hold=None):
    """One HTTP/1.0 request over a connection the HOST opens. Returns
    (status_line, headers, body). `hold` is an event: when given, the request
    is sent and then the connection is kept open until it fires, which is how
    two connections are made to overlap."""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.settimeout(timeout)
    s.sendall(("GET %s HTTP/1.0\r\nHost: logitos\r\n\r\n" % target).encode())
    if hold is not None:
        hold.wait(timeout)
    buf = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    s.close()
    head, _, body = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = lines[0].decode("latin-1") if lines else ""
    hdrs = {}
    for h in lines[1:]:
        k, _, v = h.partition(b":")
        hdrs[k.decode("latin-1").lower()] = v.strip().decode("latin-1")
    return status, hdrs, body

# 1) A 35 KB file, byte for byte. Larger than the 32 KiB send ring on purpose:
#    it cannot be produced by a single write, so this exercises segmentation,
#    the peer's window and the ACK-driven drain -- and a length check would not
#    notice a block handed out twice.
st, hdrs, body = fetch(path)
print("HTTPD_TEST fetch1 status=%r len=%d want=%d" % (st, len(body), len(want)))
if "200" not in st:
    fails.append("single fetch status %r" % st)
if body != want:
    n = min(len(body), len(want))
    first = next((i for i in range(n) if body[i] != want[i]), n)
    fails.append("single fetch: %d bytes, wanted %d, first difference at byte %d"
                 % (len(body), len(want), first))
if hdrs.get("content-length") != str(len(want)):
    fails.append("Content-Length %r, wanted %d" % (hdrs.get("content-length"), len(want)))

# 2) TWO AT ONCE. Both connections are opened and both requests are sent before
#    either is allowed to read, so the second SYN reaches the guest while the
#    first connection is live -- which is the case a one-client test cannot
#    see, and the one the negative control breaks.
gate = threading.Event()
out = {}
def worker(tag):
    try:
        out[tag] = fetch(path, hold=gate)
    except Exception as e:                       # noqa: BLE001 -- reported below
        out[tag] = ("EXC %s" % e, {}, b"")
ta = threading.Thread(target=worker, args=("a",))
tb = threading.Thread(target=worker, args=("b",))
ta.start(); tb.start()
import time
time.sleep(2.0)                                  # both are connected and sent
gate.set()
ta.join(120); tb.join(120)
for tag in ("a", "b"):
    if tag not in out:
        fails.append("concurrent %s: never finished" % tag)
        continue
    st2, _, b2 = out[tag]
    print("HTTPD_TEST concurrent-%s status=%r len=%d" % (tag, st2, len(b2)))
    if "200" not in st2:
        fails.append("concurrent %s status %r" % (tag, st2))
    elif b2 != want:
        fails.append("concurrent %s: %d bytes, wanted %d -- the two connections "
                     "did not each get their own answer" % (tag, len(b2), len(want)))

# 3) A missing file is a 404, from a server that is still healthy afterwards.
st3, _, _ = fetch("/no/such/file.txt")
print("HTTPD_TEST missing status=%r" % st3)
if "404" not in st3:
    fails.append("missing file status %r (wanted 404)" % st3)

# 4) The kernel's own counters, read over the network.
st4, _, b4 = fetch("/_stat")
print("HTTPD_TEST stat status=%r" % st4)
stat = {}
for line in b4.decode("latin-1").split("\n"):
    parts = line.split()
    if len(parts) == 2:
        try:
            stat[parts[0]] = int(parts[1])
        except ValueError:
            pass
print("HTTPD_TEST counters %s" % stat)
if stat.get("accepted", 0) < 4:
    fails.append("kernel accepted only %r connections, wanted >= 4" % stat.get("accepted"))
if stat.get("listeners", 0) < 1:
    fails.append("kernel reports %r listeners" % stat.get("listeners"))
if stat.get("free_conns", 0) < 8:
    fails.append("only %r free connection slots -- the client reserve was eaten"
                 % stat.get("free_conns"))

if fails:
    print("HTTPD_TEST_FAIL")
    for f in fails:
        print("  - %s" % f)
    sys.exit(1)
print("HTTPD_TEST_OK")
PY
RC=$?

# The guest's own verdict: it counts what it served and prints the kernel's
# refusal counters, which is the half the host cannot see.
for _ in $(seq 1 200); do
    grep -aq "HTTPD_DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
echo "----- what the guest said -----"
grep -a "HTTPD_READY\|\[httpd\]\|HTTPD_DONE\|HTTPD_FAIL" "$LOG" || true
echo "-------------------------------"

if [ "$RC" != 0 ]; then
    echo "FAIL: the host could not fetch from the guest"
    exit 1
fi
if ! grep -aq "HTTPD_DONE" "$LOG"; then
    echo "FAIL: the guest never finished (no HTTPD_DONE)"
    exit 1
fi
echo "PASS: the host fetched $(wc -c <"$LOCAL") bytes from the guest over inbound TCP,"
echo "      two connections at once each got their own answer, and 404 works"
exit 0
