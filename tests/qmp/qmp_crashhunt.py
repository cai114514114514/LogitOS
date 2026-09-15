#!/usr/bin/env python3
"""Hunt the browser's SOMETIMES-sudden-exit (闪退) with a named stress pattern.

    python3 tests/qmp/qmp_crashhunt.py --iso build-crashhunt/logit.iso \
        --disk build-crashhunt/disk.img --scenario canvas \
        --out /tmp/ch-canvas.json

WHAT THIS IS
============
An INSTRUMENT, in qmp_site.py's sense: it runs one scenario in one boot, writes
down everything the machine said, and the EXIT CODE IS ABOUT THE HARNESS. A
browser that dies is a RESULT (the JSON verdict says which death), not a driver
failure; the gate in tests/crashhunt.mk asserts the verdict, this file only
measures it. Keeping the policy out of the driver is what lets the same driver
gate a SURVIVED expectation today and an expected-fail ratchet tomorrow without
editing either's definition of "measured".

WHAT A BROWSER DEATH LOOKS LIKE FROM OUTSIDE, and where the evidence lives.
Established from the kernel sources BEFORE the first boot, because the first
house rule is "is the harness looking at the machine or at itself":

  1. `[fault] app exception: <name> (vector N) rip=... cr2=... -- terminating
     app`  (c/kernel/cpu/irq/interrupts.c:244) -- a ring-3 fault killed the app.
     The kernel survives; the window is reaped SILENTLY (wm.c reap() prints
     nothing), so without this line a fault-kill and a user close are the same
     picture.
  2. `[oom] page fault: out of memory ...` + `[oom] victim: pid N "browser"
     rss=... [window]` (c/kernel/mm/reclaim/reclaim/oom.c) -- the kernel OOM-killed the
     browser. The kill is a MARK; the process dies at its next kernel entry,
     and `[proc] kill: pid N marked` names the moment.
  3. `LOGIT_PANIC` -- the kernel itself died; every app "disappears".
  4. NOTHING AT ALL -- a clean SYS_EXIT. In the production browser this is
     reachable only from EV_CLOSE (browser.c:4285), so a window that vanishes
     with no line and no user close is the SILENT-EXIT shape, and it needs its
     own verdict rather than being rounded up to "survived".
  5. `[js] watchdog: script exceeded its CPU slice ... -- interrupted`
     (js_page.c) -- a runaway SCRIPT is interrupted; the browser SURVIVES. It
     is recorded as pressure, not as death, because conflating the two would
     hide exactly the distinction this hunt exists to make.

LIVENESS IS PROVED TWO WAYS, weakest first, and the second is the referee:
  * `about:text` ping: Ctrl+L, type about:text, Enter. load() prints
    `[browser] load: about:text` for it and about:text DOES NOT NAVIGATE
    (browser.c:1669-1697), so the ping cannot disturb the page under test.
    A browser that is alive but wedged inside a blocking script swallows the
    keystrokes (the qmp_site.py about:text lesson: keys into a busy browser go
    into the PS/2 void) -- so a missed ping alone proves nothing.
  * DOCK RE-CLICK: clicking the Browser dock tile makes the WM itself say
    which world it is in -- `[wm] launch: already live, focusing` (alive) vs a
    fresh `[wm] launched Browser` (the app had died; the click started a NEW
    process). That line is printed by the launch path, not inferred from
    pixels, and it is the only outside-in signal that separates "died and
    reaped" from "alive but not answering".

ONE SCENARIO PER BOOT, no exceptions, for the reason qmp_site.py states:
cross-scenario state (restored tabs, leaked JS heap, a previous page's
sockets) manufactures crashes the scenarios did not cause. The control
scenario (`idle`) exists so "the machine survived the wall-clock" is measured,
not assumed: a death in the canvas loop means one thing only if an idle boot
of the same duration lives.

GUEST TIME, NOT HOST TIME, per AGENTS.md: survival is reported from the
guest's own clock lines on the serial (`[time] ... uptime Ns`, `[wm] perf
t=<ms>`), because five sibling QEMUs make host wall-clock a measurement of the
host. Both are recorded; the record's "guest" block is the one to quote.

THE MEMORY SIZE IS `make run`'s (1G, 1920x1200), not the 512M the older
drivers spell: the owner's crashes happened at the interactive desktop, which
is 1 GiB, and the browser peaks near 600 MB there (CLAUDE.md ~L65). A 512M
boot answers a different question; --mem/--mode exist to ask it on purpose.
"""

