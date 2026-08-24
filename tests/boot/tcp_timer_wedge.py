#!/usr/bin/env python3
"""Does TCP still keep time when the window manager does not?

THE PROPERTY, AND WHY IT NEEDS A WEDGE TO SEE AT ALL
====================================================
Inbound segments have been off the compositor since the receive path moved to
SOFTIRQ_NET, so a SYN and a request arrive whether or not the window manager
runs.  What did NOT move was tcp_poll() -- the retransmission timeout, the
delayed-ACK flush, the zero-window persist probe, TIME_WAIT reaping and the
drain that pushes queued response bytes when the peer's window reopens.  Its
only steady-state caller was net_poll(), and net_poll()'s only steady-state
caller was the line `if (!g_net_busy) net_poll();` in c/kernel/gui/wm.c, once
per composited frame.  include/abi/logit_abi.h states the consequence in its own
words: on a machine whose WM is wedged "a server accepts connections and answers
them slowly or not at all".

A LOSSLESS FETCH CANNOT SEE THIS, which is the first thing to understand about
this harness and the reason it is not simply "wedge the WM and fetch a file".
c/net/core/lsock.c's write path calls tcp_send_nb(), which calls tcp_output()
itself, and every further push is driven by the peer's ACK arriving on the
receive softirq.  Over QEMU's SLIRP -- which drops nothing -- a whole 35 KB
response is delivered without tcp_poll() ever running.  So the wedge must be
paired with something only a TIMER can recover from.

THE CUT.  QMP `set_link ... up=false` drops every frame in both directions for
CUT_S seconds while the response is in flight.  Nothing gets through, so no
duplicate ACKs come back and fast retransmit cannot fire: the retransmission
timeout in tcp_poll() is the ONLY thing that can restart the flow.  That makes
the measurement unambiguous -- it is not "slower", it is "never".

THREE ROUNDS, ONE BOOT, and the third is the negative control
=============================================================
  R1  WM live, timers on the ktimer     -- the apparatus check.  If this does
                                           not recover, nothing below means
                                           anything.
  R2  WM wedged, timers on the ktimer   -- THE FINDING.
  R3  WM wedged, timers forced back onto net_poll (`netwedge <ms> wm`)
                                        -- the pre-change wiring, at runtime,
                                           which MUST stall.

The control is in-run rather than a second target for two reasons: a second
boot doubles a three-minute device test, and -- the one that matters -- it
would not share R1's apparatus check, so a control that "failed" because the
link cut silently stopped working would read as a pass.

WHAT IS BEING WEDGED, SAID PLAINLY.  `echo netwedge <ms> > /dev/ktrigger`
parks net_poll() in c/net/core/net.c: it returns at once for <ms>.  It is not a
sleep inside wm.c because that file belongs to another line right now, and it
does not need to be: the WM's entire contribution to the network is that one
call, so removing the call is what a wedged WM looks like from the network's
side.  The timer, the softirq and tcp_poll() itself run untouched while it is
armed -- what stops is the caller this change exists to make unnecessary.
"""

import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

SERVE = "/licenses/GPL-3.0-or-later.txt"
LOCAL = "LICENSES/GPL-3.0-or-later.txt"

CUT_S = 1.5        # link down for this long, with the response in flight
PRIME = 4096       # bytes the host must have seen before the cut -- the proof
                   # the response was really flowing when the wire went away
PARK_MS = 60000    # the wedge window; MUST outlast the longest round below, or
                   # the park expires mid-round, net_poll comes back and R3
                   # recovers for the wrong reason
