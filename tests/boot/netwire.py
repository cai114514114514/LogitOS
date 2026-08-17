#!/usr/bin/env python3
"""netwire -- an instrumented Ethernet wire between two QEMU socket netdevs.

WHY THIS EXISTS
===============
The previous networking line rebuilt TCP (send ring, Reno, window scaling,
SACK, timestamps) and measured no throughput change, then worked out why:

    QEMU user networking (SLIRP) is not a wire.  It is a host-side TCP/IP
    stack that TERMINATES the guest's connection.  The guest's TCP peer is
    SLIRP itself, one emulated hop away at sub-millisecond RTT.  There is no
    bandwidth-delay product, so a window is never the limit and no amount of
    congestion control can show up in a number.

They then tried to inject delay UPSTREAM of SLIRP (a slow HTTP server, a
relaying proxy) and it did nothing -- correctly, because that delay is between
SLIRP and the origin, on a connection the guest is not a party to.

The fix is to put the delay on the segment where the guest's own TCP lives,
i.e. BETWEEN THE GUEST AND ITS PEER.  That needs the guest's frames to travel
over something we control before they reach any stack.  Topology:

    QEMU-A (LogitOS)                netwire.py                QEMU-B
    -netdev socket,connect=:P1  <-->  delay/loss/  <-->  -netdev socket,listen=:P2
                                      instrument         -netdev user  (SLIRP)
                                                          joined on hub 0
                                                                |
                                                          host HTTP server

QEMU-B runs no guest at all (`-nodefaults -S`): a QEMU hub with a socket port
and a SLIRP port forwards frames between them from the main loop, with the
vCPU stopped.  So SLIRP -- a real BSD-derived TCP stack that does window
scaling, delayed ACK and congestion control -- becomes the guest's peer at
whatever RTT we choose, and the guest's own window and cwnd are on the
critical path for the first time.

Needs no root, no TAP, no `tc`.

WHAT IT MEASURES
================
The wire sees every frame, so the measurement is in-band and does not depend
on serial-log polling or on the guest owning a clock:

  * goodput per TCP flow, from the first payload byte to the last, over
    DISTINCT sequence space (a retransmit is not counted twice);
  * handshake RTT (SYN -> SYN/ACK) as the guest actually experiences it;
  * retransmissions, by counting payload segments whose sequence range was
    already delivered;
  * out-of-order arrivals and pure/dup ACKs;
  * frame and byte totals in each direction, which is what a driver
    comparison is actually about.

WHAT IT IS NOT
==============
Every number is a TCG number.  The guest is fully emulated, so the host CPU
executing translated x86 is a bottleneck in a way no real machine has.  The
absolute rates mean nothing as "LogitOS does N Mbit/s"; what they support is a
comparison between two builds, or two drivers, on the same host with the same
topology -- which is stated with every number this prints.

The wire itself has a ceiling.  --selftest measures it (it forwards synthetic
frames and reports frames/s and Mbit/s with no QEMU attached), and every
benchmark run should check the reported rate is well under it before believing
a throughput difference.

PROTOCOL
========
QEMU's socket netdev in TCP mode frames each Ethernet packet as a 4-byte
big-endian length followed by the raw frame.  That is the entire protocol.

Usage:
    netwire.py --listen 127.0.0.1:P1 --connect 127.0.0.1:P2 \
               [--delay-ms N] [--loss-pct F] [--rate-mbit R] \
               [--report FILE] [--duration S]
    netwire.py --selftest
"""

import argparse
import heapq
import json
import random
import select
import signal
import socket
import struct
import sys
import time

ETH_HDR = 14
ETHERTYPE_IP = 0x0800
ETHERTYPE_ARP = 0x0806


