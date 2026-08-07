#!/usr/bin/env python3
"""Measure mini-libc's allocator ON THE REAL MACHINE, through QuickJS.

    python3 tests/qmp/qmp_malloc_bench.py <iso> <disk.img> [budget-seconds]

The host unit test (`make test-malloc`) settles the algorithm. It cannot settle
whether the algorithm is what the browser actually runs: the browser links its
own 96 MiB-arena copy of malloc.c, the JS heap is QuickJS's, and every object it
builds goes through both. So this boots LogitOS, serves a fixture page from the
host over SLIRP, and lets the page's inline <script> build an object graph in
doubling stages -- 12500, 25000, 50000, 100000, 200000 nodes, every one held
live, which is the shape a React tree has and the shape the old first-fit
allocator was quadratic on.

Stage timings come from the GUEST's own Date.now(). That is trustworthy here:
mini-libc's gettimeofday() is built on SYS_MONOTONIC_MS (c/apps/libc/src/time.c),
a millisecond monotonic counter, not on the whole-second RTC. It also measures
the right thing -- the loop, not the loop plus serial-log latency. The harness
does keep host wall clock as a coarse cross-check, and needs it for the timeout,
but with the fixed allocator the whole 200k sweep runs in about a second and
host-side polling is far too coarse to resolve the stages.

Reading the result: with an O(1) allocator each stage allocates twice as many
nodes as the one before it and should take roughly twice as long, i.e. a flat
us/node column. With the old O(N^2) allocator each stage took about four times
as long, and the sweep does not finish inside any sane budget -- which is why
there is a budget at all. The run stops early rather than hanging and prints the
partial curve, because a partial curve still shows the exponent.

Exit status is 0 when the sweep completed and every doubling cost < 2.8x.
"""

import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, dock_icon, BROWSER_SLOT       # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
BUDGET = float(sys.argv[3]) if len(sys.argv) > 3 else 900.0
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

STAGES = [12500, 25000, 50000, 100000, 200000]

# Every node is a fresh object literal with four properties plus a slot in a
# growing array: an object header, a property record and an array realloc per
# node, all out of mini-libc's arena. Nothing is ever dropped, so live count ==
# node count and the allocator cannot recycle its way out of the problem.
PAGE = """<!doctype html>
<html><head><title>malloc bench</title></head>
<body style="background:#ffffff;color:#000000;font-size:24px">
<div id="out">BENCHRUNNING</div>
<script>
console.log('MALLOCBENCH-START');
var held = [];
var stages = [%s];
var t_all = Date.now();
for (var s = 0; s < stages.length; s++) {
  var target = stages[s];
  var t0 = Date.now();
  while (held.length < target) {
    var i = held.length;
    held.push({ id: i, name: 'n', flag: (i & 1) === 0, prev: null });
  }
  var n = held.length;
  console.log('MALLOCBENCH-STAGE ' + target + ' guest_ms=' + (Date.now() - t0) +
              ' live=' + n);
}
console.log('MALLOCBENCH-DONE nodes=' + held.length +
            ' guest_total_ms=' + (Date.now() - t_all));
document.getElementById('out').textContent = 'BENCHDONE';
</script>
</body></html>
""" % ",".join(str(s) for s in STAGES)

tmp = tempfile.mkdtemp(prefix="qmp_mallocbench_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        raw = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_a):
        pass


srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg, code=1):
    print("FAIL: " + msg)
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-4000:])
    proc.kill()
    sys.exit(code)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    return False


try:
    if not wait_serial("LOGIT_BOOT_OK", 240, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 120, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    for _ in range(4):
        if wait_serial("launched Browser", 15, "browser launch"):
            break
        ui.click_at(*dock_icon(BROWSER_SLOT))
    else:
        die("the Dock never launched the Browser")
    time.sleep(6)

    ui.click_at(420, 145)                      # address bar
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/bench.html" % PORT)
    ui.key("ret")

    if not wait_serial("MALLOCBENCH-START", 120, "page load"):
        die("the fixture page never loaded / its script never ran")

    t_start = time.time()
    deadline = t_start + BUDGET
    seen, times, incomplete = [], [], False
    prev_n = 0
    for stage in STAGES:
        pat = re.compile(r"MALLOCBENCH-STAGE %d guest_ms=(\d+) live=(\d+)" % stage)
        while True:
            m = pat.search(serial())
            if m:
                break
            if time.time() > deadline:
                incomplete = True
                break
            if proc.poll() is not None:
                die("QEMU exited mid-benchmark (out of memory? see the serial tail)")
            time.sleep(0.5)
        if incomplete:
            print("    ... budget of %.0fs exhausted waiting for stage %d "
                  "(host wall clock %.0fs in)" % (BUDGET, stage, time.time() - t_start))
            break
        ms = int(m.group(1))
        seen.append(stage)
        times.append(ms)
        prev_n = stage

    print("\nguest-measured, Date.now() over SYS_MONOTONIC_MS (1 ms resolution)\n")
    print("    %-10s %10s %10s %12s   %s" %
          ("live nodes", "new", "guest ms", "us/node", "ratio vs prev stage"))
    prev_n = 0
    for i, stage in enumerate(seen):
        new = stage - prev_n
        line = "    %-10d %10d %10d %12.1f" % (stage, new, times[i], times[i] * 1e3 / new)
        if i:
            line += "   x%.2f" % (times[i] / times[i - 1] if times[i - 1] else 0)
        print(line)
        prev_n = stage

    m = re.search(r"MALLOCBENCH-DONE nodes=(\d+) guest_total_ms=(\d+)", serial())
    if m:
        print("\n    guest built %s nodes in %s ms; host wall clock for the whole "
              "sweep was %.1f s" % (m.group(1), m.group(2), time.time() - t_start))

    fails = 0
    if incomplete or len(seen) < len(STAGES):
        print("\nINCOMPLETE: only %d of %d stages finished inside %.0fs"
              % (len(seen), len(STAGES), BUDGET))
        fails += 1
    for i in range(1, len(times)):
        r = times[i] / times[i - 1] if times[i - 1] else 1e9
        ok = r < 2.80
        print("%s: doubling %d -> %d nodes cost x%.2f (linear x2, quadratic x4)"
              % ("ok" if ok else "FAIL", seen[i - 1], seen[i], r))
        if not ok:
            fails += 1

    # The crisp one. Cost per node is FLAT for an O(1) allocator no matter how
    # many nodes are already live; for the old one it doubled with every
    # doubling of the live set, so across this sweep it grew about 16x.
    if len(seen) >= 2:
        first = times[0] * 1e3 / seen[0]
        last = times[-1] * 1e3 / (seen[-1] - seen[-2])
        ok = last < first * 2.5
        print("%s: cost per node went %.1f -> %.1f us as the live set grew "
              "%dx (x%.2f; flat is x1, quadratic would be x%d)"
              % ("ok" if ok else "FAIL", first, last, seen[-1] // seen[0],
                 last / first if first else 0, seen[-1] // seen[0]))
        if not ok:
            fails += 1

    print("\n%s" % ("MALLOC GUEST BENCH PASS" if not fails else
                    "MALLOC GUEST BENCH FAILED (%d)" % fails))
    proc.kill()
    sys.exit(1 if fails else 0)
except SystemExit:
    raise
except Exception as exc:                       # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