import argparse
import http.server
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui                                                     # noqa: E402
from qmp_ui import Session                                        # noqa: E402

BOOT_BUDGET = float(os.environ.get("CH_BOOT", 300))
DESKTOP_BUDGET = 120.0
SELFTEST_BUDGET = float(os.environ.get("CH_SELFTEST", 90))
PING_WAIT = 14.0          # about:text round trip; generous for TCG
RELAUNCH_WAIT = 45.0      # a FRESH browser launch is slow (~3 MB off virtio-blk)
PARK = (40, 40)           # over the menu bar, away from dock and content

FIXDIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "fixtures", "crashhunt")

SELFTEST = ("<!doctype html><html><head><title>sb</title></head>"
            "<body style='background:#ffffff'><div style='font-size:28px'>READY</div>"
            "<script>console.log('CH-READY-%s');</script></body></html>")


# --------------------------------------------------------------- the watcher
#
# Everything the guest says, triaged as it lands. A THREAD because two things
# must happen at once: the scenario is typing/clicking on the QMP socket while
# the serial file grows under us, and a death line can arrive at any moment --
# including between two keystrokes of the liveness ping that was about to test
# for it.

FAULT_RE = re.compile(r"\[fault\] app exception: ([^\n]+)")
OOM_RE = re.compile(r"\[oom\][^\n]*")
WM_PERF_RE = re.compile(r"\[wm\] perf t=(\d+)")
UPTIME_RE = re.compile(r"uptime (\d+)s")
HEAP_RE = re.compile(r"\[browser\] heap peak (\d+)K")
MMLOW_RE = re.compile(r"\[mm\] low: (\d+) frames free \((\d+) MiB\)")
WATCHDOG_RE = re.compile(r"\[js\] watchdog[^\n]*|\[worker \d+\] watchdog[^\n]*")
PANIC_RE = re.compile(r"LOGIT_PANIC|Kernel panic|double fault|triple fault",
                      re.I)
EXC_RE = re.compile(r"\[browser\] JS exception: (.*)$")


class Watcher:
    def __init__(self, serial_path, proc):
        self.path = serial_path
        self.proc = proc
        self.pos = 0
        self.stop = threading.Event()
        self.death = threading.Event()        # any of fault/oom-victim/panic
        self.faults = []                      # full lines
        self.oom = []
        self.panics = []
        self.watchdogs = []
        self.mm_low = []                      # (frames, MiB) tuples
        self.heap_peaks = []                  # ints, KiB
        self.exceptions = 0
        self.guest_uptime_s = 0
        self.guest_wm_ms = 0
        self.qemu_exit_code = None
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while not self.stop.is_set():
            try:
                with open(self.path, "rb") as fh:
                    fh.seek(self.pos)
                    chunk = fh.read()
            except OSError:
                chunk = b""
            if chunk:
                self.pos += len(chunk)
                self._parse(chunk.decode("utf-8", "replace"))
            rc = self.proc.poll()
            if rc is not None and self.qemu_exit_code is None:
                self.qemu_exit_code = rc
            time.sleep(0.25)

    def _parse(self, text):
        for ln in text.splitlines():
            m = WM_PERF_RE.search(ln)
            if m:
                self.guest_wm_ms = int(m.group(1))
            m = UPTIME_RE.search(ln)
            if m:
                self.guest_uptime_s = int(m.group(1))
            m = HEAP_RE.search(ln)
            if m:
                with self.lock:
                    self.heap_peaks.append((self.guest_wm_ms, int(m.group(1))))
            m = MMLOW_RE.search(ln)
            if m:
                with self.lock:
                    # (guest_wm_ms, frames_free): the low-water line prints on
                    # each new 1 MiB low, so the pair is a timed drain curve
                    self.mm_low.append((self.guest_wm_ms, int(m.group(1))))
            if EXC_RE.search(ln):
                with self.lock:
                    self.exceptions += 1
            if WATCHDOG_RE.search(ln):
                with self.lock:
                    self.watchdogs.append(ln.strip())
            if PANIC_RE.search(ln):
                with self.lock:
                    self.panics.append(ln.strip())
                self.death.set()
            if "[fault] app exception" in ln:
                with self.lock:
                    self.faults.append(ln.strip())
                self.death.set()
            if ln.startswith("[oom]"):
                with self.lock:
                    self.oom.append(ln.strip())
                # Only a victim mark is a death. `[oom] pmm_alloc refused`,
                # reaps and survives are PRESSURE, not death -- conflating
                # them would make every low-memory moment a crash verdict.
                if "victim:" in ln:
                    self.death.set()

    def snapshot(self):
        with self.lock:
            heaps = [h for h in self.heap_peaks]
            return {"faults": list(self.faults), "oom": list(self.oom),
                    "panics": list(self.panics), "watchdogs": list(self.watchdogs),
                    "mm_low": [(t, f) for (t, f) in self.mm_low],
                    "heap_peak_k_max": (max(h[1] for h in heaps)
                                        if heaps else None),
                    "heap_peaks": heaps[-8:],
                    "js_exceptions": self.exceptions,
                    "guest_uptime_s": self.guest_uptime_s,
                    "guest_wm_ms": self.guest_wm_ms,
                    "qemu_exit_code": self.qemu_exit_code}


