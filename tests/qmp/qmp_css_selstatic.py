#!/usr/bin/env python3
"""Prove ON THE MACHINE that the static pseudo-classes and @supports moved pixels.

    python3 tests/qmp/qmp_css_selstatic.py <iso> <disk.img>

WHY THIS EXISTS WHEN test-css-selstatic IS ALREADY GREEN. That gate asserts on
`struct cstyle` in a host process. This line's own scar is what makes that
insufficient: `make test-wpt ONLY=css/css-grid` once read 531/11152 WITH AND
WITHOUT an entire grid implementation, because the runner linked layout.c and
never called it. Linking a translation unit is not running it, and a claim about
a browser has to be a real page behaving differently in the guest. So this boots
LogitOS, serves two fixtures over SLIRP, and measures the SCREEN.

TWO FIXTURES, one per half of the slice.

  CHECKED  css_engine.c handed LibCSS h_false for :checked, so every rule behind
           it was selected away. The fixture is the CSS-only pattern real pages
           use -- `input:checked + label` -- with a checked box and an unchecked
           one side by side.

           THE ASSERTION IS A PAIR AND THAT IS THE WHOLE DESIGN. Asserting only
           that the checked box's label is painted would pass on a handler that
           matched EVERYTHING, which is the exact failure mode of a hasty
           implementation and the one no drop counter can see (nothing was
           dropped -- the wrong elements were styled). So the unchecked box's
           label carries a colour that only an over-matching engine can paint,
           and its ABSENCE is asserted as hard as the other's presence.
           "Absent beats present-and-wrong" is a claim about that second half.

  SUPPORTS @supports answered NO for transform, transform-origin and box-shadow
           -- properties css_extra.c really produces and browser_paint.c really
           draws. A NO on a feature test is not a missing feature: it is the
           page being TALKED OUT OF the branch it would have rendered.

           AND CHECKING THE BOOLEAN IS NOT ENOUGH. An @supports test that only
           asks which branch was taken is testing the lookup table against
           itself. So the true branch does not merely recolour the box, it
           TRANSLATES it: the assertion is that the box's left edge is displaced
           from an untransformed reference box beside it. If @supports says yes
           and the transform does not move anything, that is the
           says-YES-cannot-do-it lie, and only the geometric assertion sees it.

Every colour is deliberately odd so it cannot collide with the wallpaper
gradient, the glass chrome or font antialiasing. Screendumps are left in the
temp directory named at the end.
"""

import os
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import (Session, PPM,      # noqa: E402
                    configure, pt)

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

CON = (254, 1, 3)       # #fe0103  label of the CHECKED box -- must appear
COFF = (254, 254, 3)    # #fefe03  label of the UNCHECKED box -- must NEVER appear
CREF = (1, 254, 3)      # #01fe03  a plain reference box, proves the page loaded

SOK = (3, 1, 254)       # #0301fe  the @supports TRUE branch -- must appear
SBAD = (253, 2, 253)    # #fd02fd  the @supports NOT branch -- must NEVER appear
SREF = (2, 253, 253)    # #02fdfd  same box, no transform: the displacement ruler

# The `+` combinator is what makes this a real-page pattern rather than a
# selector unit test: the rule styles a SIBLING of the matched element, so the
# match has to survive the combinator, not just the pseudo-class.
PAGE_CHECKED = """<!doctype html>
<html><head><title>selstatic-checked</title><style>
  html, body { background: #ffffff; margin: 0; padding: 0; }
  label { display: block; width: 240px; height: 80px; background: #cccccc; }
  #ref { width: 240px; height: 40px; background: #01fe03; }
  #on:checked  + label { background: #fe0103; }
  #off:checked + label { background: #fefe03; }
</style></head><body>
<div id="ref"></div>
<input type="checkbox" id="on" checked><label for="on"></label>
<input type="checkbox" id="off"><label for="off"></label>
</body></html>
"""

# #sref carries no transform and is the ruler. #sok is inside the @supports
# TRUE branch and is translated 200px right; if the branch is taken but the
# transform is inert, sok and sref share a left edge and the test fails.
PAGE_SUPPORTS = """<!doctype html>
<html><head><title>selstatic-supports</title><style>
  html, body { background: #ffffff; margin: 0; padding: 0; }
  #sref { width: 120px; height: 60px; background: #02fdfd; }
  #sok  { width: 120px; height: 60px; background: #888888; }
  @supports (transform: translateX(200px)) {
    #sok { background: #0301fe; transform: translateX(200px); }
  }
  @supports not (transform: translateX(200px)) {
    #sok { background: #fd02fd; }
  }
</style></head><body>
<div id="sref"></div>
<div id="sok"></div>
</body></html>
"""


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        raw = (PAGE_SUPPORTS if "supports" in self.path else PAGE_CHECKED).encode()
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