# --------------------------------------------------------------------------
# TCP flow accounting
# --------------------------------------------------------------------------
class Flow:
    """One direction of one TCP connection, tracked by sequence space.

    Counting bytes on the wire would over-count retransmissions, which is
    exactly the thing worth knowing separately.  So payload is credited to
    `delivered` only for sequence ranges not seen before; a segment that adds
    nothing new is a retransmit.
    """

    __slots__ = ("key", "first_ts", "last_ts", "delivered", "wire_bytes",
                 "segments", "retrans", "ooo", "acks", "dup_acks",
                 "_hi", "_seen", "_last_ack", "syn_ts", "synack_ts", "fin_ts",
                 "max_win", "wscale")

    def __init__(self, key):
        self.key = key
        self.first_ts = None
        self.last_ts = None
        self.delivered = 0      # distinct payload bytes
        self.wire_bytes = 0     # payload bytes incl. retransmits
        self.segments = 0
        self.retrans = 0
        self.ooo = 0
        self.acks = 0
        self.dup_acks = 0
        self._hi = None         # highest contiguous seq seen
        self._seen = []         # [(start, end)] out-of-order ranges
        self._last_ack = None
        self.syn_ts = None
        self.synack_ts = None
        self.fin_ts = None
        self.max_win = 0
        self.wscale = None

    def payload(self, ts, seq, n):
        self.segments += 1
        self.wire_bytes += n
        if self._hi is None:
            self._hi = seq
        # 32-bit sequence arithmetic
        off = (seq - self._hi) & 0xFFFFFFFF
        if off > 0x7FFFFFFF:
            off -= 0x100000000
        new = 0
        if off == 0:
            new = n
            self._hi = (self._hi + n) & 0xFFFFFFFF
            # absorb any queued out-of-order ranges now contiguous
            changed = True
            while changed:
                changed = False
                for r in list(self._seen):
                    d = (r[0] - self._hi) & 0xFFFFFFFF
                    if d > 0x7FFFFFFF:
                        d -= 0x100000000
                    if d <= 0:
                        end_off = (r[1] - self._hi) & 0xFFFFFFFF
                        if end_off > 0x7FFFFFFF:
                            end_off -= 0x100000000
                        if end_off > 0:
                            new += end_off
                            self._hi = r[1]
                        self._seen.remove(r)
                        changed = True
        elif off > 0:
            self.ooo += 1
            self._seen.append((seq, (seq + n) & 0xFFFFFFFF))
        else:
            self.retrans += 1
        if new:
            self.delivered += new
            if self.first_ts is None:
                self.first_ts = ts
            self.last_ts = ts

    def ack(self, ts, ack):
        self.acks += 1
        if self._last_ack == ack:
            self.dup_acks += 1
        self._last_ack = ack

    def summary(self):
        dt = (self.last_ts - self.first_ts) if (self.first_ts and self.last_ts
                                                and self.last_ts > self.first_ts) else 0.0
        return {
            "flow": self.key,
            "bytes": self.delivered,
            "wire_bytes": self.wire_bytes,
            "seconds": round(dt, 6),
            "mbit_s": round(self.delivered * 8.0 / dt / 1e6, 3) if dt > 0 else None,
            "segments": self.segments,
            "retrans": self.retrans,
            "ooo": self.ooo,
            "acks": self.acks,
            "dup_acks": self.dup_acks,
            "max_win": self.max_win,
            "wscale": self.wscale,
            "handshake_rtt_ms": (round((self.synack_ts - self.syn_ts) * 1e3, 3)
                                 if self.syn_ts and self.synack_ts else None),
        }