# ------------------------------------------------------------------ plumbing

def ctrl(ui, ch):
    ui.key_mods(("ctrl",), ch)
    time.sleep(0.3)


def nav(ui, base, watcher, url, wait_load=75.0, settle=3.0):
    """Type a URL and confirm the browser said it loaded it. The confirmation
    is against `[browser] load: ` (the browser's own line), because a URL that
    never arrived is a harness miss that otherwise reads as a page crash."""
    mark = watcher.pos
    ctrl(ui, "l")
    ui.typ(url)
    ui.key("ret")
    end = time.time() + wait_load
    load_line = None
    while time.time() < end and not watcher.death.is_set():
        try:
            with open(watcher.path, "rb") as fh:
                fh.seek(mark)
                text = fh.read().decode("utf-8", "replace")
        except OSError:
            text = ""
        if "[browser] load: " in text:
            for ln in text.splitlines():
                if "[browser] load: " in ln:
                    load_line = ln.split("[browser] load: ", 1)[1].strip()
                    break
            break
        time.sleep(0.4)
    time.sleep(settle)
    return load_line


def ping(ui, watcher):
    """The about:text liveness ping. Returns True iff the browser answered.
    A False is NOT a death verdict -- see the module docstring's liveness
    block; the dock re-click referee runs after this returns False."""
    mark = watcher.pos
    ctrl(ui, "l")
    ui.typ("about:text")
    ui.key("ret")
    end = time.time() + PING_WAIT
    while time.time() < end:
        try:
            with open(watcher.path, "rb") as fh:
                fh.seek(mark)
                text = fh.read().decode("utf-8", "replace")
        except OSError:
            text = ""
        if "[browser] load: about:text" in text:
            return True
        if watcher.death.is_set():
            return False
        time.sleep(0.4)
    return False


def referee(ui, watcher):
    """Dock re-click. Returns 'alive', 'relaunched' (it had died), or None
    (neither line arrived: the WM itself is not answering)."""
    mark = watcher.pos
    dock = ui.dock()
    x, y = qmp_ui.dock_icon_of("browser", dock)
    ui.click_at(x, y)
    end = time.time() + RELAUNCH_WAIT
    while time.time() < end:
        try:
            with open(watcher.path, "rb") as fh:
                fh.seek(mark)
                text = fh.read().decode("utf-8", "replace")
        except OSError:
            text = ""
        if "[wm] launch: already live, focusing" in text:
            return "alive"
        if "[wm] launched Browser" in text:
            return "relaunched"
        if watcher.death.is_set():
            return None
        time.sleep(0.4)
    return None


# ----------------------------------------------------------------- scenarios

