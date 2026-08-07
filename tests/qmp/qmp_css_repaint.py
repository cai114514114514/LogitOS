#!/usr/bin/env python3
"""What a REPAINT costs on the real machine, measured by the page itself.

    python3 tests/qmp/qmp_css_repaint.py <iso> <disk.img> [tag]

The bug report was "deepseek.com pulls stylesheets extremely slowly, then
repaints extremely slowly". The fetching half belongs to the HTTP layer. This
measures the repainting half, on device, under TCG, with the network reduced to
a host server one hop away over SLIRP.

WHY THE PAGE TIMES ITSELF. A host stopwatch around a QEMU run measures boot,
the dock, the .aex load off virtio-blk and the fixture fetch -- all of which
dwarf and hide the thing under test. The fixture instead calls Date.now() in
the guest, around the mutations only, and console.log()s the result, which the
browser printf's to serial. What comes out is guest wall-clock for N repaints
and nothing else.

WHY setTimeout AND NOT A LOOP. Mutating 50 times inside one script turn is ONE
repaint: js_dom.c records invalidation scopes and browser.c re-styles once when
the turn ends. That would measure scope coalescing, not repaint cost. Chaining
one mutation per setTimeout tick gives N separate turns, so N separate
css_apply_scoped + layout + paint cycles -- which is exactly the shape of a live
page nudging one element per tick, and exactly the path the caches changed.

THE STYLESHEET IS REAL. The three deepseek.com stylesheets (66 KiB, 306 custom
properties, 511 var() calls) are served alongside, because the cost being
measured was proportional to stylesheet size: the engine used to re-parse the
whole sheet on every single mutation. Against a 200-byte stylesheet the bug is
invisible, which is why a synthetic fixture would have found nothing.
"""
import http.server
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, dock_icon, pt, BROWSER_SLOT   # noqa: E402

ISO = sys.argv[1]
DISK = sys.argv[2]
TAG = sys.argv[3] if len(sys.argv) > 3 else "run"

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIX = os.path.join(ROOT, "tests", "fixtures", "cssperf")
SHEETS = sorted(f for f in os.listdir(FIX) if f.startswith("ds-") and f.endswith(".css"))

NMUT = 40

# A body with enough elements that a scope is not the whole document, and a
# chained setTimeout that mutates ONE leaf per tick. The class it toggles
# carries a real declaration, so the cascade genuinely changes something and the
# CSS_CHANGED_NONE short circuit cannot be what is being measured.
PAGE = """<!doctype html><html><head>
%s
<style>
.rowA { color: #112233; background: #fafbfc; padding: 2px }
.rowB { color: #445566; background: #f0f1f2; padding: 2px }
</style></head><body>
<div id="wrap">%s</div>
<script>
var n = 0, N = %d, leaf = document.getElementById('leaf'), t0 = 0;
function tick() {
  if (n === 0) t0 = Date.now();
  if (n >= N) {
    console.log('REPAINT-DONE ' + N + ' ' + (Date.now() - t0));
    return;
  }
  leaf.className = (n %% 2) ? 'rowA' : 'rowB';
  n++;
  setTimeout(tick, 0);
}
console.log('REPAINT-START');
setTimeout(tick, 0);
</script></body></html>
"""

rows = "".join(
    '<p class="rowA"><span>row %d</span> some words to lay out here</p>' % i
    for i in range(300)
)
rows += '<p id="leaf" class="rowA"><span>the leaf</span></p>'
links = "".join('<link rel="stylesheet" href="/%s">' % s for s in SHEETS)
PAGE = PAGE % (links, rows, NMUT)


# REAL-PAGE MODE. CSSPERF_PAGE=wikipedia.html serves that captured page instead
# of the synthetic one, with its <link> hrefs rewritten to the local sheets.
# There is no script and so no REPAINT-DONE marker: this mode exists for the
# before/after PICTURE and the load wall-clock on a page nobody wrote for us.
REAL = os.environ.get("CSSPERF_PAGE", "")
if REAL:
    raw = open(os.path.join(FIX, REAL), "r", encoding="utf-8", errors="replace").read()
    wp = sorted(f for f in os.listdir(FIX) if f.startswith("wp-") and f.endswith(".css"))
    n = [0]

    def _relink(m):
        i = n[0]
        n[0] += 1
        return '<link rel="stylesheet" href="/%s">' % (wp[i] if i < len(wp) else wp[-1])

    PAGE = re.sub(r'<link[^>]*rel="stylesheet"[^>]*>', _relink, raw)
    SHEETS = wp


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        name = self.path.lstrip("/").split("?")[0]
        if name.endswith(".css"):
            path = os.path.join(FIX, os.path.basename(name))
            if not os.path.exists(path):
                self.send_response(404); self.end_headers(); return
            raw = open(path, "rb").read()
            ctype = "text/css"
        else:
            raw = PAGE.encode()
            ctype = "text/html"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_a):
        pass


srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

tmp = tempfile.mkdtemp(prefix="qmp_repaint_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")

XRES = int(os.environ.get("QMP_XRES", "1280"))
YRES = int(os.environ.get("QMP_YRES", "800"))
SCALE = configure(XRES, YRES)

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES),
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


def wait_serial(needle, secs):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        time.sleep(0.5)
    return False


rc = 1
try:
    if not wait_serial("LOGIT_BOOT_OK", 240):
        print("FAIL: kernel never booted; artefacts in %s" % tmp)
        raise SystemExit(1)
    time.sleep(6)

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    time.sleep(3.0)

    ui.click_at(pt(420), pt(145))
    for _ in range(60):
        ui.key("backspace")
    url = "http://10.0.2.2:%d/repaint.html" % PORT
    ui.typ(url)
    t_enter = time.time()
    ui.key("ret")

    if not wait_serial("[browser] load done", 300):
        print("FAIL: page never finished loading; artefacts in %s" % tmp)
        raise SystemExit(1)
    t_loaded = time.time()

    nmut = ms = 0
    if not REAL:
        if not wait_serial("REPAINT-DONE", 300):
            print("FAIL: the mutation ticks never completed; artefacts in %s" % tmp)
            raise SystemExit(1)
        m = re.search(r"REPAINT-DONE (\d+) (\d+)", serial())
        if not m:
            print("FAIL: no REPAINT-DONE marker")
            raise SystemExit(1)
        nmut, ms = int(m.group(1)), int(m.group(2))
    else:
        time.sleep(3)                      # let the last repaint settle

    shot = os.path.join(tmp, "page_%s.ppm" % TAG)
    ui.screendump(shot, settle=0.5)

    print("")
    print("=== repaint benchmark [%s]%s ===" % (TAG, " " + REAL if REAL else ""))
    print("  host wall-clock, Enter -> load done : %6.2f s" % (t_loaded - t_enter))
    if nmut:
        print("  guest, %d scoped repaints           : %6d ms  (%.1f ms each)"
              % (nmut, ms, ms / float(nmut)))
    print("  screenshot: %s" % shot)
    rc = 0
finally:
    try:
        proc.terminate(); proc.wait(timeout=10)
    except Exception:
        proc.kill()
raise SystemExit(rc)
