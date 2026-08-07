#!/usr/bin/env python3
"""
tools/perf/perfbench.py -- the fixed benchmark for LogitOS.

WHY THIS EXISTS
---------------
"It got laggier" is not a number, and this tree has been wrong every time it
reasoned about performance instead of measuring it (mini-libc's O(N^2)
allocator; "emulation is why JS is slow", wrong by two orders of magnitude and
wrong about WHICH thing was slow).  So: one script, run against any commit,
that prints numbers.

WHAT MAKES THE NUMBERS TRUSTWORTHY
----------------------------------
The host is contended -- other agents run QEMU concurrently -- so host wall
clock is worthless here.  Three defences, in order of importance:

  1. EVERY DURATION IS MEASURED BY THE GUEST, NOT BY THE HOST.  A phase is
     bracketed by two reads of /dev/kstat's `uptime_ms`, which is the guest's
     own monotonic clock.  When the host deschedules QEMU, the guest's clock
     does advance (it is host-wallclock-derived under TCG), so this is a
     defence and not a cure -- but it removes all of the harness's own
     scheduling, pipe and Python latency from the measurement, which is the
     part we control.

  2. NOTHING IS PACED FROM THE HOST INSIDE A MEASURED WINDOW.  A phase's whole
     command block is written to the serial socket in ONE write before the
     guest starts executing it, so the guest never sits at the prompt waiting
     for the harness to type.  (QEMU's 16550 applies flow control, so a big
     blob is throttled, not dropped.)  This is why the phases are batched and
     not sent line by line.

  3. REPETITION AND A MEDIAN.  --repeat N does N independent boots; every
     metric is reported as median plus the interquartile-ish spread (min/max
     and MAD).  A single sample on this machine is noise.  Additionally each
     phase repeats its unit of work K times inside one boot, so the guest
     clock's 10 ms granularity (klog/kstat both tick at TIMER_HZ=100) is under
     1% of the measured interval instead of 25% of it.

  Plus a fourth, cheap one: `null_ms`, a phase that does nothing between its
  two kstat reads.  It is the harness's own floor -- one `cat /dev/kstat`
  process spawn.  Every other phase carries that same constant, so a number
  that moved while null_ms moved with it is the machine being slow, not the
  thing under test.

WHAT IT MEASURES
----------------
  boot_ok_ms       guest ms from reset to LOGIT_BOOT_OK   (kernel bring-up)
  desktop_ms       guest ms to the last `first-run tid` of a desktop app
                   (= the WM is live and has launched its apps)
  prompt_ms        guest ms at the first kstat read after /bin/sh's prompt
  null_ms          the floor: one kstat round trip, nothing else
  shell_ms         SHELL_N fork+execve round trips of /bin/true
  read_ms          READ_N reads of a 2.2 MB font off logitfs (`wc`)
  launch_ms        LAUNCH_N loads of the big ring-3 image named by --launch
  page_ms          PAGE_N fetches of a fixed host-served page (DNS+TCP+HTTP)
  mouse_ms         a fixed shell workload while MOUSE_N pointer motions are
                   injected over QMP -- the compositor tax, in the units the
                   owner actually feels
  mouse_tax_ms     mouse_ms - shell_ms, i.e. what the pointer cost

USAGE
    python3 tools/perf/perfbench.py --iso build/logit.iso --disk build/disk.img
    python3 tools/perf/perfbench.py ... --repeat 5 --json out.json
"""

import argparse
import json
import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE_DIR = os.path.join(HERE, "fixture")

PROMPT = "/ $ "

# Repetitions of the unit of work INSIDE one boot. Chosen so each phase lasts
# ~1 s of guest time: the guest clock ticks at 100 Hz, so a 1 s window has 1%
# quantisation error and a 40 ms window would have 25%.
SHELL_N = 24
READ_N = 6
LAUNCH_N = 8
PAGE_N = 8
MOUSE_N = 400