def sc_idle(ui, base, watcher, budget, rec):
    """CONTROL: boot, browser, one blank tab, nothing. If THIS dies, every
    other scenario's death is suspect as QEMU/host noise, not the browser."""
    ui.goto(*PARK)
    t0 = time.time()
    while time.time() - t0 < budget and not watcher.death.is_set():
        time.sleep(2.0)
    return "idled %.0fs" % (time.time() - t0)


def sc_canvas(ui, base, watcher, budget, rec, iters=24):
    """2000x2000 canvases in a loop -- 16 MiB of surface per iteration plus
    the ~2.1 MB toDataURL encode every 8th, the shapes FingerprintJS made
    ordinary. Landing today (268e92bac); its allocation path checks malloc,
    so the question is the LOOP, not one call."""
    url = base + "/canvas-alloc.html?iters=%d" % iters
    rec["url"] = url
    got = nav(ui, base, watcher, url)
    if got != url:
        return "the URL never arrived intact (%r)" % got
    return _wait_marker(watcher, "CH-CANVAS-DONE", budget, "CH-CANVAS-ITER")


def sc_dom(ui, base, watcher, budget, rec, nodes=80000):
    url = base + "/dom-growth.html?nodes=%d" % nodes
    rec["url"] = url
    got = nav(ui, base, watcher, url)
    if got != url:
        return "the URL never arrived intact (%r)" % got
    return _wait_marker(watcher, "CH-DOM-DONE", budget, "CH-DOM-ITER")


def sc_heap(ui, base, watcher, budget, rec, mib=640):
    url = base + "/js-heap.html?mib=%d" % mib
    rec["url"] = url
    got = nav(ui, base, watcher, url)
    if got != url:
        return "the URL never arrived intact (%r)" % got
    return _wait_marker(watcher, "CH-HEAP-DONE", budget, "CH-HEAP-ITER")


def sc_docwrite(ui, base, watcher, budget, rec, rounds=300):
    """document.write edge shapes on a loop -- the revival code landed TODAY
    (48a2ebd96) and its edge shapes (nested writes, after-load writes, writes
    that insert scripts) are exactly the fresh paths a hunt owes a pass over."""
    url = base + "/docwrite-edge.html?rounds=%d" % rounds
    rec["url"] = url
    got = nav(ui, base, watcher, url)
    if got != url:
        return "the URL never arrived intact (%r)" % got
    return _wait_marker(watcher, "CH-DW-DONE", budget, "CH-DW-ITER")


def sc_spin(ui, base, watcher, budget, rec, rounds=0):
    """The 45-second demonstration: a script that honestly spins 120 s in one
    entry. Expected: the js_page watchdog bites at 45 s (JS_SLICE_MS_DEFAULT,
    js_page.c:240) with `[js] watchdog: ... -- interrupted`, the entry aborts
    as an uncatchable InternalError, and THE BROWSER SURVIVES -- the page's
    own timers keep firing (CH-SPIN-ALIVE-AFTER) and the ping answers."""
    url = base + "/spin-45s.html"
    rec["url"] = url
    got = nav(ui, base, watcher, url)
    if got != url:
        return "the URL never arrived intact (%r)" % got
    return _wait_marker(watcher, "CH-SPIN-ALIVE-AFTER", budget, "CH-SPIN")


def sc_map(ui, base, watcher, budget, rec, rounds=400):
    """The owner's real crash shape (rip=js_map_get+0x90, cr2=0x10): Maps +
    WeakMaps + canvas churn + GC pressure + DOM reads in one timer-paced
    loop. See map-churn.html's header for why each ingredient is there."""
    url = base + "/map-churn.html?rounds=%d" % rounds
    rec["url"] = url
    got = nav(ui, base, watcher, url)
    if got != url:
        return "the URL never arrived intact (%r)" % got
    return _wait_marker(watcher, "CH-MAP-DONE", budget, "CH-MAP-ITER")


