#!/usr/bin/env bash
# run-link-test.sh -- the link layer, asserted from the WIRE.
#
# WHY A PCAP AND NOT THE GUEST'S COUNTERS
# =======================================
# The guest's own counters are the thing under test. A build whose eth_send
# forgot to pad would report `tx_padded 0` perfectly honestly, and a build that
# never announced its address would report `tx_announce 0` and pass any check
# written against itself. So the assertions here are made against what QEMU's
# filter-dump recorded actually crossing the wire, which the guest cannot lie
# about, and the boot log is used only for diagnostics.
#
# THE THREE PROPERTIES, and what each one was before:
#
#  1. EVERY TRANSMITTED FRAME IS AT LEAST 60 BYTES. IEEE 802.3's minimum. An
#     ARP packet is 14 + 28 = 42, so before eth_send padded, every ARP frame
#     this OS sent was an illegal runt. It worked because two of the four NIC
#     drivers happen to pad (rtl8139 in software, e1000 via TCTL.PSP) -- so
#     whether ARP worked at all was a property of which card was plugged in,
#     and on the two that do not pad, a real switch drops the frame.
#
#  2. THE MACHINE ANNOUNCES ITS ADDRESS. RFC 5227 s3: a broadcast ARP request
#     with sender == target == our own address, sent when the address is taken.
#     This OS had never sent one. Nothing on the segment learned our binding
#     until we happened to talk to it, no switch learned our port until we
#     transmitted, and a second host on the same address was never noticed.
#
#  3. RESOLUTION IS PACED. The old arp_resolve() broadcast a request on EVERY
#     call, so the request rate was whatever the caller's retry rate happened
#     to be -- sock.c carries a hand-written rate limiter whose comment says
#     exactly that. The state machine now paces retransmits itself, so a whole
#     boot plus a bulk fetch must contain only a handful of requests per
#     target, not one per packet.
#
# All three fail on the build this replaces, which is the point of asserting
# them from outside.
#
# Usage: run-link-test.sh <iso> <disk.img> [qemu-device]
set -u

ISO="${1:?usage: run-link-test.sh <iso> <disk.img> [qemu-device]}"
DISK="${2:?usage: run-link-test.sh <iso> <disk.img> [qemu-device]}"
DEV="${3:-e1000}"
QEMU="${QEMU:-qemu-system-x86_64}"

TMP="$(mktemp -d)"
LOG="$TMP/serial.log"
PCAP="$TMP/link.pcap"
PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT

# A host peer to fetch from, so the run contains real traffic and not only the
# boot's DHCP: resolution, a gateway binding, and a few thousand frames through
# eth_input's dispatch.
python3 - "$TMP" "$TMP/port" <<'PY' &
import http.server, pathlib, sys
root, pf = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
(root / "probe.bin").write_bytes(bytes((i * 31 + 7) & 0xff for i in range(262144)))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, _f, *_a): pass
srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0),
        lambda *a, **kw: Quiet(*a, directory=str(root), **kw))
pf.write_text(str(srv.server_port), encoding="ascii")
srv.serve_forever()
PY
PIDS+=("$!")
for _ in $(seq 1 100); do [ -s "$TMP/port" ] && break; sleep 0.05; done
[ -s "$TMP/port" ] || { echo "FAIL: host HTTP server did not start"; exit 1; }
PORT="$(cat "$TMP/port")"