# --------------------------------------------------------------------------
# serial transport
# --------------------------------------------------------------------------
class Serial:
    """The guest's console. A reader thread accumulates; the writer never
    blocks the reader."""

    def __init__(self, sock, t0):
        self.sock = sock
        self.buf = bytearray()
        # (end_offset, host_time) per recv, so any byte in the stream can be
        # dated. This is the universal clock: /dev/kstat only exists in the
        # last few hours of today's history, so a harness that could only read
        # the guest's own clock could not measure most of the day. Serial bytes
        # arrive within well under a millisecond of the guest emitting them,
        # and the harness never types inside a measured window, so the interval
        # between two markers is guest execution as seen from here.
        self.stamps = [(0, t0)]
        self.lock = threading.Lock()
        self.alive = True
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()

    def _reader(self):
        while self.alive:
            try:
                b = self.sock.recv(65536)
            except OSError:
                break
            if not b:
                break
            now = time.time()
            with self.lock:
                self.buf += b
                self.stamps.append((len(self.buf), now))

    def text(self):
        with self.lock:
            return self.buf.decode("utf-8", "replace")

    def time_at(self, index):
        """Host time at which the byte at `index` arrived."""
        with self.lock:
            for end, t in self.stamps:
                if index < end:
                    return t
            return self.stamps[-1][1]

    def find(self, needle, start=0):
        """Byte offset of `needle`, or -1. Byte offsets, not character offsets:
        the console carries UTF-8 (the font and shaping lines print it) and a
        character index would not line up with the arrival stamps."""
        with self.lock:
            return self.buf.find(needle.encode(), start)

    def size(self):
        with self.lock:
            return len(self.buf)

    def slice(self, a, b):
        with self.lock:
            return bytes(self.buf[a:b]).decode("utf-8", "replace")

    def t_of(self, needle, start=0):
        """Host time the LAST byte of `needle` arrived, or None."""
        i = self.find(needle, start)
        if i < 0:
            return None
        return self.time_at(i + len(needle) - 1)

    def send(self, s):
        """One write, in the background: send() may block if the guest is busy
        and QEMU has stopped draining the socket, and that must not stall the
        reader thread or be mistaken for guest time."""
        data = s.encode()

        def _w():
            try:
                self.sock.sendall(data)
            except OSError:
                pass

        threading.Thread(target=_w, daemon=True).start()

    def wait_for(self, needle, timeout, proc=None):
        """Wait for `needle` to appear. Returns True/False -- never raises, so
        a build that boots but misbehaves yields a missing metric rather than a
        wrong one."""
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.text():
                return True
            if proc is not None and proc.poll() is not None:
                # QEMU died; give the reader a moment to drain, then decide.
                time.sleep(0.2)
                return needle in self.text()
            time.sleep(0.05)
        return False

    def close(self):
        self.alive = False
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# QMP, for pointer motion
# --------------------------------------------------------------------------
class Qmp:
    def __init__(self, path, timeout=10):
        self.f = None
        end = time.time() + timeout
        while time.time() < end:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                self.sock = s
                self.f = s.makefile("rwb")
                break
            except OSError:
                time.sleep(0.1)
        if self.f is None:
            return
        self._read()                       # greeting
        self._cmd({"execute": "qmp_capabilities"})

    def ok(self):
        return self.f is not None

    def _read(self):
        line = self.f.readline()
        if not line:
            return None
        try:
            return json.loads(line)
        except ValueError:
            return None

    def _cmd(self, obj):
        self.f.write((json.dumps(obj) + "\n").encode())
        self.f.flush()
        for _ in range(50):
            r = self._read()
            if r is None:
                return None
            if "return" in r or "error" in r:
                return r
        return None

    def move(self, dx, dy):
        self._cmd({
            "execute": "input-send-event",
            "arguments": {"events": [
                {"type": "rel", "data": {"axis": "x", "value": dx}},
                {"type": "rel", "data": {"axis": "y", "value": dy}},
            ]},
        })

    def close(self):
        try:
            if self.f:
                self.f.close()
            self.sock.close()
        except (OSError, AttributeError):
            pass


# --------------------------------------------------------------------------
# the host-served fixture
# --------------------------------------------------------------------------
class Fixture:
    """A python http.server on the host, reachable from the guest at
    10.0.2.2:<port> over SLIRP. Serving a file from disk (rather than a
    generated body) keeps the bytes identical on every commit."""

    def __init__(self, port):
        self.port = port
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1",
             "--directory", FIXTURE_DIR],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.4)

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# --------------------------------------------------------------------------
# one boot
# --------------------------------------------------------------------------
UPTIME_RE = re.compile(r"^uptime_ms\s+(\d+)", re.M)
KMSG_RE = re.compile(r"^\[\s*(\d+)\.(\d{3})\]\s+\S+\s+\S+\s+(.*)$", re.M)