class Turnaround:
    """How long the guest takes to REACT to a packet -- the latency number the
    receive path actually controls.

    A remote peer's think time dominates any wall-clock handshake measurement
    (traced against cloudflare through WSL's NAT: 170 ms between our
    ClientHello leaving and the server's flight arriving, against a 1.5 ms host
    ping). No change inside the guest touches that. What the guest owns is the
    interval between a segment ARRIVING and the guest EMITTING the
    acknowledgement of it -- notice, drain, protocol, transmit.

    That interval is exactly the quantity a polled receive path inflates: if
    nothing looks at the NIC until the next timer tick, the reaction cannot be
    faster than the tick. So it is measured per inbound data segment: the first
    outbound packet whose ack covers the segment's last byte, minus the moment
    that segment crossed the wire. Pure ACKs the guest sends for its own
    reasons cannot be credited, because a segment is only matched once.
    """

    def __init__(self):
        self.pending = {}      # conn key -> [(end_seq, ts)]
        self.samples = []

    @staticmethod
    def _conn(a, b):
        return tuple(sorted((a, b)))

    def inbound(self, key_a, key_b, ts, seq, n):
        self.pending.setdefault(self._conn(key_a, key_b), []).append(
            ((seq + n) & 0xFFFFFFFF, ts))

    def outbound(self, key_a, key_b, ts, ack, has_ack):
        if not has_ack:
            return
        q = self.pending.get(self._conn(key_a, key_b))
        if not q:
            return
        keep = []
        for end, t in q:
            d = (ack - end) & 0xFFFFFFFF
            covered = d < 0x80000000        # ack >= end, modulo 2^32
            if covered:
                self.samples.append((ts - t) * 1e3)
            else:
                keep.append((end, t))
        self.pending[self._conn(key_a, key_b)] = keep


class Instrument:
    def __init__(self, trace=None):
        # An optional per-packet timeline. The aggregate numbers answer "how
        # fast"; this answers "who was everyone waiting for", which is the
        # question a latency measurement actually turns on -- a gap between our
        # last send and their first reply is the network, and a gap between
        # their last send and our next one is us.
        self.trace = trace
        self.t0 = None
        self.turn = Turnaround()
        self.flows = {}
        self.frames = [0, 0]     # [guest->up, up->guest]
        self.octets = [0, 0]
        self.arp = 0
        self.other = 0
        self.pending_syn = {}    # key -> ts, to pair SYN with SYN/ACK

    def flow(self, key):
        f = self.flows.get(key)
        if f is None:
            f = Flow(key)
            self.flows[key] = f
        return f

    def observe(self, direction, frame, ts):
        self.frames[direction] += 1
        self.octets[direction] += len(frame)
        if len(frame) < ETH_HDR:
            return
        et = struct.unpack_from("!H", frame, 12)[0]
        if et == ETHERTYPE_ARP:
            self.arp += 1
            return
        if et != ETHERTYPE_IP:
            self.other += 1
            return
        ip = frame[ETH_HDR:]
        if len(ip) < 20:
            return
        ihl = (ip[0] & 0x0F) * 4
        if ip[9] != 6 or len(ip) < ihl + 20:      # not TCP
            return
        total = struct.unpack_from("!H", ip, 2)[0]
        tcp = ip[ihl:total] if total <= len(ip) else ip[ihl:]
        if len(tcp) < 20:
            return
        sport, dport = struct.unpack_from("!HH", tcp, 0)
        seq, ack = struct.unpack_from("!II", tcp, 4)
        doff = (tcp[12] >> 4) * 4
        flags = tcp[13]
        win = struct.unpack_from("!H", tcp, 14)[0]
        src = ".".join(str(b) for b in ip[12:16])
        dst = ".".join(str(b) for b in ip[16:20])
        key = "%s:%d>%s:%d" % (src, sport, dst, dport)
        rkey = "%s:%d>%s:%d" % (dst, dport, src, sport)
        f = self.flow(key)
        if win > f.max_win:
            f.max_win = win

        if flags & 0x02:                           # SYN
            if flags & 0x10:                       # SYN/ACK -> pair with the SYN
                peer = self.flows.get(rkey)
                if peer is not None and peer.syn_ts and peer.synack_ts is None:
                    peer.synack_ts = ts
            else:
                f.syn_ts = ts
            # window scale option, so the report says what was negotiated
            i, end = 20, min(doff, len(tcp))
            while i + 1 < end:
                kind = tcp[i]
                if kind == 0:
                    break
                if kind == 1:
                    i += 1
                    continue
                ln = tcp[i + 1]
                if ln < 2:
                    break
                if kind == 3 and ln == 3:
                    f.wscale = tcp[i + 2]
                i += ln
        if flags & 0x01:                           # FIN
            f.fin_ts = ts
        n = len(tcp) - doff
        if n > 0:
            f.payload(ts, seq, n)
        elif flags & 0x10:
            f.ack(ts, ack)

        # direction 1 = toward the guest, so an inbound data segment is what the
        # guest has to react to, and an outbound ACK is the reaction.
        if direction == 1 and n > 0:
            self.turn.inbound(key, rkey, ts, seq, n)
        elif direction == 0:
            self.turn.outbound(key, rkey, ts, ack, bool(flags & 0x10))

        if self.trace is not None:
            if self.t0 is None:
                self.t0 = ts
            names = "FSRPAU"
            fl = "".join(names[i] for i in range(6) if flags & (1 << i))
            self.trace.write("%9.3f %s %-21s %-21s %-6s seq=%-10u ack=%-10u len=%-5d win=%u\n"
                             % ((ts - self.t0) * 1e3,
                                "G>N" if direction == 0 else "N>G",
                                "%s:%d" % (src, sport), "%s:%d" % (dst, dport),
                                fl or "-", seq, ack, n, win))

    def report(self):
        # CHRONOLOGICAL, not sorted by size: run-net-ab.sh pairs sample i of one
        # arm with sample i of another, so the order flows appear in is load-
        # bearing. dict preserves insertion order, and a flow is inserted when
        # its first packet crosses the wire.
        flows = [f.summary() for f in self.flows.values() if f.delivered > 0]
        s = sorted(self.turn.samples)
        turn = None
        if s:
            def pct(p):
                return round(s[min(len(s) - 1, int(len(s) * p))], 3)
            turn = {"n": len(s), "median_ms": pct(0.5), "p90_ms": pct(0.9),
                    "p99_ms": pct(0.99), "max_ms": round(s[-1], 3),
                    "min_ms": round(s[0], 3)}
        return {
            "rx_to_ack_turnaround": turn,
            "frames_guest_to_net": self.frames[0],
            "frames_net_to_guest": self.frames[1],
            "octets_guest_to_net": self.octets[0],
            "octets_net_to_guest": self.octets[1],
            "arp_frames": self.arp,
            "flows": flows,
        }