STALL_S = 20.0     # how long R3 is watched failing before the park is released
# RECOVER_S IS 30 BECAUSE 10 WAS MEASURED TO BE MARGINAL, and the distinction
# between what was measured and what is merely believed matters here.
#
# MEASURED, on this host on 2026-08-21, shipped kernel: R1 and R2 recover in
# 1.59-1.60 s, run after run -- and in one run of four, R2 did not complete
# within a 10 s budget at all (14,032 of 35,149 bytes) even though the park had
# reported 1,204 ktimer fires and 1,202 softirq passes. So the timer was
# demonstrably running and the TRANSFER was slow, which is not the same
# finding, and a 10 s threshold turns the difference into a coin flip.
#
# BELIEVED, and written down as a hypothesis rather than a fact because the
# longer budget has not reproduced the slow case to confirm it: the recovery
# time is dominated by RFC 6298 backoff accumulated DURING the cut. RTO_MIN is
# 100 ms and every timeout doubles, so 1.5 s of dead wire is four doublings
# (0.1/0.3/0.7/1.5 s cumulative) and the next attempt after the wire returns is
# up to 1.6 s out -- which is exactly the 1.60 s that keeps being reported. If
# that first attempt is ALSO lost, the next is 3.2 s out, then 6.4, then 12.8,
# and 12.8 > 10.
#
# CONFIRMED STABLE AT 30: four consecutive runs of the shipped kernel after the
# change, R1 1.59 s and R2 1.60 s in every one of them, R3 `never` in every one
# of them (8,192 / 14,032 / 15,492 / 8,192 of 35,149 body bytes). Two runs of a
# -DTCP_TIMERS_ON_WM kernel through the same harness failed at R2, both times.
#
# Either way the threshold is not the measurement: R3 is watched failing for
# 20 s against an R2 that recovers in 1.6 s, so the gate has more than an order
# of magnitude of separation and does not turn on where in that range the
# budget sits.
RECOVER_S = 30.0


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Serial:
    """The guest's console: a log we grep and a keyboard we type on."""

    def __init__(self, proc, path):
        self.proc = proc
        self.buf = ""
        self.lock = threading.Lock()
        self.fh = open(path, "w", encoding="latin-1")
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while True:
            chunk = self.proc.stdout.read(1)
            if not chunk:
                return
            with self.lock:
                self.buf += chunk.decode("latin-1", "replace")
            self.fh.write(chunk.decode("latin-1", "replace"))
            self.fh.flush()

    def mark(self):
        with self.lock:
            return len(self.buf)

    def since(self, at):
        with self.lock:
            return self.buf[at:]

    def wait(self, needle, timeout, at=0):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.since(at):
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def type(self, line):
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()


class Qmp:
    def __init__(self, port):
        end = time.time() + 30
        self.s = None
        while time.time() < end:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                time.sleep(0.2)
        if self.s is None:
            raise RuntimeError("no QMP connection")
        self.f = self.s.makefile("rwb")
        self._read()                       # the greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP closed")
            msg = json.loads(line.decode())
            if "event" in msg:
                continue
            return msg

    def cmd(self, verb, **args):
        # `verb`, not `name`: set_link's own argument is called `name`, and a
        # positional called `name` collides with it in **args.
        req = {"execute": verb}
        if args:
            req["arguments"] = args
        self.f.write((json.dumps(req) + "\n").encode())
        self.f.flush()
        return self._read()

    def link(self, name, up):
        r = self.cmd("set_link", name=name, up=up)
        if "error" in r:
            raise RuntimeError("set_link %s up=%s: %s" % (name, up, r["error"]))