def run_once(args, port, seq):
    """Boot once, run every phase, return {metric: value} (missing -> absent).

    A phase that does not complete inside its timeout is simply absent from the
    result. That distinction matters: the sweep must be able to tell "slower"
    from "this commit's kernel cannot do this at all", and never report the
    second as the first."""
    tmp = tempfile.mkdtemp(prefix="perfbench.")
    ser_path = os.path.join(tmp, "ser")
    qmp_path = os.path.join(tmp, "qmp")
    out = {}
    proc = None
    ser = None
    qmp = None
    try:
        qemu = os.environ.get("QEMU", "qemu-system-x86_64")
        cmd = [
            qemu, "-cpu", os.environ.get("QEMU_CPU", "max"),
            "-cdrom", args.iso,
            "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
            "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
            "-m", "512M",
            "-smp", str(args.smp), "-accel", "tcg,thread=multi",
            "-vga", "none", "-device", "virtio-gpu-pci",
            "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
            "-serial", "unix:%s,server=on,wait=off" % ser_path,
            "-qmp", "unix:%s,server=on,wait=off" % qmp_path,
            "-display", "none", "-no-reboot",
        ]
        t_launch = time.time()
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)

        s = None
        end = time.time() + 20
        while time.time() < end:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(ser_path)
                break
            except OSError:
                s = None
                time.sleep(0.1)
        if s is None:
            return out, "no-serial"
        ser = Serial(s, t_launch)

        # ---- boot ---------------------------------------------------------
        if not ser.wait_for(PROMPT, args.boot_timeout, proc):
            return out, "no-prompt"

        # Host-clock boot timeline. Works on EVERY commit, including the ~110
        # of today's that predate /dev/kstat and /dev/kmsg. GRUB's own delay is
        # in here as a constant; it is identical for every commit, so it biases
        # the absolute number and not the comparison.
        for key, needle in (("boot_ok_ms", "LOGIT_BOOT_OK"),
                            ("desktop_ms", "desktop live"),
                            ("prompt_ms", PROMPT)):
            t = ser.t_of(needle)
            if t is not None:
                out[key] = round((t - t_launch) * 1000.0)

        qmp = Qmp(qmp_path)

        # Does this build have the diagnostic devices? Their absence is normal
        # for most of today; it costs the guest-clock cross-check, not a phase.
        ser.send("cat /dev/kstat\necho PBPROBEZ\n")
        has_kstat = (ser.wait_for("PBPROBEZ\r", 60, proc)
                     and "logit kstat" in ser.text())

        # ---- the boot timeline, read back off klog -------------------------
        # Where klog exists it is better than the host clock: it is the guest's
        # own timestamp and it excludes GRUB. Read FIRST, before any phase runs
        # -- klog is a fixed-size ring and the phases emit [execve]/[sched]
        # records by the hundred, so reading it at the end loses LOGIT_BOOT_OK
        # to the benchmark's own noise. (Observed: it dropped the boot metric
        # from 2 of the first 3 runs.)
        if has_kstat:
            mark = ser.size()
            ser.send("cat /dev/kmsg\necho PBKMSGZ\n")
            if ser.wait_for("PBKMSGZ\r", 60, proc):
                seg = ser.slice(mark, ser.size())
                best_boot = None
                best_desk = None
                for sec, ms, msg in KMSG_RE.findall(seg):
                    t = int(sec) * 1000 + int(ms)
                    if "LOGIT_BOOT_OK" in msg and best_boot is None:
                        best_boot = t
                    if "first-run tid" in msg and ("Finder" in msg or "Clock" in msg):
                        best_desk = t if best_desk is None else max(best_desk, t)
                if best_boot is not None:
                    out["kboot_ok_ms"] = best_boot
                if best_desk is not None:
                    out["kdesktop_ms"] = best_desk

        # ---- phases -------------------------------------------------------
        def phase(name, lines, timeout, need=None, count=0):
            """Time one phase.

            The interval measured is between the arrival of marker A and marker
            Z on the console. Both markers are guest output; the whole command
            block was written in a single socket write BEFORE the guest began
            executing it, so the guest never waits on the harness inside the
            window and the interval is guest execution.

            Returns (host_ms, guest_ms_or_None).

            `need`/`count` are the phase's proof that it did the work. A phase
            whose command failed instantly would otherwise be recorded as an
            enormous speed-up -- the single most dangerous failure mode a perf
            harness has, and the one tests/qmp/qmp_freeze.py fell into."""
            tag = "PB%s" % name
            probe = "cat /dev/kstat\n" if has_kstat else ""
            blob = ("echo %sA\n" % tag + probe
                    + "".join(l + "\n" for l in lines)
                    + probe + "echo %sZ\n" % tag)
            mark = ser.size()
            ser.send(blob)
            if not ser.wait_for(tag + "Z\r", timeout, proc):
                return None, None
            i = ser.find(tag + "A\r", mark)
            j = ser.find(tag + "Z\r", mark)
            if i < 0 or j < 0 or j <= i:
                return None, None
            body = ser.slice(i, j)
            if need is not None and body.count(need) < count:
                return None, None
            host_ms = round((ser.time_at(j) - ser.time_at(i)) * 1000.0)
            guest_ms = None
            ups = UPTIME_RE.findall(body)
            if len(ups) >= 2:
                guest_ms = int(ups[-1]) - int(ups[0])
            return host_ms, guest_ms

        def record(key, r):
            h, g = r
            if h is not None:
                out[key] = h
            if g is not None:
                out["g_" + key] = g
            return h

        # A discarded warm-up. The first seconds after the prompt are not idle
        # -- the waitq self-test, the time cross-check and the IPv6 DAD timers
        # all still fire -- so the first phase measured would be measuring
        # them. Warming also faults in /bin/cat and /bin/true off the disk.
        phase("warm", ["true"] * 4, 60)

        record("null_ms", phase("null", [], 30))
        record("shell_ms", phase("shell", ["true"] * SHELL_N, 90))
        record("read_ms", phase("read", ["wc %s" % args.readfile] * READ_N, 120,
                                need=args.readfile, count=READ_N))
        record("launch_ms", phase("launch", [args.launch] * LAUNCH_N, 180))

        url = "http://10.0.2.2:%d/bench.html" % port
        record("page_ms", phase("page", ["net get %s" % url] * PAGE_N, 180,
                                need="http bytes ", count=PAGE_N))

        # ---- the compositor tax -------------------------------------------
        # Same shell workload as shell_ms, but with the pointer moving. The
        # difference is what compositing the desktop costs the rest of the
        # machine -- which is the thing the owner reports feeling. Relative
        # motion is used on purpose: absolute coordinates rot (this repo's
        # qmp_freeze.py is the cautionary tale) whereas "move by one pixel"
        # cannot.
        #
        # The comparison is BRACKETED: A, mouse, B, and the tax is mouse minus
        # the mean of its two neighbours -- not against a phase measured six
        # phases earlier, because the later a phase runs the faster it is (warm
        # cache, boot self-tests finished).
        #
        # The workload is the FILE READ, not the fork/exec one, and that choice
        # is a measurement finding in itself. With `true`x24 the mouse phase
        # came out 30-40% FASTER than its own brackets, consistently. The cause
        # is not compositing: tty_read blocks in `hlt` and is woken by the next
        # interrupt, so with the pointer still it waits up to a 10 ms timer
        # tick per character, while a moving pointer delivers PS/2 IRQ12s that
        # wake it immediately. The shell round trip is tick-latency-bound, so
        # any interrupt source at all speeds it up and the compositor's cost
        # disappears underneath that. `wc` of a 2.2 MB file spends its time in
        # the guest rather than waiting on the console, so what the pointer
        # costs is visible.
        if qmp is not None and qmp.ok():
            mwork = ["wc %s" % args.readfile] * READ_N
            a = record("mouseA_ms", phase("mouseA", mwork, 120,
                                          need=args.readfile, count=READ_N))
            stop = threading.Event()

            def jiggle():
                d = 3
                for _ in range(MOUSE_N):
                    if stop.is_set():
                        break
                    qmp.move(d, d)
                    d = -d

            th = threading.Thread(target=jiggle, daemon=True)
            th.start()
            m = record("mouse_ms", phase("mouse", mwork, 180,
                                         need=args.readfile, count=READ_N))
            stop.set()
            th.join(timeout=10)
            b = record("mouseB_ms", phase("mouseB", mwork, 120,
                                          need=args.readfile, count=READ_N))
            if m is not None and a is not None and b is not None:
                out["mouse_tax_ms"] = round(m - (a + b) / 2.0)
                out["mouse_tax_pct"] = round(100.0 * (m - (a + b) / 2.0)
                                             / ((a + b) / 2.0), 1)

        # A second reading of the floor, at the END of the run. If null_ms and
        # null2_ms disagree the machine drifted under us and every number
        # between them inherits that drift -- which is a thing to know rather
        # than to hide.
        record("null2_ms", phase("null2", [], 30))

        # Overhead-subtracted forms. The floor is `echo`+prompt, plus one
        # `cat /dev/kstat` where that device exists -- a process spawn and
        # ~1.5 KB pushed out of a 16550 one `outb` at a time, which under TCG
        # is not free. It sits inside every phase, so the subtraction is what
        # makes the phases comparable; the raw numbers are kept too, because
        # the subtraction is an assumption.
        floors = [out[k] for k in ("null_ms", "null2_ms") if k in out]
        if floors:
            floor = min(floors)
            out["floor_ms"] = floor
            for k in ("shell_ms", "read_ms", "launch_ms", "page_ms", "mouse_ms"):
                if k in out:
                    out[k[:-3] + "_net_ms"] = max(0, out[k] - floor)

        return out, "ok"
    finally:
        if qmp is not None:
            qmp.close()
        if ser is not None:
            ser.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


