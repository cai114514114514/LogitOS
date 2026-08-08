#!/usr/bin/env python3
"""The acceptance case: bing.com's own bytes, on the machine, to the screen.

    python3 tests/qmp/qmp_bing.py <iso> <disk.img>

WHY THE FIXTURE AND NOT THE LIVE SITE
The live site is a different page every hour, serves a different document per
edge, and needs DNS and TLS to be working on the day. The bytes in
tests/fixtures/webapi/bing are the exact ones the host probe
(tests/unit/webapi_probe.c) measured, captured at this browser's own
User-Agent, so what this harness reports is comparable with what the probe
predicted -- and a difference between them is a finding rather than noise.
They are served here at the paths the document names (/rp/<hash>.js), so the
browser does its own resource discovery and its own four sub-fetches.

WHAT IS ASSERTED AND WHAT IS ONLY REPORTED
Asserted: the document loads, all five resources are fetched from this server,
the page's scripts run, and the number that DIE equals the number the host
probe predicts (one). Reported, not asserted: the screenshot, and every
exception text.

The one death is bing's own. Its second inline script ends with
`_w.sj_pt = sj_pt`, and the only `var _w = window` in the 193 KiB document is
37 KiB further down, in a later script. A browser executes classic scripts in
document order, so Chrome throws the identical ReferenceError at the identical
statement. Calling that a browser bug -- and calling a page that renders anyway
a pass -- is what this harness exists not to do.
"""

import os
import sys
import subprocess
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM, dock_icon, BROWSER_SLOT      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, "..", "fixtures", "webapi", "bing")

# The document, and the four scripts it <script src>es, keyed by the path the
# document asks for. Read as BYTES: this page is 193 KiB of UTF-8 with a large
# Chinese fraction, and a decode/re-encode here would change what the guest
# receives from what the probe measured.
INDEX = open(os.path.join(FIX, "index.html"), "rb").read()
ROUTES = {}
for line in open(os.path.join(FIX, "manifest.txt"), encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    src, fn = line.split("\t")
    ROUTES[src] = open(os.path.join(FIX, fn), "rb").read()

tmp = tempfile.mkdtemp(prefix="qmp_bing_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

requested = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        requested.append(self.path)
        path = self.path.split("?")[0]
        if path in ("/", "/index.html", "/page.html"):
            raw, ctype = INDEX, "text/html; charset=utf-8"
        elif path in ROUTES:
            raw, ctype = ROUTES[path], "application/javascript"
        else:
            raw, ctype = b"", "text/plain"
        self.send_response(200 if raw else 404)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

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

checks = []


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def report_and_exit(code):
    log = serial()
    exc = [ln for ln in log.splitlines()
           if "JS exception" in ln or "uncaught in" in ln]
    print("\n===== bing.com, on the machine =====")
    print("resources the host server served: %d" % len(requested))
    for r in requested:
        print("   %s" % r)
    print("JS exceptions (%d):" % len(exc))
    for e in exc:
        print("   %s" % e.strip())
    print("artefacts: %s" % tmp)
    proc.kill()
    sys.exit(code)


def die(msg):
    print("FAIL: " + msg)
    for ok, name in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
    print("----- serial (tail) -----")
    print(serial()[-8000:])
    report_and_exit(1)


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

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    for _ in range(4):
        if wait_serial("launched Browser", 15, "browser launch"):
            break
        ui.click_at(*dock_icon(BROWSER_SLOT))
    else:
        die("the Dock never launched the Browser")
    time.sleep(6)

    ui.click_at(420, 145)
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/index.html" % PORT)
    ui.key("ret")

    # `load done` is browser.c's own end-of-load line and the only marker on the
    # serial that means the document parsed, its <script src>es were fetched and
    # every classic script has run. Waiting on it rather than on a count of
    # requests matters now: bing's own loader keeps fetching after the initial
    # five, so a request count is a moving target and `load done` is not.
    ck(wait_serial("load done", 300, "the document"),
       "the 193 KiB document parsed, its scripts were fetched, and they ran")

    # Then let the page's own dynamic loader run for a while: everything after
    # the first five requests is bing asking for its own modules, which is what
    # a working page does and a dead one does not.
    for _ in range(45):
        n = len(requested)
        time.sleep(2)
        if len(requested) == n:
            break

    ck(len(requested) >= 5,
       "the browser fetched the document and its four scripts (%d requests total)"
       % len(requested))

    log = serial()
    exc = [ln for ln in log.splitlines() if "JS exception" in ln]

    # The host probe predicts exactly one death, and names it. If the machine
    # produces a different number, the machine has a problem the host does not
    # -- which is the only reason this is an assertion and not a printout.
    ck(len(exc) == 1,
       "exactly one of bing's twelve scripts throws, as the host probe predicts "
       "(%d thrown)" % len(exc))
    ck("_w" in exc[0] and "not defined" in exc[0],
       "...and it is bing's own `_w` ordering bug (%s)" % exc[0].strip())

    time.sleep(3)
    p = PPM(ui.screendump(shot("bing")))
    print("screendump: %s" % shot("bing"))
    # "A flat grey viewport" is the reported symptom, so count what is on it.
    # Distinct colours over the content area is the cheapest measurement that
    # separates "nothing was painted" from "something was", and it is REPORTED
    # rather than asserted -- painting is the renderer's line, not this one.
    seen = set()
    step = 3
    for y in range(200, min(p.h, 780), step):
        for x in range(60, min(p.w, 1220), step):
            seen.add(p.at(x, y))
    print("distinct colours sampled in the viewport: %d" % len(seen))

    print("\nPASS: bing's own bytes load on the machine, all five resources are "
          "fetched, and eleven of its twelve scripts run clean")
    report_and_exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