def fetch_with_cut(port, qmp, nic, want, ser, recover_s, tag):
    """One HTTP/1.0 fetch, with the wire cut mid-response.

    Returns (ok, t_recover, note).  t_recover is measured from the link coming
    back UP to the last byte of the body -- deliberately not from the first
    byte, because SLIRP terminates the host's connection and has its own
    buffered bytes to hand over that say nothing about the guest."""
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    s.sendall(("GET %s HTTP/1.0\r\nHost: logitos\r\n\r\n" % SERVE).encode())

    buf = b""
    s.settimeout(30)
    t0 = time.time()
    while len(buf) < PRIME:
        if time.time() - t0 > 30:
            s.close()
            return False, None, "no first bytes within 30 s (before the cut)"
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    if len(buf) < PRIME:
        s.close()
        return False, None, "only %d bytes before the cut, wanted >= %d" % (len(buf), PRIME)

    at = ser.mark()
    qmp.link(nic, False)
    time.sleep(CUT_S)
    qmp.link(nic, True)
    t_up = time.time()

    # THE APPARATUS CHECK IS READ AFTER THE LOOP, NOT HERE, and that ordering
    # was a real defect: waiting up to 5 s for the guest to print `link: DOWN`
    # between t_up and the first recv() spent the recovery budget being
    # measured. The line is in the serial buffer either way, so it costs
    # nothing to look at it once the timing is over.
    s.settimeout(0.25)
    deadline = t_up + recover_s
    t_done = None
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            t_done = time.time()
            break
        buf += chunk
    s.close()

    # The guest's own driver has to have SEEN the cut. A set_link that silently
    # did nothing would make R3 "pass" by never needing a retransmit at all.
    saw_down = "[e1000] link: DOWN" in ser.since(at)
    head, _, body = buf.partition(b"\r\n\r\n")
    ok = t_done is not None and body == want
    note = "link DOWN seen by the guest: %s; %d body bytes of %d" % (
        saw_down, len(body), len(want))
    if not saw_down:
        note = "APPARATUS: " + note
    return ok, (t_done - t_up) if t_done else None, note


