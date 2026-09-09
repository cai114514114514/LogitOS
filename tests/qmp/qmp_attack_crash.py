#!/usr/bin/env python3
"""CRASH CENSUS: one real site, one boot, the owner's interactive shape,
and every word the machine said about how the browser died.

    python3 tests/qmp/qmp_attack_crash.py --iso build-atk-crash/logit.iso \
        --disk build-atk-crash/disk.img --name bing --url https://www.bing.com/ \
        --out build-atk-crash/crash/bing.json

WHAT THIS IS, AND WHAT IT IS NOT
================================
An INSTRUMENT in qmp_crashhunt.py's sense: it never fixes, it asserts nothing
about the browser, and its exit code is about the harness. It exists because
the owner's verdict -- "any heavy site explodes immediately" -- is a sentence,
and tests/scoreboard/ has never recorded a single `[fault]` or `[core]` line
(grep over every archived serial.txt: zero), because the scoreboard dwells for
SECONDS at 512M/1280x800 and the owner sits for MINUTES at 1G/1920x1200. The
crashhunt driver measures synthetic stress pages; nothing measures the real
sites at the owner's size with the owner's dwell. This does exactly that, so a
"crash" becomes a faulting rip resolvable against THIS build's browser.elf.

THE SHAPE PER SITE (one QEMU per site, never two sites in one boot):
  boot -> desktop -> launch Browser -> local self-test page (proves keyboard,
  Ctrl+T, address bar, SLIRP, HTML, JS before anything is claimed about the
  site) -> Ctrl+T, type the URL, Enter, confirm `[browser] load: <url>` ->
  wait for `[browser] load done` (or death) -> idle 45 s ON THE GUEST CLOCK ->
  one wheel scroll at the page centre -> one click at the page centre ->
  idle 15 s on the guest clock -> the verdict ladder -> liveness ping ->
  dock re-click referee.

GUEST CLOCK, NOT HOST CLOCK (AGENTS.md section 5). The wm prints
`[wm] perf t=<ms>` once per second of ACTIVITY and stays silent when idle, so
during the "idle" phases the pointer is wiggled one pixel over the MENU BAR
(never over the window: the page receives nothing) purely to keep that line
coming. `cat /dev/kstat` typed into the serial-console shell gives a second
clock (`uptime_ms`) AND `mem_free_bytes`, and is taken at phase boundaries so
"explodes" can be told from "ran out of memory". A hand-wave that neither clock
advanced for HANG_HOST_S host seconds is the GUEST-HANG verdict: the machine,
not the browser, stopped answering, and there is by definition no line for it.

DEATH LINES (established from the kernel sources before the first boot):
  `[core] pid N (Browser) sig S -> /core.K ... rip=... rsp=... cr2=...`
      coredump.c:674 -- printed BEFORE the [fault] line, registers read back
      out of the file.
  `[fault] app exception: NAME (vector V) rip=... err=... cr2=... rsp=...`
      interrupts.c:244 -- a ring-3 fault killed the app.
  `[oom] victim: pid N "Browser" ...`      oom.c:351 -- OOM-killed.
  `[js] watchdog: ...` / `[watchdog] ...`  js_page.c:539 -- a SCRIPT was
      interrupted; the browser survives. Pressure, not death.
  `LOGIT_PANIC`                            the kernel died.
  nothing + a dock re-click that LAUNCHES a fresh Browser -- silent exit.

THE CORE FILE IS KEPT. The disk is an APFS clone per site (cp -c, instant),
booted WITHOUT -snapshot, so /core.K survives the boot and is pulled off the
image with tests/boot/lfs_extract.py. That file holds the register file and
the run containing rsp (coredump.c:386 puts it first), which is what makes a
BACKTRACE possible on the host by scanning the stack for browser.elf text
addresses -- the kernel prints a rip, and a rip without callers cannot tell
two crashes in one helper apart. The known js_map_get+0x90 crash is exactly
that question.

NO SOURCE EDITS. This file only reads what the machine already prints.
"""

import argparse
import http.server
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui                                                     # noqa: E402
from qmp_ui import Session                                        # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
LFS_EXTRACT = os.path.join(os.path.dirname(HERE), "boot", "lfs_extract.py")

BOOT_BUDGET = float(os.environ.get("AC_BOOT", 300))
DESKTOP_BUDGET = 120.0
SELFTEST_BUDGET = float(os.environ.get("AC_SELFTEST", 90))
LOAD_BUDGET = float(os.environ.get("AC_LOAD", 420))     # host cap on `load done`
PING_WAIT = 14.0
RELAUNCH_WAIT = 45.0
HANG_HOST_S = 120.0        # neither guest clock moved for this long -> GUEST-HANG
PARK = (40, 40)            # over the menu bar: no window, no dock
WIGGLE = ((40, 40), (41, 40))