def sc_nav(ui, base, watcher, budget, rec, rounds=10):
    """Navigation churn driven from OUTSIDE: alternate two documents in one
    tab (full teardown+parse each time), then a tab block -- 6 tabs opened by
    Ctrl+T, cycled by Ctrl+Tab and Ctrl+1..6 (each switch is a
    dehydrate+hydrate of the singleton engine), closed by Ctrl+W. The tab
    block is browser.c's own keyboard path, no pixel coordinates."""
    a = base + "/nav-a.html"
    b = base + "/nav-b.html"
    t0 = time.time()
    for i in range(rounds):
        if watcher.death.is_set() or time.time() - t0 > budget:
            break
        got = nav(ui, base, watcher, a if i % 2 == 0 else b,
                  wait_load=45.0, settle=1.5)
        rec.setdefault("navs", []).append(got)
    # the tab block: 6 tabs, cycle, close
    for _ in range(6):
        if watcher.death.is_set():
            break
        ctrl(ui, "t")
        time.sleep(1.0)
    for cyc in range(2):
        for k in range(1, 7):
            if watcher.death.is_set():
                break
            ctrl(ui, str(k))
            time.sleep(2.0)
    for _ in range(6):
        if watcher.death.is_set():
            break
        ctrl(ui, "w")
        time.sleep(1.5)
    return "%d navigations + tab block done" % len(rec.get("navs", []))


def sc_specimen(ui, base, watcher, budget, rec, page=None):
    """Serve any saved document and DWELL on it -- the long-session shape the
    scoreboard never measures (its dwell is seconds; a user's is minutes)."""
    path = page
    with open(path, "rb") as fh:
        doc = fh.read()
    rec["specimen_bytes"] = len(doc)
    rec["url"] = base + "/specimen.html"
    got = nav(ui, base, watcher, rec["url"], wait_load=120.0, settle=8.0)
    if got != rec["url"]:
        return "the URL never arrived intact (%r)" % got
    t0 = time.time()
    while time.time() - t0 < budget and not watcher.death.is_set():
        time.sleep(2.0)
    return "dwelled %.0fs" % (time.time() - t0)


def _wait_marker(watcher, done, budget, progress):
    """Wait for the page's own DONE marker, recording its progress markers.
    The page's console.log lines are bare on the serial (js_page.c con_out has
    no prefix), so the markers are grepped as whole words."""
    t0 = time.time()
    last_note = 0
    while time.time() - t0 < budget:
        if watcher.death.is_set():
            return "DEATH while waiting for %s" % done
        try:
            with open(watcher.path, "rb") as fh:
                text = fh.read().decode("utf-8", "replace")
        except OSError:
            text = ""
        if done in text:
            return done
        n = text.count(progress)
        if n != last_note:
            last_note = n
        time.sleep(1.0)
    return "%s never appeared (%d %s markers)" % (done, last_note, progress)


SCENARIOS = {
    "idle": sc_idle,
    "canvas": sc_canvas,
    "dom": sc_dom,
    "heap": sc_heap,
    "docwrite": sc_docwrite,
    "nav": sc_nav,
    "specimen": sc_specimen,
    "spin": sc_spin,
    "map": sc_map,
}


# ----------------------------------------------------------------- the pages