def main():
    if len(sys.argv) < 3:
        print("usage: tcp_timer_wedge.py <iso> <disk.img>")
        return 2
    iso, disk = sys.argv[1], sys.argv[2]
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    local = os.path.join(root, LOCAL)
    if not os.path.exists(local):
        print("FAIL: %s missing (nothing to compare against)" % local)
        return 1
    want = open(local, "rb").read()

    hostport = free_port()
    qmpport = free_port()
    logdir = os.path.join(root, "build", "tcp-timer")
    os.makedirs(logdir, exist_ok=True)
    logpath = os.path.join(logdir, "serial.log")

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    cmd = [
        qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", iso,
        "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
        "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
        "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
        "-vga", "none", "-device", "virtio-gpu-pci",
        "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-10.0.2.15:8080" % hostport,
        "-device", "e1000,netdev=n0,id=nic0",
        "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % qmpport,
        "-serial", "stdio", "-display", "none", "-no-reboot",
    ]
    print("=== TCP timers off the WM loop -- the wedged-WM gate ===")
    print("  host port %d -> guest 10.0.2.15:8080 ; QMP on %d" % (hostport, qmpport))
    print("  serial log: %s" % logpath)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, bufsize=0)
    ser = Serial(proc, logpath)
    fails = []
    results = {}
    counters = {}
    try:
        if not ser.wait("LogitOS shell", 240):
            print("FAIL: no shell on the serial console")
            print(ser.since(0)[-3000:])
            return 1
        time.sleep(2.0)
        # Backgrounded, because the shell has to stay usable: the wedge is
        # armed by writing to /dev/ktrigger from this same console.
        ser.type("httpd 8080 / 12 &")
        if not ser.wait("HTTPD_READY", 120):
            print("FAIL: httpd never listened")
            print(ser.since(0)[-3000:])
            return 1
        print("  " + [l for l in ser.since(0).splitlines() if "HTTPD_READY" in l][-1])
        wiring = [l for l in ser.since(0).splitlines() if "tcp timers:" in l]
        print("  " + (wiring[-1] if wiring else "NO [net] tcp timers LINE -- old kernel?"))
        if not wiring:
            fails.append("the kernel printed no `[net] tcp timers:` line at all")

        qmp = Qmp(qmpport)

        for tag, park, on_wm, budget in (
                ("R1 wm-live      ", 0,       False, RECOVER_S),
                ("R2 wm-wedged    ", PARK_MS, False, RECOVER_S),
                ("R3 wm-wedged+old", PARK_MS, True,  STALL_S)):
            at = ser.mark()
            if park:
                ser.type("echo netwedge %d %s > /dev/ktrigger"
                         % (park, "wm" if on_wm else ""))
                if not ser.wait("[net] park: net_poll parked", 20, at):
                    fails.append("%s: the park never armed" % tag.strip())
                    continue
            ok, t, note = fetch_with_cut(hostport, qmp, "n0", want, ser, budget, tag)
            results[tag.strip()] = (ok, t, note)
            print("  %s  complete=%-5s t_recover=%s  (%s)"
                  % (tag, ok, ("%.2fs" % t) if t is not None else "never", note))
            if park:
                # Release: `netwedge 0` also puts the timers back on the ktimer,
                # so R3's connection finishes and httpd is free for the next
                # round -- which is itself the proof that the stall was the
                # timers and not a connection that had died.
                at2 = ser.mark()
                ser.type("echo netwedge 0 > /dev/ktrigger")
                ser.wait("[net] park: released", 10, at2)
                time.sleep(3.0)
                summ = [l for l in ser.since(at).splitlines()
                        if "[net] park:" in l and "tcp timer fires" in l]
                if not summ:
                    fails.append("%s: the park ended without a report -- no counters"
                                 % tag.strip())
                else:
                    print("       %s" % summ[-1].strip())
                    m = re.search(r"refused, tcp timer fires \+(\d+), softirq passes \+(\d+)",
                                  summ[-1])
                    fires, passes = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
                    counters[tag.strip()] = (fires, passes)

        rx = [l for l in ser.since(0).splitlines() if "rx path:" in l]
        if rx:
            print("  " + rx[-1].strip())
    finally:
        try:
            proc.kill()
            proc.wait(10)
        except Exception:                     # noqa: BLE001 -- teardown only
            pass

    r1 = results.get("R1 wm-live", (False, None, ""))
    r2 = results.get("R2 wm-wedged", (False, None, ""))
    r3 = results.get("R3 wm-wedged+old", (False, None, ""))

    if not r1[0]:
        print("FAIL (APPARATUS): the control round did not even recover with the "
              "WM running -- nothing below this line means anything")
        return 1
    if not r2[0]:
        fails.append("R2: with the WM wedged the response never recovered from a "
                     "%.1f s wire cut -- the timers are still on the WM loop" % CUT_S)
    if r3[0]:
        fails.append("NEGATIVE CONTROL FAILED: R3 put TCP's timers back on net_poll "
                     "with the WM wedged and the response recovered anyway (%s). "
                     "Something other than tcp_poll is retransmitting, so R2 proves "
                     "nothing." % (("%.2fs" % r3[1]) if r3[1] else "?"))

    # THE INSTRUMENT'S OWN READOUT, asserted rather than printed. R2 recovering
    # is the outcome; these two numbers are the mechanism, and they separate
    # "the timer ran the pass" from "the transfer completed for some other
    # reason we have not thought of".
    f2, p2 = counters.get("R2 wm-wedged", (-1, -1))
    f3, p3 = counters.get("R3 wm-wedged+old", (-1, -1))
    if f2 < 100 or p2 < 100:
        fails.append("R2: the park reported %d ktimer fires / %d softirq passes; "
                     "at 10 ms both should be in the hundreds, so the recovery "
                     "was not this timer's doing" % (f2, p2))
    if f3 != 0:
        fails.append("R3: the park reported %d ktimer fires with the timers "
                     "forced onto the WM loop -- the control did not actually "
                     "restore the old wiring" % f3)

    print()
    if fails:
        print("TCP_TIMER_WEDGE_FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("PASS: with net_poll parked for %d ms, a %.1f s wire cut mid-response "
          "recovered in %.2f s (R2)." % (PARK_MS, CUT_S, r2[1]))
    print("      The same wedge with TCP's timers forced back onto net_poll did "
          "not recover in %.1f s (R3), which is the wiring this change replaced."
          % STALL_S)
    print("      Control with the WM running: %.2f s (R1)." % r1[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