# --------------------------------------------------------------------------
# The wire
# --------------------------------------------------------------------------
class Endpoint:
    """One QEMU socket-netdev connection: length-prefixed Ethernet frames."""

    def __init__(self, sock):
        self.sock = sock
        try:
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass            # AF_UNIX socketpair (--selftest) has no TCP level
        self.rx = bytearray()
        self.tx = bytearray()
        self.closed = False

    def fileno(self):
        return self.sock.fileno()

    def feed(self):
        """Read available bytes; yield complete frames."""
        try:
            data = self.sock.recv(262144)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            self.closed = True
            return
        if not data:
            self.closed = True
            return
        self.rx += data
        while len(self.rx) >= 4:
            n = struct.unpack_from("!I", self.rx, 0)[0]
            if n > 65536:                      # desync; nothing sane is this big
                self.closed = True
                return
            if len(self.rx) < 4 + n:
                return
            frame = bytes(self.rx[4:4 + n])
            del self.rx[:4 + n]
            yield frame

    def queue(self, frame):
        self.tx += struct.pack("!I", len(frame)) + frame

    def flush(self):
        while self.tx:
            try:
                k = self.sock.send(self.tx)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                self.closed = True
                return
            if k <= 0:
                return
            del self.tx[:k]


def parse_addr(s):
    host, _, port = s.rpartition(":")
    return (host or "127.0.0.1", int(port))