def load_pages():
    pages = {}
    for name in ("canvas-alloc.html", "dom-growth.html", "js-heap.html",
                 "docwrite-edge.html", "nav-a.html", "nav-b.html",
                 "spin-45s.html", "map-churn.html"):
        p = os.path.join(FIXDIR, name)
        if os.path.exists(p):
            pages["/" + name] = open(p, "rb").read()
    return pages


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--scenario", required=True, choices=sorted(SCENARIOS))
    ap.add_argument("--out", required=True)
    ap.add_argument("--budget", type=float, default=300.0,
                    help="seconds the pattern runs after the page loads")
    ap.add_argument("--mem", default="1G",
                    help="guest RAM, `make run`'s 1G by default -- the owner's "
                         "interactive size, NOT the older drivers' 512M")
    ap.add_argument("--mode", default="1920x1200",
                    help="WxH, `make run`'s mode by default")
    ap.add_argument("--iters", type=int, default=24)
    ap.add_argument("--nodes", type=int, default=80000)
    ap.add_argument("--mib", type=int, default=640)
    ap.add_argument("--rounds", type=int, default=300)
    ap.add_argument("--page", default=None, help="specimen: a .html file to serve")
    ap.add_argument("--serial", default=None, help="where to keep the serial log")
    args = ap.parse_args()

    mw, mh = (int(v) for v in args.mode.split("x"))
    qmp_ui.configure(mw, mh)

    pages = load_pages()
    if args.scenario == "specimen":
        if not args.page or not os.path.exists(args.page):
            _harness_out(args, "HARNESS", "specimen scenario needs --page")
            return 0
        pages["/specimen.html"] = open(args.page, "rb").read()
    missing = {"canvas": "/canvas-alloc.html", "dom": "/dom-growth.html",
               "heap": "/js-heap.html", "docwrite": "/docwrite-edge.html",
               "nav": "/nav-a.html", "spin": "/spin-45s.html",
               "map": "/map-churn.html"}.get(args.scenario)
    if missing and missing not in pages:
        _harness_out(args, "HARNESS", "fixture %s missing under %s"
                     % (missing, FIXDIR))
        return 0

    token = "%d" % (os.getpid() & 0xFFFF)
    selftest = (SELFTEST % token).encode()

    class Serve(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def _send(self, body, code=200):
            self.send_response(code)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except OSError:
                pass

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/sb.html":
                self._send(selftest)
            elif path in pages:
                self._send(pages[path])
            else:
                self._send(b"<!doctype html>not here", 404)

        def log_message(self, *_a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Serve)
    port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://10.0.2.2:%d" % port

    tmp = tempfile.mkdtemp(prefix="crashhunt_%s_" % args.scenario)
    qmp_path = os.path.join(tmp, "qmp.sock")
    # QEMU always writes ITS serial inside tmp; --serial names only the COPY
    # made at exit. They were the same path once and the copy truncated the
    # file it was about to read (open(dst,"w") runs before serial() is
    # evaluated), destroying the whole boot's log with a green verdict on
    # top -- the watcher's own fd kept the data, but the record's evidence
    # was gone. Never alias the two.
    serial_path = os.path.join(tmp, "serial.log")

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    cmd = [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", args.mem, "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (mw, mh),
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]

    rec = {"scenario": args.scenario, "mem": args.mem, "mode": args.mode,
           "budget_s": args.budget, "verdict": "HARNESS", "why": "did not run",
           "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "serial_log": serial_path, "base": base,
           "params": {k: v for k, v in vars(args).items()
                      if k not in ("iso", "disk", "out", "serial")}}

    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            rec["why"] = "the %s (%s) is missing or empty" % (what, p)
            _write(rec, args.out)
            return 0

    # QEMU's own output kept: a missing disk.img once read as "the kernel never
    # printed LOGIT_BOOT_OK" across a whole corpus (qmp_site.py's own scar).
    qlog = open(os.path.join(tmp, "qemu.log"), "wb")
    proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)
    watcher = Watcher(serial_path, proc)

    def serial(frm=0):
        try:
            with open(serial_path, "rb") as fh:
                fh.seek(frm)
                return fh.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def wait_for(needle, secs):
        end = time.time() + secs
        while time.time() < end:
            if needle in serial():
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def finish(verdict, why, note=None):
        rec["verdict"] = verdict
        rec["why"] = why
        if note:
            rec["scenario_note"] = note
        rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        rec["guest"] = watcher.snapshot()
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        watcher.stop.set()
        # the full serial, beside the record, because a verdict without the
        # log that produced it cannot be argued with. Read BEFORE opening the
        # destination (see the serial_path comment for the aliasing bug).
        dst = args.serial or os.path.join(
            os.path.dirname(os.path.abspath(args.out)),
            "%s.%s.serial.txt" % (args.scenario, time.strftime("%H%M%S")))
        body = serial()
        try:
            with open(dst, "w", encoding="utf-8", errors="replace") as fh:
                fh.write(body)
            rec["serial_log"] = dst
        except OSError:
            pass
        _write(rec, args.out)
        print(json.dumps({"scenario": args.scenario, "verdict": verdict,
                          "why": why,
                          "guest_uptime_s": rec["guest"]["guest_uptime_s"]},
                         ensure_ascii=False))
        return 0

    try:
        if not wait_for("LOGIT_BOOT_OK", BOOT_BUDGET):
            finish("HARNESS", "the kernel never printed LOGIT_BOOT_OK")
            return 0
        if not wait_for("desktop live", DESKTOP_BUDGET):
            finish("HARNESS", "the window manager never brought the desktop up")
            return 0
        time.sleep(3)

        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            finish("HARNESS", str(e))
            return 0
        time.sleep(7)

        # the self-test: prove keyboard/Ctrl+T/address bar/network/JS before
        # anything is claimed about a scenario (qmp_site.py's discipline)
        mark = len(serial())
        ctrl(ui, "t")
        ui.typ(base + "/sb.html")
        ui.key("ret")
        end = time.time() + SELFTEST_BUDGET
        while time.time() < end and "CH-READY-" + token not in serial(mark):
            if watcher.death.is_set():
                break
            time.sleep(0.5)
        if "CH-READY-" + token not in serial(mark):
            finish("HARNESS", "the self-test page never loaded -- nothing was "
                              "measured about the scenario")
            return 0
        rec["selftest_ok"] = True

        ui.goto(*PARK)
        time.sleep(1.0)

        fn = SCENARIOS[args.scenario]
        kw = {}
        if args.scenario == "canvas":
            kw = {"iters": args.iters}
        elif args.scenario == "dom":
            kw = {"nodes": args.nodes}
        elif args.scenario == "heap":
            kw = {"mib": args.mib}
        elif args.scenario in ("docwrite", "nav", "map"):
            kw = {"rounds": args.rounds}
        if args.scenario == "specimen":
            note = fn(ui, base, watcher, args.budget, rec, page=args.page)
        else:
            note = fn(ui, base, watcher, args.budget, rec, **kw)

        time.sleep(2.0)

        # ---- the verdict ladder: machine first, then the browser ----
        g = watcher.snapshot()
        if g["panics"]:
            finish("KERNEL-PANIC", "; ".join(g["panics"][:2]), note)
            return 0
        if watcher.qemu_exit_code is not None:
            finish("QEMU-EXIT", "QEMU exited rc=%s before the verdict ladder"
                   % watcher.qemu_exit_code, note)
            return 0
        if g["faults"]:
            finish("BROWSER-DIED-FAULT", g["faults"][0], note)
            return 0
        browser_oom = [l for l in g["oom"] if "victim:" in l and "browser" in l]
        if browser_oom:
            finish("BROWSER-DIED-OOM", browser_oom[0], note)
            return 0

        # alive? ping first (non-destructive), dock click as the referee
        answered = ping(ui, watcher)
        if answered:
            finish("SURVIVED", "the about:text ping answered after the "
                              "scenario", note)
            return 0
        g = watcher.snapshot()
        if g["faults"]:
            finish("BROWSER-DIED-FAULT", g["faults"][0], note)
            return 0
        ref = referee(ui, watcher)
        g = watcher.snapshot()
        if g["faults"]:
            finish("BROWSER-DIED-FAULT", g["faults"][0], note)
            return 0
        if ref == "alive":
            finish("ALIVE-WEDGED", "the window is alive (WM says already "
                   "live) but the about:text ping went unanswered -- a busy "
                   "or wedged browser, not a death", note)
        elif ref == "relaunched":
            finish("BROWSER-DIED-SILENT", "the dock click started a FRESH "
                   "Browser -- the previous instance exited with no fault, "
                   "oom or panic line on the serial", note)
        else:
            finish("HARNESS", "neither liveness referee answered (WM printed "
                              "nothing for the dock click)", note)
        return 0
    except SystemExit:
        raise
    except Exception as e:                                        # noqa: BLE001
        import traceback
        traceback.print_exc()
        rec["traceback"] = traceback.format_exc()
        finish("HARNESS", "driver error: %r" % (e,))
        return 0


def _harness_out(args, verdict, why):
    rec = {"scenario": args.scenario, "verdict": verdict, "why": why,
           "started": time.strftime("%Y-%m-%dT%H:%M:%S")}
    _write(rec, args.out)
    print(json.dumps({"scenario": args.scenario, "verdict": verdict,
                      "why": why}))


def _write(rec, out):
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(rec, fh, indent=1, ensure_ascii=False)


if __name__ == "__main__":
    sys.exit(main())