# Wait for the guest to be READY rather than sleeping a fixed amount. A fixed
# sleep was flaky here for a reason worth recording: this host runs other
# agents' QEMU, boot time under TCG varies by several seconds, and commands
# typed before /bin/sh exists are swallowed by the PS/2-less serial console --
# which presents as "the run carried no traffic", i.e. a link-layer failure,
# from a harness bug. The marker is DHCP completing, which is the last thing
# net_init does and the point after which `net get` can work at all.
wait_for() {                       # wait_for <pattern> <deciseconds>
    for _ in $(seq 1 "$2"); do
        grep -aq "$1" "$LOG" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

{ wait_for '\[dhcp\] bound\|dhcp failed' 900 || true
  sleep 2
  printf 'net info\n';  sleep 2
  printf 'net ping\n';  sleep 6
  printf 'net get http://10.0.2.2:%s/probe.bin\n' "$PORT"
  sleep 25; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 2 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device "$DEV",netdev=n0 \
    -object filter-dump,id=f0,netdev=n0,file="$PCAP" \
    -serial stdio -display none -no-reboot >"$LOG" 2>"$TMP/qemu.err" &
QPID=$!; PIDS+=("$QPID")

for _ in $(seq 1 700); do
    grep -aq 'http bytes ' "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 1
kill "$QPID" 2>/dev/null
sleep 0.5

fail() {
    echo "FAIL[$DEV]: $1"
    echo "----- serial tail -----"; tail -30 "$LOG"; echo "-----------------------"
    exit 1
}

[ -s "$PCAP" ] || fail "no pcap was written"

# Diagnostics from the guest, printed but not asserted on.
grep -a '^\[eth\]' "$LOG" | sed 's/^/  /'
grep -a '^\[arp\]' "$LOG" | sed 's/^/  /'

python3 - "$PCAP" "$LOG" <<'PY'
import struct, sys, collections

pcap, log = sys.argv[1], sys.argv[2]
data = open(pcap, "rb").read()
if len(data) < 24:
    print("FAIL: pcap is empty"); sys.exit(1)
magic = data[:4]
if magic == b"\xd4\xc3\xb2\xa1":   endian, nano = "<", False
elif magic == b"\xa1\xb2\xc3\xd4": endian, nano = ">", False
elif magic == b"\x4d\x3c\xb2\xa1": endian, nano = "<", True
elif magic == b"\xa1\xb2\x3c\x4d": endian, nano = ">", True
else:
    print("FAIL: unrecognised pcap magic %r" % magic); sys.exit(1)

frames, off = [], 24
while off + 16 <= len(data):
    ts, tus, caplen, origlen = struct.unpack(endian + "IIII", data[off:off+16])
    off += 16
    if caplen > len(data) - off: break
    frames.append((origlen, data[off:off+caplen]))
    off += caplen

if len(frames) < 50:
    print("FAIL: only %d frames captured; the run did not carry traffic." % len(frames))
    print("      Check the serial tail below: if the guest never reached a shell")
    print("      prompt this is a slow boot on a loaded host, not a link fault.")
    sys.exit(1)

# The guest's MAC, taken from the boot log rather than assumed, so this works
# whatever QEMU hands the card.
our = None
for line in open(log, errors="replace"):
    if "NIC bound:" in line and "MAC" in line:
        tok = line.split("MAC")[1].split()[0].strip()
        parts = tok.split(":")
        if len(parts) == 6:
            try: our = bytes(int(p, 16) for p in parts)
            except ValueError: pass
        break
if our is None:
    print("FAIL: could not read the guest MAC from the boot log"); sys.exit(1)
print("  guest MAC %s, %d frames captured" % (":".join("%02x" % b for b in our), len(frames)))

tx = [(n, f) for (n, f) in frames if len(f) >= 12 and f[6:12] == our]
rx = [(n, f) for (n, f) in frames if len(f) >= 12 and f[6:12] != our]
print("  %d frames transmitted by the guest, %d inbound" % (len(tx), len(rx)))
if len(tx) < 20:
    print("FAIL: the guest transmitted almost nothing (%d frames)" % len(tx)); sys.exit(1)

failed = []

# --- 1. the 60-byte minimum ---------------------------------------------
runts = [(n, f[12:14].hex()) for (n, f) in tx if n < 60]
if runts:
    kinds = collections.Counter(k for _, k in runts)
    failed.append("%d transmitted frames were shorter than the 60-byte Ethernet "
                  "minimum (shortest %d bytes; ethertypes %s). eth_send is not "
                  "padding." % (len(runts), min(n for n, _ in runts), dict(kinds)))
else:
    print("  PASS: all %d transmitted frames are >= 60 bytes (shortest %d)"
          % (len(tx), min(n for n, _ in tx)))

# --- 2. the RFC 5227 announcement ---------------------------------------
def arp(f):
    if len(f) < 42 or f[12:14] != b"\x08\x06": return None
    a = f[14:42]
    if a[0:2] != b"\x00\x01" or a[2:4] != b"\x08\x00" or a[4] != 6 or a[5] != 4:
        return None
    return dict(op=int.from_bytes(a[6:8], "big"), sha=a[8:14],
                spa=a[14:18], tha=a[18:24], tpa=a[24:28])

tx_arp = [a for a in (arp(f) for _, f in tx) if a]
rx_arp = [a for a in (arp(f) for _, f in rx) if a]
announcements = [a for a in tx_arp if a["op"] == 1 and a["spa"] == a["tpa"]
                 and a["spa"] != b"\x00\x00\x00\x00"]
if not announcements:
    failed.append("the guest never announced its address: no broadcast ARP "
                  "request with sender == target was transmitted (RFC 5227 s3). "
                  "%d ARP frames were sent in total." % len(tx_arp))
else:
    ip = ".".join(str(b) for b in announcements[0]["spa"])
    print("  PASS: address %s announced (%d announcement(s))" % (ip, len(announcements)))

# --- 3. resolution is paced ---------------------------------------------
requests = [a for a in tx_arp if a["op"] == 1 and a["spa"] != a["tpa"]]
per_target = collections.Counter(a["tpa"] for a in requests)
print("  ARP: %d requests sent, %d replies sent, %d ARP frames received"
      % (len(requests), len([a for a in tx_arp if a["op"] == 2]), len(rx_arp)))
LIMIT = 12
for tpa, n in per_target.items():
    if n > LIMIT:
        failed.append("%d ARP requests for %s in one run (limit %d). Resolution "
                      "is not being paced by the state machine -- this is the "
                      "broadcast-per-call behaviour."
                      % (n, ".".join(str(b) for b in tpa), LIMIT))
if per_target and all(n <= LIMIT for n in per_target.values()):
    print("  PASS: at most %d requests per target (limit %d, %d targets)"
          % (max(per_target.values()), LIMIT, len(per_target)))

# --- the fetch still worked ----------------------------------------------
body = open(log, errors="replace").read()
if "http bytes " not in body:
    failed.append("the HTTP fetch never completed: the link layer is not "
                  "carrying traffic")
else:
    print("  PASS: %s" % [l.strip() for l in body.splitlines()
                          if "http bytes " in l][-1])

if failed:
    for f in failed: print("FAIL: " + f)
    sys.exit(1)
print("PASS: link layer verified on the wire")
PY
rc=$?
[ $rc -eq 0 ] || fail "wire assertions failed (see above)"
echo "PASS[$DEV]: link layer"