# --------------------------------------------------------------------------
METRICS = ["boot_ok_ms", "desktop_ms", "prompt_ms",
           "kboot_ok_ms", "kdesktop_ms",
           "null_ms", "null2_ms", "floor_ms",
           "shell_ms", "shell_net_ms", "read_ms", "read_net_ms",
           "launch_ms", "launch_net_ms", "page_ms", "page_net_ms",
           "mouse_ms", "mouse_net_ms", "mouseA_ms", "mouseB_ms",
           "mouse_tax_ms", "mouse_tax_pct",
           "g_shell_ms", "g_read_ms", "g_launch_ms", "g_page_ms", "g_mouse_ms"]


def summarise(runs):
    """median + spread per metric. MAD (median absolute deviation) rather than
    stdev: with 3-5 samples on a contended box, one starved run should not be
    allowed to define the spread."""
    res = {}
    for m in METRICS:
        vals = [r[m] for r in runs if m in r]
        if not vals:
            continue
        med = statistics.median(vals)
        mad = statistics.median([abs(v - med) for v in vals]) if len(vals) > 1 else 0
        res[m] = {"median": med, "mad": mad, "min": min(vals), "max": max(vals),
                  "n": len(vals), "values": vals}
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/logit.iso")
    ap.add_argument("--disk", default="build/disk.img")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--port", type=int, default=0, help="0 = pick a free one")
    ap.add_argument("--boot-timeout", type=float, default=90.0)
    # NOT the browser: a GUI .aex execve'd from a tty has no window and never
    # returns (probed -- it hangs at the prompt), so it cannot be a phase. The
    # honest decomposition of "the browser takes ages to open" is (a) the
    # loader path -- fork + execve + page in a large mini-libc-linked image,
    # which is what /bin/as is here -- and (b) reading megabytes off logitfs,
    # which is read_ms. Both are measured; neither is called "browser launch".
    ap.add_argument("--launch", default="as /usr/as/examples/hello.as",
                    help="command whose cost is 'load and run a large ring-3 image'")
    ap.add_argument("--readfile", default="/fonts/ui.ttf")
    ap.add_argument("--json", default=None)
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    port = args.port
    if port == 0:
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()

    fx = Fixture(port)
    runs = []
    statuses = []
    try:
        for i in range(args.repeat):
            r, st = run_once(args, port, i)
            statuses.append(st)
            runs.append(r)
            print("  run %d/%d: %s %s" % (i + 1, args.repeat, st,
                                          {k: r[k] for k in METRICS if k in r}),
                  file=sys.stderr)
    finally:
        fx.close()

    res = summarise(runs)
    doc = {"label": args.label, "iso": args.iso, "repeat": args.repeat,
           "statuses": statuses, "metrics": res,
           "reps": {"shell": SHELL_N, "read": READ_N, "launch": LAUNCH_N,
                    "page": PAGE_N, "mouse": MOUSE_N}}
    if args.json:
        with open(args.json, "w") as f:
            json.dump(doc, f, indent=1)

    print("%-14s %10s %8s %10s %10s %3s" % ("metric", "median", "mad", "min", "max", "n"))
    for m in METRICS:
        if m in res:
            d = res[m]
            print("%-14s %10.1f %8.1f %10.1f %10.1f %3d"
                  % (m, d["median"], d["mad"], d["min"], d["max"], d["n"]))
    if not res:
        print("NO METRICS: %s" % statuses)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