def run(args):
    trace_fh = open(args.trace, "w") if args.trace else None
    inst = Instrument(trace_fh)
    rng = random.Random(args.seed)
    delay = args.delay_ms / 1000.0
    # rate limiting is per direction, in bytes/second on the wire
    rate = args.rate_mbit * 1e6 / 8.0 if args.rate_mbit else 0.0

    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(parse_addr(args.listen))
    lsock.listen(1)
    if args.print_port:
        print(lsock.getsockname()[1], flush=True)

    # Upstream (QEMU-B) may not be listening yet.
    up_sock = None
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            up_sock = socket.create_connection(parse_addr(args.connect), timeout=1)
            break
        except OSError:
            time.sleep(0.05)
    if up_sock is None:
        sys.stderr.write("netwire: upstream %s never accepted\n" % args.connect)
        return 1
    up = Endpoint(up_sock)
    up.sock.setblocking(False)

    lsock.settimeout(30)
    try:
        gsock, _ = lsock.accept()
    except OSError:
        sys.stderr.write("netwire: guest never connected\n")
        return 1
    lsock.close()
    guest = Endpoint(gsock)
    guest.sock.setblocking(False)

    # Scheduled deliveries: (due_ts, seq, direction, frame)
    q = []
    counter = 0
    next_free = [0.0, 0.0]          # earliest time each direction may transmit
    started = time.monotonic()
    stop_at = started + args.duration if args.duration else None
    dropped = [0, 0]

    # The harness ends a run by signalling us; the report is the whole point of
    # the process, so both signals mean "stop the loop and write it", not "die".
    stopping = [False]

    def _stop(_sig, _frm):
        stopping[0] = True
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    def emit(direction, frame, now):
        nonlocal counter
        if args.loss_pct and rng.random() * 100.0 < args.loss_pct:
            dropped[direction] += 1
            return
        due = now + delay
        if rate:
            serial = len(frame) / rate
            base = max(due, next_free[direction])
            next_free[direction] = base + serial
            due = base + serial
        counter += 1
        heapq.heappush(q, (due, counter, direction, frame))

    # HOW MUCH OF A MEASURED LATENCY IS THIS PROCESS?
    #
    # The turnaround number above is "inbound frame crossed the wire" to "the
    # guest's ACK crossed the wire", and BOTH timestamps are taken by this
    # single-threaded Python loop.  So a relay that is near saturation inflates
    # exactly the quantity the receive path is being judged on, and the guest
    # gets blamed for the relay's queueing.  Arguing from CPU utilisation is not
    # enough -- measure it.
    #
    # Per loop iteration: how long from the select() return that read the frames
    # to the moment they have been handed to the kernel's socket buffer.  That
    # is this process's entire contribution to any latency it reports.  One
    # append per ITERATION, not per frame, so the hot loop does not pay a list
    # append per packet.
    self_lat = []          # [(seconds, frames_that_iteration)]

    while True:
        now = time.monotonic()
        if stopping[0]:
            break
        if stop_at and now > stop_at:
            break
        if guest.closed or up.closed:
            break
        timeout = 0.05
        if q:
            timeout = max(0.0, min(timeout, q[0][0] - now))
        rlist = [guest, up]
        wlist = [e for e in (guest, up) if e.tx]
        try:
            r, w, _ = select.select(rlist, wlist, [], timeout)
        except (OSError, ValueError):
            break
        except InterruptedError:
            continue
        now = time.monotonic()
        nf = 0
        for e in r:
            direction = 0 if e is guest else 1
            for frame in e.feed():
                inst.observe(direction, frame, now)
                emit(direction, frame, now)
                nf += 1
        while q and q[0][0] <= now:
            _, _, direction, frame = heapq.heappop(q)
            (up if direction == 0 else guest).queue(frame)
        for e in (guest, up):
            if e.tx:
                e.flush()
        if nf:
            self_lat.append((time.monotonic() - now, nf))

    rep = inst.report()
    if self_lat:
        lats = sorted(x[0] * 1e3 for x in self_lat)
        nfr = sum(x[1] for x in self_lat)
        rep["relay_self_latency_ms"] = {
            "iterations": len(lats),
            "frames": nfr,
            "frames_per_iteration": round(nfr / len(lats), 2),
            "median": round(lats[len(lats) // 2], 4),
            "p90": round(lats[int(len(lats) * 0.9)], 4),
            "p99": round(lats[int(len(lats) * 0.99)], 4),
            "max": round(lats[-1], 4),
            "total_s": round(sum(lats) / 1e3, 4),
        }
    rep["config"] = {
        "delay_ms_each_way": args.delay_ms,
        "rtt_ms_added": args.delay_ms * 2,
        "loss_pct": args.loss_pct,
        "rate_mbit": args.rate_mbit,
        "dropped_guest_to_net": dropped[0],
        "dropped_net_to_guest": dropped[1],
        "wall_seconds": round(time.monotonic() - started, 3),
    }
    if trace_fh:
        trace_fh.close()
    out = json.dumps(rep, indent=2)
    if args.report:
        with open(args.report, "w") as fh:
            fh.write(out + "\n")
    else:
        print(out)
    return 0


def selftest():
    """Measure what the wire itself can do, with no QEMU involved.

    A throughput comparison run over this relay is only believable while the
    observed rate is well under this ceiling, so the number is printed with
    every benchmark rather than assumed.
    """
    inst = Instrument(None)
    # a plausible 1514-byte TCP data frame, so the parser cost is included
    eth = bytes(6) + bytes(6) + struct.pack("!H", ETHERTYPE_IP)
    ip = (b"\x45\x00" + struct.pack("!H", 1500) + b"\x00\x00\x40\x00\x40\x06"
          + b"\x00\x00" + bytes([10, 0, 2, 2]) + bytes([10, 0, 2, 15]))
    payload = bytes(1460)
    n = 200000
    t0 = time.monotonic()
    for i in range(n):
        tcp = struct.pack("!HHIIBBHHH", 80, 40000, 1 + i * 1460, 1,
                          0x50, 0x10, 65535, 0, 0)
        inst.observe(1, eth + ip + tcp + payload, t0)
    dt = time.monotonic() - t0
    print("netwire selftest: parse+account %d frames in %.3f s"
          % (n, dt))
    print("  ceiling (accounting only): %.0f frames/s = %.1f Mbit/s at 1514 B"
          % (n / dt, n * 1514 * 8 / dt / 1e6))
    # forwarding ceiling: loopback socket pair through the same framing
    a, b = socket.socketpair()
    ea, eb = Endpoint(a), Endpoint(b)
    ea.sock.setblocking(False)
    eb.sock.setblocking(False)
    frame = eth + ip + struct.pack("!HHIIBBHHH", 80, 40000, 1, 1,
                                   0x50, 0x10, 65535, 0, 0) + payload
    m, moved, t0 = 20000, 0, time.monotonic()
    for _ in range(m):
        eb.queue(frame)
        eb.flush()
        for f in ea.feed():
            moved += 1
    dt = time.monotonic() - t0
    print("  ceiling (framing+socket): %.0f frames/s = %.1f Mbit/s at 1514 B (%d moved)"
          % (m / dt, m * 1514 * 8 / dt / 1e6, moved))
    return _selftest_endtoend(frame)


def _selftest_endtoend(frame):
    """The ceiling of the loop that is ACTUALLY USED, measured end to end.

    WHY THIS STAGE EXISTS, and it overturned a conclusion.  The two stages above
    time `observe()` and `queue/flush` in SEPARATE loops, so the natural way to
    combine them is the series formula 1/(1/A + 1/F) -- and a benchmark run was
    read against exactly that, concluding the wire could carry ~1124 Mbit/s and
    therefore that a measured ~270-300 Mbit/s had to be the guest's fault.

    But the production loop (see `run`) is not observe+queue.  Per frame it also
    pays a `select()` round trip, a `heapq` push and pop through the delay
    queue, a `len()`-prefixed copy into a bytearray and a `del` off the front of
    another, and it does all of it inside one single-threaded Python process
    that must ALSO be scheduled against two QEMUs on the same host.  None of
    that is in A or F, so the series figure is not an upper bound on this loop;
    it is an upper bound on a loop nobody runs.

    So: spawn the real script, the way the harness spawns it, and push frames
    through it net->guest -- the direction a body travels.  This number is the
    one a measured goodput should be compared against.
    """
    import os
    import subprocess
    import tempfile
    import threading

    def free_port():
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        p = s.getsockname()[1]
        s.close()
        return p

    p1, p2 = free_port(), free_port()

    # Upstream: what QEMU-B's socket netdev is, i.e. netwire's --connect peer.
    up_l = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    up_l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    up_l.bind(("127.0.0.1", p2))
    up_l.listen(1)

    n = 20000                      # ~30 MB, well past any one-fetch body
    rep = os.path.join(tempfile.mkdtemp(), "r.json")
    proc = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__),
         "--listen", "127.0.0.1:%d" % p1, "--connect", "127.0.0.1:%d" % p2,
         "--duration", "60", "--report", rep],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    up_c = None
    try:
        up_l.settimeout(30)
        up_c, _ = up_l.accept()
        up_l.close()

        # Guest side: connect to netwire's listen port and drain.
        g = socket.create_connection(("127.0.0.1", p1), timeout=30)

        got = [0]
        done = threading.Event()
        # Count what is actually SENT, not `n` -- the blast is in blocks of 64,
        # so a `want` of n frames is never reached and `done.wait` runs to its
        # full timeout, which silently reports the timeout as the transfer time.
        sent_frames = (n // 64) * 64
        want = sent_frames * (len(frame) + 4)

        def drain():
            while got[0] < want:
                try:
                    b = g.recv(1 << 20)
                except OSError:
                    break
                if not b:
                    break
                got[0] += len(b)
            done.set()

        th = threading.Thread(target=drain, daemon=True)
        th.start()

        blob = (struct.pack("!I", len(frame)) + frame) * 64
        t0 = time.monotonic()
        for _ in range(n // 64):
            up_c.sendall(blob)
        ok = done.wait(120)
        dt = time.monotonic() - t0
        moved = got[0] / (len(frame) + 4)
        if not ok:
            print("  ceiling (REAL LOOP, end to end): TIMED OUT after %.1f s "
                  "with %d of %d frames -- number below is a floor, not a ceiling"
                  % (dt, moved, sent_frames))
        if dt > 0 and moved > 0:
            print("  ceiling (REAL LOOP, end to end): %.0f frames/s = %.1f Mbit/s "
                  "at %d B (%d of %d frames in %.3f s)"
                  % (moved / dt, moved * len(frame) * 8 / dt / 1e6,
                     len(frame), moved, sent_frames, dt))
            print("  ^ compare a measured goodput against THIS, not against the "
                  "series of the two above")
        else:
            print("  ceiling (REAL LOOP, end to end): FAILED to move frames")
            return 1
    finally:
        try:
            if up_c is not None:
                up_c.close()
        except OSError:
            pass
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--listen", default="127.0.0.1:0")
    p.add_argument("--connect", default="127.0.0.1:0")
    p.add_argument("--delay-ms", type=float, default=0.0,
                   help="one-way delay each direction; RTT added is twice this")
    p.add_argument("--loss-pct", type=float, default=0.0)
    p.add_argument("--rate-mbit", type=float, default=0.0,
                   help="0 = no shaping")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--report", default=None)
    p.add_argument("--duration", type=float, default=0.0)
    p.add_argument("--print-port", action="store_true")
    p.add_argument("--trace", default=None,
                   help="write a per-packet timeline here (ms since first packet)")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args()
    if args.selftest:
        return selftest()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