XRES = int(os.environ.get("QMP_XRES", "1280"))
YRES = int(os.environ.get("QMP_YRES", "800"))
SCALE = configure(XRES, YRES)
print("display %dx%d device px, backing scale %d%%" % (XRES, YRES, SCALE))

tmp = tempfile.mkdtemp(prefix="selstatic-")
serial_path = os.path.join(tmp, "serial.log")
qmp_path = os.path.join(tmp, "qmp.sock")

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

checks = []


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    print("----- artefacts in %s -----" % tmp)
    try:
        proc.kill()
    except Exception:
        pass
    sys.exit(1)


def ck(cond, what):
    checks.append((bool(cond), what))
    print(("ok   " if cond else "FAIL ") + what)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited during %s" % what)
        time.sleep(0.5)
    return False


def block(img, rgb, what):
    box = img.find_color(rgb)
    if box is None:
        die("no pixel of %s (%s) on screen" % (str(rgb), what))
    return box


def load(ui, path, bar_y):
    """Type a URL into the address bar and wait for the page.

    `bar_y` is a parameter for the reason qmp_css_modern.py records: the first
    click lands while the window is still playing its open-pop animation, so
    the bar is higher up the screen than it will be for the second navigation.
    Clicking the settled y twice drags the window by its titlebar instead of
    focusing the bar, the second fixture never loads, and every later assertion
    silently measures the FIRST page -- which is why the first check after a
    navigation is always that the previous page is gone."""
    ui.click_at(pt(420), pt(bar_y))
    for _ in range(80):
        ui.key("backspace")
    ui.typ("http://10.0.2.2:%d/%s" % (PORT, path))
    ui.key("ret")
    time.sleep(10)


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    time.sleep(6)

    ui = Session(qmp_path, serial=serial_path)
    # launch_app ADDS the check this driver never had: it used to click and
    # sleep 3 s, so a click that opened nothing (or the wrong app) silently
    # turned every later screendump comparison into a comparison of whatever
    # window did happen to be up.
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(3.0)
    ui.screendump(os.path.join(tmp, "launch.ppm"), settle=0.4)

    # ------------------------------- :checked -------------------------------
    load(ui, "checked.html", 145)
    shot = os.path.join(tmp, "checked.ppm")
    ui.screendump(shot)
    img = PPM(shot)

    block(img, CREF, "the plain reference box (did the page load at all?)")
    on = block(img, CON, "the label of the CHECKED box")
    print("checked label = %s" % (on,))
    ck(on is not None,
       ":checked -- `input:checked + label` paints the checked box's label")

    off = img.find_color(COFF)
    ck(off is None,
       ":checked -- and the UNCHECKED box's label is NOT painted "
       "(an over-matching handler would paint it)")

    # ------------------------------ @supports -------------------------------
    load(ui, "supports.html", 175)
    shot2 = os.path.join(tmp, "supports.ppm")
    ui.screendump(shot2)
    img2 = PPM(shot2)

    gone = img2.find_color(CON)
    ck(gone is None, "the second fixture really did load (the first is gone)")

    ref = block(img2, SREF, "the untransformed reference box")
    ok = block(img2, SOK, "the box inside the @supports TRUE branch")
    print("sref=%s  sok=%s" % (ref, ok))

    ck(ok is not None,
       "@supports (transform:...) takes the TRUE branch on a real page")

    bad = img2.find_color(SBAD)
    ck(bad is None,
       "@supports -- the `not (transform:...)` fallback is NOT painted")

    # THE ONE THAT TESTS THE ENGINE RATHER THAN THE TABLE.
    dx = ok[0] - ref[0]
    want = pt(200)
    print("displacement = %d device px (want ~%d)" % (dx, want))
    ck(abs(dx - want) <= pt(8),
       "...and the transform REALLY MOVED the box %d px, so the YES is a "
       "capability and not a claim" % want)

    bad_count = sum(1 for okc, _ in checks if not okc)
    print("\nqmp_css_selstatic: %d checks, %d failures" % (len(checks), bad_count))
    print("artefacts in %s" % tmp)
    if bad_count:
        sys.exit(1)
finally:
    try:
        proc.kill()
    except Exception:
        pass
