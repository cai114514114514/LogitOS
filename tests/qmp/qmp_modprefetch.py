#!/usr/bin/env python3
"""THE MODULE-GRAPH PREFETCH CONTROL, watched failing before it was trusted.

    python3 tests/qmp/qmp_modprefetch.py <iso> <disk.img> [--expect-slow]

WHAT THIS MEASURES, and why it is not a host stopwatch: js_module.c's
mod_compile_and_prefetch() (see that file's own comment) turns each module's
fan-out into ONE CONCURRENT WAVE instead of a chain of round trips. The
fixture below is a root module statically importing 18 leaf modules split
across THREE origins (three local HTTP ports), 6 leaves per origin -- which
lines up exactly with browser_rt.c's hpool_config(&g_pool, 6, 2, 60000): two
connections per origin, three origins, six concurrent requests in flight at
once. Every fixture response sleeps 200ms before answering, on the HOST, so
the wall-clock signal is dominated by that constant and not by TCG's own
variance.

THE NUMBER READ IS FROM THE GUEST'S OWN CLOCK, not this script's. browser_rt.c
already stamps two lines with monotonic_ms() around exactly the window this
test cares about: "[wa] t=... nav" when the navigation is armed, and
"[wa] t=... loadend ..." once the whole load (module graph included) has
finished -- printed right before browser.c's own "load done" line. This
driver's ONLY measurement is (loadend.t - nav.t), read off those two lines,
because AGENTS.md's rule is explicit: never measure wall clock on the host,
other agents run QEMU on it concurrently. The 200ms-per-request DELAY is a
host sleep only because it has to live somewhere outside the guest's own
clock (something has to make a round trip slow); the DURATION under test is
measured entirely inside the guest.

THE BOUNDS were set from a real measurement, not an estimate: run 2026-09-02,
this fixture, this host, both builds watched both ways -- prefetch 2440ms,
no-prefetch 5220ms. That is a clean ~2.14x, well short of the ~6x the fixture's
3-origin/6-connection shape allows in principle, because the number also
carries fixed costs neither build can avoid (the document and root.mjs fetches
ahead of any leaf, HTML/script-collection overhead, QEMU usermode NAT connect
latency to three ports, TCG scheduling jitter) and this script does not try to
subtract them out -- see FAST_BOUND_MS/SLOW_BOUND_MS's own comment for where
the bounds sit relative to those two readings.

--expect-slow selects the OTHER assertion (elapsed must EXCEED SLOW_BOUND):
tests/jsmodpf.mk's negctl target passes it against a browser.aex built with
-DJS_MODULE_NO_PREFETCH (js_module.c's own control, see that file), which is
mod_compile_and_prefetch()'s exact pre-prefetch code path -- one JS_Eval call,
QuickJS's own unconditional resolve, no seam for concurrency. If the two
builds print the same number, prefetch is not happening; that is the whole
test.
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
from qmp_ui import Session      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
EXPECT_SLOW = "--expect-slow" in sys.argv[3:]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

N_ORIGINS = 3
N_PER_ORIGIN = 6
N = N_ORIGINS * N_PER_ORIGIN            # 18 -- matches hpool's 2-per-origin x 3
DELAY = float(os.environ.get("MODPF_DELAY", "0.2"))
HOST = "10.0.2.2"                        # QEMU usermode NAT's view of the host

FAST_BOUND_MS = 3800                     # prefetch build must finish under this
SLOW_BOUND_MS = 4200                     # no-prefetch build must take longer
# MEASURED 2026-09-02 (this fixture, this host, both builds, watched both ways):
# prefetch 2440ms, no-prefetch 5220ms -- a clean ~2.14x, not the ~6x ceiling the
# fixture's 3-origin/6-connection shape allows (hpool's per-origin idle probe,
# QEMU usermode NAT's own connect latency across 3 ports, and TCG scheduling
# jitter all sit inside the measured number and were not separated out). The
# bounds above sit in the ~1000-1400ms of headroom either side of those two
# readings, not at their midpoint (2830ms) -- a run landing in the gap is a
# broken test, not a coin flip, and either bound is far enough from its build's
# reading to absorb host jitter without the two ever being able to swap places.

# ---------------------------------------------------------------- fixture --

class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"        # keep-alive: a 1.0 server would hide
                                          # the very pooling this measures

    def do_GET(self):
        path = self.path.split("?")[0]
        time.sleep(DELAY)                # the ONE thing that makes a round
                                          # trip slow enough to see
        entry = self.server.files.get(path)
        if entry is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body, ctype = entry
        raw = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        # Every response, unconditionally: this test is about connection
        # concurrency, not about re-litigating CORS, and the fixture is not a
        # real third party -- withholding the header would make a defect in
        # THAT unrelated mechanism read as a defect in prefetch.
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_a):
        pass


def start_server():
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
    srv.files = {}
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


servers = [start_server() for _ in range(N_ORIGINS)]
origins = [(HOST, s.server_port) for s in servers]

root_lines = []
names = []
idx = 0
for oi, (h, p) in enumerate(origins):
    for k in range(N_PER_ORIGIN):
        nm = "v%d" % idx
        servers[oi].files["/leaf%d.mjs" % idx] = (
            "export const v = %d;\n" % idx, "text/javascript")
        root_lines.append(
            "import { v as %s } from 'http://%s:%d/leaf%d.mjs';" % (nm, h, p, idx))
        names.append(nm)
        idx += 1
assert idx == N

root_lines.append("var sum = " + " + ".join(names) + ";")
root_lines.append("console.log('MODPF-DONE sum=' + sum);")
ROOT_MJS = "\n".join(root_lines) + "\n"

servers[0].files["/root.mjs"] = (ROOT_MJS, "text/javascript")
PAGE = ("<!doctype html>\n<html><head><title>modpf</title></head><body>"
        "<script type=\"module\" src=\"http://%s:%d/root.mjs\"></script>"
        "</body></html>\n") % origins[0]
servers[0].files["/"] = (PAGE, "text/html")
servers[0].files["/index.html"] = (PAGE, "text/html")

BASE_URL = "http://%s:%d/" % origins[0]

# ------------------------------------------------------------------ boot --

tmp = tempfile.mkdtemp(prefix="qmp_modpf_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

checks = []


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    for ok, name in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
    print("----- serial (tail) -----")
    print(serial()[-6000:])
    print("-------------------------")
    proc.kill()
    sys.exit(1)


def ck(cond, name):
    checks.append((bool(cond), name))
    print(("ok: " if cond else "FAIL: ") + name)
    if not cond:
        die(name)


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
    if not wait_serial("desktop live", 90, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path, serial=serial_path)
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(6)

    # Snapshot BEFORE navigating: the browser's own startup page may already
    # have printed a nav/loadend pair for something that is not this test, and
    # scanning only the suffix is what keeps this measurement from accidentally
    # reading somebody else's load.
    before = serial()

    # The address bar is already focused on the first navigation (every other
    # [wa]/module driver in this tree relies on the same fact -- see
    # qmp_module_page.py's goto()).
    for _ in range(90):
        ui.key("backspace", settle=0.02)
    ui.typ(BASE_URL)
    ui.key("ret")

    ck(wait_serial("MODPF-DONE sum=", 60, "module graph"),
       "the root module's 18 static imports (3 origins x 6) all resolved and ran")

    # loadend is printed right before browser.c's "load done" -- give it a
    # moment to land on serial after the script itself has finished.
    ck(wait_serial(" loadend ", 15, "the load-end timeline stamp"),
       "browser_rt.c printed its per-load [wa] loadend stamp")

    log = serial()
    after = log[len(before):]

    m_nav = re.search(r"\[wa\] t=(\d+) \(\+\d+\) nav\b", after)
    ck(m_nav is not None, "found this navigation's own [wa] ... nav stamp")
    m_end = re.search(r"\[wa\] t=(\d+) loadend ", after[m_nav.end():])
    ck(m_end is not None,
       "found the matching [wa] ... loadend stamp after it")

    t_nav = int(m_nav.group(1))
    t_end = int(m_end.group(1))
    elapsed = t_end - t_nav
    print("    guest clock: nav t=%d loadend t=%d elapsed=%dms "
          "(N=%d origins=%d delay=%dms)"
          % (t_nav, t_end, elapsed, N, N_ORIGINS, int(DELAY * 1000)))
    ck(elapsed > 0, "loadend came after nav on the guest's own clock")

    if EXPECT_SLOW:
        ck(elapsed > SLOW_BOUND_MS,
           "NO-PREFETCH build took MORE than %dms (%dms) -- one request in "
           "flight at a time, ~N x %dms" % (SLOW_BOUND_MS, elapsed, int(DELAY * 1000)))
    else:
        ck(elapsed < FAST_BOUND_MS,
           "prefetch build took LESS than %dms (%dms) -- the 6-wide wave, "
           "~ceil(N/6) x %dms" % (FAST_BOUND_MS, elapsed, int(DELAY * 1000)))

    label = "no-prefetch (control)" if EXPECT_SLOW else "prefetch (default)"
    print("\nPASS: module-graph %s build: %dms for %d static imports across "
          "%d origins, read off the guest's own clock" % (label, elapsed, N, N_ORIGINS))
    proc.kill()
    sys.exit(0)
except Exception:
    proc.kill()
    raise