UA = os.environ.get("SITE_UA", "Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

SELFTEST = ("<!doctype html><html><head><title>sb</title></head>"
            "<body style='background:#ffffff'><div style='font-size:28px'>READY</div>"
            "<script>console.log('AC-READY-%s');</script></body></html>")

# ------------------------------------------------------------------ patterns
FAULT_RE = re.compile(r"\[fault\] app exception: (\S+) \(vector (\d+)\) rip=(0x[0-9a-fA-F]+) "
                      r"err=([0-9a-fA-F]+) cr2=(0x[0-9a-fA-F]+) rsp=(0x[0-9a-fA-F]+)")
CORE_RE = re.compile(r"\[core\] pid (\d+) \(([^)]*)\) sig (\d+) -> (/core\.\d) (\d+) bytes: "
                     r"regions (\d+)/(\d+) bytes (\d+)/(\d+) rip=(0x[0-9a-fA-F]+) "
                     r"rsp=(0x[0-9a-fA-F]+) cr2=(0x[0-9a-fA-F]+) err=([0-9a-fA-F]+)(.*)")
WM_PERF_RE = re.compile(r"\[wm\] perf t=(\d+)")
# `[time] tickloss ... uptime Ns` -- printed once a second exactly when the
# host is starving the guest, which is exactly when a third clock is needed.
UPTIME_RE = re.compile(r"uptime (\d+)s")
WM_WIN_RE = re.compile(r"\[wm\] win (\d+) frame (-?\d+) (-?\d+) (\d+) (\d+) content (\d+) (\d+) pt "
                       r"zoom (\d+) min (\d+) (.*)$")
KSTAT_UP_RE = re.compile(r"uptime_ms\s+(\d+)")
KSTAT_FREE_RE = re.compile(r"mem_free_bytes\s+(\d+)")
KSTAT_TOTAL_RE = re.compile(r"mem_total_bytes\s+(\d+)")
HEAP_RE = re.compile(r"\[browser\] heap peak (\d+)K")
MMLOW_RE = re.compile(r"\[mm\] low: (\d+) frames free \((\d+) MiB\)")
WATCHDOG_RE = re.compile(r"\[js\] watchdog[^\n]*|\[worker \d+\] watchdog[^\n]*|\[watchdog\][^\n]*")
PANIC_RE = re.compile(r"LOGIT_PANIC|Kernel panic|double fault|triple fault", re.I)
EXC_RE = re.compile(r"\[browser\] JS exception: (.*)$")
LOADDONE_RE = re.compile(r"\[browser\] load done: (\d+) requests, (\d+) connections dialled, "
                         r"(\d+) reused, (\d+) modules loaded \((\d+) failed\)")
LOAD_RE = re.compile(r"\[browser\] load: (\S+)")
PROCKILL_RE = re.compile(r"\[proc\] kill: pid (\d+) (marked|exiting)")


# ------------------------------------------------------------- serial pump
#
# The serial is a SOCKET, not a file, because it is written to as well as read:
# the serial console is /bin/login -> /bin/sh (wm.c:5907), and `cat /dev/kstat`
# typed into it is the independent memory reading. `wait=on` makes QEMU hold
# the CPU until we are connected, so no boot byte is lost.

class Serial:
    def __init__(self, path, proc, mirror):
        self.path = path
        self.proc = proc
        self.mirror = mirror              # a file the log is mirrored into
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.sock = None
        self.stop = threading.Event()
        self._connect()
        self.mf = open(mirror, "wb")
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _connect(self):
        s = socket.socket(socket.AF_UNIX)
        for _ in range(600):
            try:
                s.connect(self.path)
                self.sock = s
                return
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError("qemu died before the serial socket appeared")
                time.sleep(0.1)
        raise RuntimeError("serial socket never appeared")

    def _run(self):
        self.sock.settimeout(0.25)
        while not self.stop.is_set():
            try:
                b = self.sock.recv(65536)
                if not b:
                    time.sleep(0.1)
                    continue
            except socket.timeout:
                continue
            except OSError:
                time.sleep(0.1)
                continue
            with self.lock:
                self.buf.extend(b)
                try:
                    self.mf.write(b)
                    self.mf.flush()
                except OSError:
                    pass

    def text(self, frm=0):
        with self.lock:
            return self.buf[frm:].decode("utf-8", "replace")

    def size(self):
        with self.lock:
            return len(self.buf)

    def send(self, s):
        try:
            self.sock.sendall(s.encode())
        except OSError:
            pass


# ------------------------------------------------------------------ watcher

class Watcher:
    """Everything the guest said, triaged as it lands (a thread: a death line
    can arrive between two keystrokes of the very ping about to test for it)."""

    def __init__(self, serial):
        self.ser = serial
        self.pos = 0
        self.stop = threading.Event()
        self.death = threading.Event()
        self.lock = threading.Lock()
        self.faults = []
        self.cores = []
        self.oom = []
        self.panics = []
        self.watchdogs = []
        self.mm_low = []
        self.heap_peaks = []
        self.exceptions = []
        self.loads = []               # (guest_ms, url)
        self.load_done = []           # (guest_ms, line)
        self.prockills = []
        self.kstat = []               # dicts {uptime_ms, free, total, tag}
        self.wins = {}                # title -> dict
        self.guest_ms = 0             # max of wm perf t=, kstat uptime_ms, tickloss uptime
        self.last_clock_host = time.time()
        self.ref_host = None          # set by main at LOGIT_BOOT_OK
        self.ref_host2 = None         # host time of the first guest tick after that
        self.ref_guest = None
        self.boot_host_s = None       # host seconds QEMU start -> LOGIT_BOOT_OK
        self.qemu_exit_code = None
        self.pending_kstat_tag = None
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        carry = ""
        while not self.stop.is_set():
            n = self.ser.size()
            if n > self.pos:
                chunk = self.ser.text(self.pos)
                self.pos = n
                text = carry + chunk
                lines = text.split("\n")
                carry = lines.pop()          # an unterminated tail waits
                for ln in lines:
                    self._line(ln.rstrip("\r"))
            rc = self.ser.proc.poll()
            if rc is not None and self.qemu_exit_code is None:
                self.qemu_exit_code = rc
            time.sleep(0.2)

    def _tick(self, ms):
        if ms > self.guest_ms:
            self.guest_ms = ms
            self.last_clock_host = time.time()
            if self.ref_guest is None and self.ref_host is not None:
                self.ref_guest = ms
                self.ref_host2 = time.time()

    def slow(self):
        """HOST SECONDS PER GUEST SECOND, measured, so every host-side budget
        below is a guest budget in disguise. On an idle host this is 1.0; on
        the host this census ran on (load average 25, ten sibling QEMUs) the
        guest reached 7.7 s of uptime in 90 host seconds -- a factor of ~12 --
        and a 90 s launch timeout filed a healthy machine as 'the click hit
        nothing'. Until the guest clock has moved 2 s after LOGIT_BOOT_OK the
        boot itself is the estimate: host seconds to BOOT_OK over a nominal
        uncontended 12 s."""
        now = time.time()
        if self.ref_guest is not None and self.guest_ms - self.ref_guest >= 2000:
            f = (now - self.ref_host2) / ((self.guest_ms - self.ref_guest) / 1000.0)
        elif self.boot_host_s is not None:
            f = self.boot_host_s / 12.0
        else:
            f = 1.0
        return max(1.0, min(15.0, f))

    def _line(self, ln):
        m = WM_PERF_RE.search(ln)
        if m:
            self._tick(int(m.group(1)))
        m = UPTIME_RE.search(ln)
        if m:
            self._tick(int(m.group(1)) * 1000)
        m = KSTAT_UP_RE.search(ln)
        if m:
            self._tick(int(m.group(1)))
            with self.lock:
                self.kstat.append({"uptime_ms": int(m.group(1)), "tag": self.pending_kstat_tag})
        m = KSTAT_TOTAL_RE.search(ln)
        if m and self.kstat:
            with self.lock:
                self.kstat[-1]["total"] = int(m.group(1))
        m = KSTAT_FREE_RE.search(ln)
        if m and self.kstat:
            with self.lock:
                self.kstat[-1]["free"] = int(m.group(1))
        m = WM_WIN_RE.search(ln)
        if m:
            with self.lock:
                self.wins[m.group(10).strip()] = {
                    "idx": int(m.group(1)), "x": int(m.group(2)), "y": int(m.group(3)),
                    "w": int(m.group(4)), "h": int(m.group(5)),
                    "cw": int(m.group(6)), "ch": int(m.group(7))}
        m = HEAP_RE.search(ln)
        if m:
            with self.lock:
                self.heap_peaks.append((self.guest_ms, int(m.group(1))))
        m = MMLOW_RE.search(ln)
        if m:
            with self.lock:
                self.mm_low.append((self.guest_ms, int(m.group(1)), int(m.group(2))))
        m = EXC_RE.search(ln)
        if m:
            with self.lock:
                self.exceptions.append(m.group(1).strip()[:200])
        m = LOAD_RE.search(ln)
        if m and "load done" not in ln:
            with self.lock:
                self.loads.append((self.guest_ms, m.group(1)))
        if LOADDONE_RE.search(ln):
            with self.lock:
                self.load_done.append((self.guest_ms, ln.strip()))
        m = PROCKILL_RE.search(ln)
        if m:
            with self.lock:
                self.prockills.append(ln.strip())
        if WATCHDOG_RE.search(ln):
            with self.lock:
                self.watchdogs.append((self.guest_ms, ln.strip()))
        if PANIC_RE.search(ln):
            with self.lock:
                self.panics.append(ln.strip())
            self.death.set()
        if "[core] " in ln:
            with self.lock:
                self.cores.append((self.guest_ms, ln.strip()))
        if "[fault] app exception" in ln:
            with self.lock:
                self.faults.append((self.guest_ms, ln.strip()))
            self.death.set()
        if ln.startswith("[oom]"):
            with self.lock:
                self.oom.append((self.guest_ms, ln.strip()))
            # only a VICTIM mark is a death (crashhunt's rule); refusals and
            # reaps are pressure
            if "victim:" in ln:
                self.death.set()

    def snapshot(self):
        with self.lock:
            return {
                "faults": list(self.faults), "cores": list(self.cores),
                "oom": list(self.oom), "panics": list(self.panics),
                "watchdogs": list(self.watchdogs), "mm_low": list(self.mm_low),
                "heap_peaks": list(self.heap_peaks),
                "heap_peak_k_max": max([h[1] for h in self.heap_peaks] or [0]) or None,
                "js_exceptions": len(self.exceptions),
                "js_exception_samples": self.exceptions[:12],
                "loads": list(self.loads), "load_done": list(self.load_done),
                "prockills": list(self.prockills), "kstat": list(self.kstat),
                "wins": dict(self.wins), "guest_ms": self.guest_ms,
                "qemu_exit_code": self.qemu_exit_code}


# ---------------------------------------------------------------- host side

def host_probe(url, result, chrome_dir):
    """The same URL from the host: (a) urllib with the guest's own UA -- is the
    site up, what did it serve, did it REFUSE us; (b) headless Chrome's DOM
    after a virtual-time budget -- the independent 'how heavy is this page'
    oracle (nodes, scripts, bytes), so a crash can be ranked against weight
    rather than against a name."""
    import urllib.request
    import urllib.error
    import gzip
    import io
    t0 = time.time()
    try:
        hdrs = {"Accept": "text/html,application/xhtml+xml,*/*",
                "Accept-Encoding": "gzip, identity", "User-Agent": UA}
        req = urllib.request.Request(url, headers=hdrs)
        with urllib.request.urlopen(req, timeout=45) as r:
            raw = r.read(4 << 20)
            if r.headers.get("Content-Encoding", "") == "gzip":
                try:
                    raw = gzip.GzipFile(fileobj=io.BytesIO(raw)).read()
                except OSError:
                    pass
            body = raw.decode("utf-8", "replace")
            result.update(ok=True, status=r.status, final_url=r.geturl(), bytes=len(raw),
                          script_tags=len(re.findall(r"<script\b", body, re.I)),
                          script_src=len(re.findall(r"<script\b[^>]*\bsrc=", body, re.I)),
                          elapsed=round(time.time() - t0, 1))
    except urllib.error.HTTPError as e:
        raw = b""
        try:
            raw = e.read()
        except Exception:                                         # noqa: BLE001
            pass
        result.update(ok=True, status=e.code, bytes=len(raw),
                      refused=(e.code in (403, 429, 503)),
                      elapsed=round(time.time() - t0, 1))
    except Exception as e:                                        # noqa: BLE001
        result.update(ok=False, error="%s: %s" % (type(e).__name__, e),
                      elapsed=round(time.time() - t0, 1))

    # headless Chrome. It writes the DOM and then (on this host) does not exit
    # -- measured 2026-09-02: 561 bytes of example.com on stdout and the
    # process still alive at 45 s -- so the read is bounded and the process
    # is killed; whatever it wrote is the measurement.
    ch = {"ok": False}
    result["chrome"] = ch
    if not os.path.exists(CHROME) or os.environ.get("AC_NO_CHROME"):
        ch["why"] = "chrome absent or AC_NO_CHROME set"
        return
    prof = os.path.join(chrome_dir, "prof-%d" % os.getpid())
    shutil.rmtree(prof, ignore_errors=True)
    cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
           "--user-data-dir=" + prof, "--timeout=30000", "--virtual-time-budget=8000",
           "--user-agent=" + UA, "--dump-dom", url]
    t1 = time.time()
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            out, _ = p.communicate(timeout=75)
        except subprocess.TimeoutExpired:
            p.kill()
            out, _ = p.communicate()
        dom = out.decode("utf-8", "replace")
        ch.update(ok=len(dom) > 0, bytes=len(dom),
                  elements=len(re.findall(r"<[a-zA-Z][^>]*>", dom)),
                  script_tags=len(re.findall(r"<script\b", dom, re.I)),
                  title=(re.search(r"<title[^>]*>(.*?)</title>", dom, re.S | re.I) or [None, ""])[1][:80]
                  if re.search(r"<title[^>]*>(.*?)</title>", dom, re.S | re.I) else "",
                  elapsed=round(time.time() - t1, 1))
    except Exception as e:                                        # noqa: BLE001
        ch.update(ok=False, why="%s: %s" % (type(e).__name__, e))
    finally:
        subprocess.call(["pkill", "-f", "user-data-dir=" + prof], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)
        shutil.rmtree(prof, ignore_errors=True)


# ------------------------------------------------------------------ plumbing

def ctrl(ui, ch):
    ui.key_mods(("ctrl",), ch, settle=0.25)


def typ(ui, text, settle=0.16):
    """qmp_ui.typ, PACED HARDER. The emulated PS/2 controller has a one-byte
    buffer, and on a host running ten QEMUs (load average 25 when this was
    measured, 34% guest tick loss) the guest does not drain it inside
    qmp_ui's 50 ms: the first census pass typed `sb.hml` on one site and
    NOTHING on the next. 160 ms per key is 5 s for a URL and buys a URL that
    arrives whole; the confirmation loop that follows is still the referee."""
    for ch in text:
        if ch in qmp_ui.SHIFT:
            ui.key_mods(("shift",), qmp_ui.SHIFT[ch], settle=settle)
        elif "A" <= ch <= "Z":
            ui.key_mods(("shift",), ch.lower(), settle=settle)
        else:
            ui.key(qmp_ui.KMAP.get(ch, ch), settle=settle)


def wheel(ui, down=True):
    btn = "wheel-down" if down else "wheel-up"
    ui._input([{"type": "btn", "data": {"button": btn, "down": True}},
               {"type": "btn", "data": {"button": btn, "down": False}}])


def page_centre(watcher):
    """Centre of the browser's VIEWPORT in device pixels, from the guest's own
    `[wm] win ... Browser` line: frame in device px, content in points. The
    chrome above the page is TBH (wm.c TITLEBAR_H 30 pt) + TABH + BARH
    (browser.c 30 + 30 pt); the status line below is 18 pt."""
    w = watcher.snapshot()["wins"].get("Browser")
    if not w:
        return (qmp_ui.SCREEN_W // 2, qmp_ui.SCREEN_H // 2), None
    view_y = 60
    view_h = w["ch"] - view_y - 18
    cx = w["x"] + w["w"] // 2
    cy = w["y"] + qmp_ui.pt(30) + qmp_ui.pt(view_y + view_h // 2)
    return (cx, cy), w


def idle_guest(ui, watcher, guest_s, host_cap, rec, phase):
    """Idle `guest_s` seconds ON THE GUEST CLOCK, wiggling the pointer over the
    menu bar so the wm keeps stamping `[wm] perf t=`. Returns 'ok', 'death',
    or 'hang' (no guest clock line for HANG_HOST_S host seconds)."""
    start_ms = watcher.guest_ms
    t0 = time.time()
    i = 0
    stalled_since = time.time()
    last_ms = watcher.guest_ms
    while True:
        if watcher.death.is_set():
            rec.setdefault("phases", {})[phase] = {"guest_ms_elapsed": watcher.guest_ms - start_ms,
                                                   "host_s": round(time.time() - t0, 1),
                                                   "end": "death"}
            return "death"
        if watcher.guest_ms - start_ms >= guest_s * 1000:
            rec.setdefault("phases", {})[phase] = {"guest_ms_elapsed": watcher.guest_ms - start_ms,
                                                   "host_s": round(time.time() - t0, 1),
                                                   "end": "ok"}
            return "ok"
        if watcher.guest_ms != last_ms:
            last_ms = watcher.guest_ms
            stalled_since = time.time()
        hang_s = HANG_HOST_S * watcher.slow()
        if time.time() - stalled_since > hang_s or time.time() - t0 > host_cap * watcher.slow():
            hung = time.time() - stalled_since > hang_s
            rec.setdefault("phases", {})[phase] = {"guest_ms_elapsed": watcher.guest_ms - start_ms,
                                                   "host_s": round(time.time() - t0, 1),
                                                   "slow": round(watcher.slow(), 2),
                                                   "end": "hang" if hung else "host-cap"}
            return "hang" if hung else "ok"
        ui.goto(*WIGGLE[i % 2], settle=0.0)
        i += 1
        time.sleep(1.0)


def kstat(ser, watcher, tag, wait=8.0):
    """`cat /dev/kstat` on the serial shell. The parse happens in the watcher;
    this only waits for the uptime_ms line to land so the sample is attributed
    to `tag`."""
    n0 = len(watcher.kstat)
    watcher.pending_kstat_tag = tag
    ser.send("cat /dev/kstat\n")
    end = time.time() + wait
    while time.time() < end:
        if len(watcher.kstat) > n0:
            time.sleep(0.5)          # the free/total lines follow uptime
            return watcher.kstat[-1]
        time.sleep(0.2)
    return None


def wait_text(ser, needle, secs, frm=0, watcher=None):
    end = time.time() + secs
    while time.time() < end:
        if needle in ser.text(frm):
            return True
        if ser.proc.poll() is not None:
            return False
        if watcher is not None and watcher.death.is_set():
            return False
        time.sleep(0.4)
    return False


def ping(ui, ser, watcher):
    mark = ser.size()
    ctrl(ui, "l")
    typ(ui, "about:text")
    ui.key("ret")
    end = time.time() + PING_WAIT * watcher.slow()
    while time.time() < end:
        if "[browser] load: about:text" in ser.text(mark):
            return True
        if watcher.death.is_set():
            return False
        time.sleep(0.4)
    return False


def referee(ui, ser, watcher):
    mark = ser.size()
    dock = ui.dock()
    x, y = qmp_ui.dock_icon_of("browser", dock)
    ui.click_at(x, y)
    end = time.time() + RELAUNCH_WAIT * watcher.slow()
    while time.time() < end:
        t = ser.text(mark)
        if "[wm] launch: already live, focusing" in t:
            return "alive"
        if "[wm] launched Browser" in t:
            return "relaunched"
        if watcher.death.is_set():
            return None
        time.sleep(0.4)
    return None


def _write(rec, out):
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(rec, fh, indent=1, ensure_ascii=False)


# ---------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True, help="the PRISTINE image; cloned per run")
    ap.add_argument("--name", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--mode", default="1920x1200")
    ap.add_argument("--idle1", type=float, default=45.0, help="guest seconds after load")
    ap.add_argument("--idle2", type=float, default=15.0, help="guest seconds after the click")
    ap.add_argument("--keep-disk", action="store_true", help="keep the per-run disk clone")
    args = ap.parse_args()

    mw, mh = (int(v) for v in args.mode.split("x"))
    qmp_ui.configure(mw, mh)

    outdir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(outdir, exist_ok=True)
    rec = {"name": args.name, "url": args.url, "mem": args.mem, "mode": args.mode,
           "idle1_s": args.idle1, "idle2_s": args.idle2,
           "verdict": "HARNESS", "why": "did not run",
           "started": time.strftime("%Y-%m-%dT%H:%M:%S"), "host": {}}

    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            rec["why"] = "the %s (%s) is missing or empty" % (what, p)
            _write(rec, args.out)
            print(json.dumps({"name": args.name, "verdict": "HARNESS", "why": rec["why"]}))
            return 0

    probe = {}
    rec["host"] = probe
    th = threading.Thread(target=host_probe, args=(args.url, probe, outdir), daemon=True)
    th.start()

    tmp = tempfile.mkdtemp(prefix="ac_%s_" % re.sub(r"\W+", "_", args.name))
    qmp_path = os.path.join(tmp, "qmp.sock")
    ser_path = os.path.join(tmp, "ser.sock")
    mirror = os.path.join(outdir, "%s.serial.txt" % args.name)
    rec["serial_log"] = mirror

    # THE DISK CLONE: no -snapshot, so /core.K survives to be extracted; a
    # fresh clone per run so no site inherits another's /browser/* session.
    clone = os.path.join(outdir, "%s.disk.img" % args.name)
    if os.path.exists(clone):
        os.unlink(clone)
    if subprocess.call(["cp", "-c", args.disk, clone]) != 0:
        shutil.copyfile(args.disk, clone)
    rec["disk_clone"] = clone

    token = "%d" % (os.getpid() & 0xFFFF)
    selftest = (SELFTEST % token).encode()

    class Serve(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_GET(self):
            body = selftest if self.path.split("?", 1)[0] == "/sb.html" else b"<!doctype html>not here"
            self.send_response(200 if body is selftest else 404)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except OSError:
                pass

        def log_message(self, *_a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Serve)
    port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    cmd = [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % clone,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-m", args.mem, "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (mw, mh),
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-chardev", "socket,id=ser0,path=%s,server=on,wait=on" % ser_path,
           "-serial", "chardev:ser0",
           "-qmp", "unix:%s,server,nowait" % qmp_path]
    rec["qemu_cmd"] = " ".join(cmd)
    qlog = open(os.path.join(outdir, "%s.qemu.log" % args.name), "wb")
    t_qemu = time.time()
    proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)
    try:
        ser = Serial(ser_path, proc, mirror)
    except RuntimeError as e:
        rec["why"] = str(e)
        _write(rec, args.out)
        print(json.dumps({"name": args.name, "verdict": "HARNESS", "why": rec["why"]}))
        return 0
    watcher = Watcher(ser)

    def finish(verdict, why, note=None):
        rec["verdict"] = verdict
        rec["why"] = why
        if note:
            rec["note"] = note
        rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        rec["slow_factor"] = round(watcher.slow(), 2)
        rec["guest"] = watcher.snapshot()
        th.join(timeout=60)
        # let a core write finish before the machine is pulled
        time.sleep(2.0)
        try:
            proc.kill()
            proc.wait(timeout=10)
        except Exception:                                         # noqa: BLE001
            pass
        watcher.stop.set()
        ser.stop.set()
        time.sleep(0.4)
        try:
            ser.mf.flush()
            ser.mf.close()
        except OSError:
            pass
        srv.shutdown()
        # the core file(s) named on the serial, pulled off the clone
        cores = []
        for _, ln in rec["guest"]["cores"]:
            m = CORE_RE.search(ln)
            if not m:
                continue
            path = m.group(4)
            dst = os.path.join(outdir, "%s%s" % (args.name, path.replace("/", ".")))
            try:
                r = subprocess.run([sys.executable, LFS_EXTRACT, clone, path, dst],
                                   capture_output=True, text=True, timeout=120)
                cores.append({"path": path, "out": dst if r.returncode == 0 else None,
                              "msg": (r.stdout + r.stderr).strip()[-300:]})
            except Exception as e:                                # noqa: BLE001
                cores.append({"path": path, "out": None, "msg": repr(e)})
        rec["core_files"] = cores
        if not args.keep_disk:
            try:
                os.unlink(clone)
            except OSError:
                pass
        _write(rec, args.out)
        print(json.dumps({"name": args.name, "verdict": verdict, "why": why,
                          "guest_ms": rec["guest"]["guest_ms"]}, ensure_ascii=False))
        return 0

    try:
        if not wait_text(ser, "LOGIT_BOOT_OK", BOOT_BUDGET):
            return finish("HARNESS", "the kernel never printed LOGIT_BOOT_OK")
        watcher.boot_host_s = round(time.time() - t_qemu, 1)
        watcher.ref_host = time.time()
        rec["boot_host_s"] = watcher.boot_host_s
        if not wait_text(ser, "desktop live", DESKTOP_BUDGET * watcher.slow()):
            return finish("HARNESS", "the window manager never brought the desktop up")
        time.sleep(3)

        ui = Session(qmp_path, serial_text_fn=ser.text)
        rec["slow_at_launch"] = round(watcher.slow(), 2)
        try:
            ui.launch_app("browser", timeout=90.0 * watcher.slow())
        except AssertionError as e:
            return finish("HARNESS", str(e))
        time.sleep(min(60.0, 7.0 * watcher.slow()))

        base = "http://10.0.2.2:%d" % port
        # RETRIED, and confirmed against the browser's own `[browser] load:`
        # line before the marker is waited for. The first census pass lost
        # one site to `sb.hml` -- a `t` dropped by the one-byte PS/2 buffer
        # -- and filed it as HARNESS with nothing measured. A mistyped URL is
        # the harness's; it is retyped, never scored.
        ok = False
        for attempt in range(3):
            mark = ser.size()
            ctrl(ui, "t")
            time.sleep(0.5)
            typ(ui, base + "/sb.html")
            ui.key("ret")
            if not wait_text(ser, "[browser] load: ", 30.0 * watcher.slow(), mark, watcher):
                continue
            # SUBSTRING, not line equality: the kernel interleaves its own
            # `[time] tickloss` line into the middle of `[browser] load: `
            # under host contention, so the URL is whole but not alone on
            # its line. What matters is that the typed bytes arrived intact.
            txt = ser.text(mark)
            if (base + "/sb.html") not in txt:
                got = None
                for ln in txt.splitlines():
                    i = ln.find("[browser] load: ")
                    if i >= 0:
                        got = ln[i + len("[browser] load: "):].strip()
                        break
                rec.setdefault("selftest_mistypes", []).append(got)
                continue
            if wait_text(ser, "AC-READY-" + token, SELFTEST_BUDGET * watcher.slow(), mark, watcher):
                ok = True
                break
        if not ok:
            return finish("HARNESS", "the self-test page never loaded (%r) -- nothing measured"
                          % rec.get("selftest_mistypes"))
        rec["selftest_ok"] = True
        rec["selftest_attempts"] = attempt + 1
        ui.goto(*PARK)
        kstat(ser, watcher, "after-selftest")

        # ---- the site: typed, CONFIRMED against the browser's own line ----
        typed = None
        for attempt in range(3):
            mark = ser.size()
            ctrl(ui, "t")
            time.sleep(0.5)
            typ(ui, args.url)
            t_nav = time.time()
            ui.key("ret")
            if not wait_text(ser, "[browser] load: ", 30.0 * watcher.slow(), mark, watcher):
                if watcher.death.is_set():
                    break
                continue
            txt = ser.text(mark)
            for ln in txt.splitlines():
                i = ln.find("[browser] load: ")
                if i >= 0:
                    typed = ln[i + len("[browser] load: "):].strip()
                    break
            if args.url.rstrip("/") in txt:          # substring: see the self-test note
                typed = args.url
                break
            rec.setdefault("url_mistypes", []).append(typed)
            typed = None
        if typed is None and not watcher.death.is_set():
            return finish("HARNESS", "the URL never reached the address bar intact (%r)"
                          % rec.get("url_mistypes"))
        rec["url_confirmed"] = typed
        rec["nav_guest_ms"] = watcher.guest_ms

        loaded = False
        fetch_failed = False
        while time.time() - t_nav < LOAD_BUDGET * watcher.slow() and not watcher.death.is_set():
            s = ser.text(mark)
            if "[browser] load done:" in s:
                loaded = True
                break
            if "[browser] page fetch failed" in s:
                fetch_failed = True
                break
            if proc.poll() is not None:
                return finish("HARNESS", "QEMU exited during the load")
            ui.goto(*WIGGLE[int(time.time()) % 2], settle=0.0)
            time.sleep(0.8)
        rec["load"] = {"done": loaded, "fetch_failed": fetch_failed,
                       "host_s": round(time.time() - t_nav, 1),
                       "guest_ms_at_nav": rec["nav_guest_ms"],
                       "slow": round(watcher.slow(), 2),
                       "guest_ms_at_done": watcher.guest_ms if loaded else None,
                       "died_during_load": watcher.death.is_set()}
        for ln in ser.text(mark).splitlines():
            if "[browser] page fetch failed" in ln:
                rec["load"]["fetch_failed_line"] = ln.strip()
                break

        if not watcher.death.is_set():
            ui.goto(*PARK)
            kstat(ser, watcher, "after-load")

        # ---- idle 1 ----
        r1 = "skipped"
        if not watcher.death.is_set():
            r1 = idle_guest(ui, watcher, args.idle1, 400.0, rec, "idle1")
            if r1 == "ok":
                kstat(ser, watcher, "after-idle1")

        # ---- scroll once, click once ----
        centre, win = page_centre(watcher)
        rec["page_centre"] = list(centre)
        rec["browser_win"] = win
        if not watcher.death.is_set() and r1 != "hang":
            ui.goto(*centre)
            time.sleep(0.4)
            m_scroll = ser.size()
            for _ in range(3):
                wheel(ui, True)
                time.sleep(0.2)
            time.sleep(3.0)
            rec["scroll"] = {"guest_ms": watcher.guest_ms,
                             "lines_after": len(ser.text(m_scroll).splitlines())}
            m_click = ser.size()
            nloads = len(watcher.snapshot()["loads"])
            ui.click_at(*centre)
            time.sleep(5.0)
            after = watcher.snapshot()["loads"][nloads:]
            rec["click"] = {"guest_ms": watcher.guest_ms, "navigations": after,
                            "lines_after": len(ser.text(m_click).splitlines())}
            ui.goto(*PARK)

        # ---- idle 2 ----
        r2 = "skipped"
        if not watcher.death.is_set() and r1 != "hang":
            r2 = idle_guest(ui, watcher, args.idle2, 200.0, rec, "idle2")
            if r2 == "ok":
                kstat(ser, watcher, "after-idle2")

        time.sleep(2.0)
        # if it died, ask the machine's own reader for the register file
        g = watcher.snapshot()
        if g["cores"]:
            m = CORE_RE.search(g["cores"][-1][1])
            if m:
                m0 = ser.size()
                ser.send("readcore %s\n" % m.group(4))
                wait_text(ser, "REGIONS", 15.0, m0)
                rec["readcore"] = ser.text(m0)[-6000:]
        kstat(ser, watcher, "end")

        # ---- the verdict ladder: machine first, then the browser ----
        note = "load=%s idle1=%s idle2=%s" % (loaded, r1, r2)
        g = watcher.snapshot()
        if g["panics"]:
            return finish("KERNEL-PANIC", "; ".join(g["panics"][:2]), note)
        if watcher.qemu_exit_code is not None:
            return finish("QEMU-EXIT", "QEMU exited rc=%s" % watcher.qemu_exit_code, note)
        if g["faults"]:
            return finish("BROWSER-DIED-FAULT", g["faults"][0][1], note)
        browser_oom = [l for (_, l) in g["oom"] if "victim:" in l and "rowser" in l]
        if browser_oom:
            return finish("BROWSER-DIED-OOM", browser_oom[0], note)
        if r1 == "hang" or r2 == "hang":
            return finish("GUEST-HANG", "no guest clock line (wm perf / kstat) for %.0f host s"
                          % HANG_HOST_S, note)
        if fetch_failed:
            return finish("FETCH-FAIL", rec["load"].get("fetch_failed_line", "page fetch failed"), note)

        answered = ping(ui, ser, watcher)
        g = watcher.snapshot()
        if g["faults"]:
            return finish("BROWSER-DIED-FAULT", g["faults"][0][1], note)
        if answered:
            return finish("SURVIVED" if loaded else "SURVIVED-NOLOAD",
                          "the about:text ping answered" +
                          ("" if loaded else " (but `load done` never came in %.0fs)" % LOAD_BUDGET),
                          note)
        ref = referee(ui, ser, watcher)
        g = watcher.snapshot()
        if g["faults"]:
            return finish("BROWSER-DIED-FAULT", g["faults"][0][1], note)
        if ref == "alive":
            return finish("ALIVE-WEDGED", "WM says already live; the about:text ping went "
                          "unanswered -- busy or wedged, not dead", note)
        if ref == "relaunched":
            return finish("BROWSER-DIED-SILENT", "the dock click started a FRESH Browser -- "
                          "the previous instance exited with no fault/oom/panic line", note)
        return finish("HARNESS", "neither liveness referee answered (WM printed nothing "
                      "for the dock click)", note)
    except SystemExit:
        raise
    except Exception as e:                                        # noqa: BLE001
        import traceback
        traceback.print_exc()
        rec["traceback"] = traceback.format_exc()
        return finish("HARNESS", "driver error: %r" % (e,))


if __name__ == "__main__":
    sys.exit(main())
