#!/usr/bin/env python3
"""Prove ON THE MACHINE that a modern stylesheet now reaches the pixels.

    python3 tests/qmp/qmp_css_modern.py <iso> <disk.img>

The host tests in tests/unit/css_modern_test.c assert on `struct cstyle`, and
`make audit-css` asserts on the display list. Both stop one step short of the
claim, which is about a rendered page. This boots LogitOS, serves two fixtures
from the host over SLIRP, loads them in the Browser, and measures the SCREEN.

Each fixture reproduces, in miniature, a way a real corpus page came out as a
single unstyled column:

  LAYER   tailwind.com wraps all 692 KiB of its CSS in five `@layer` blocks. An
          at-rule this LibCSS did not know was discarded WITH ITS BLOCK, so not
          one of its rules cascaded: measured with `make audit-css-before` the
          page had 0 flex containers, 0 grid containers and 0 elements governed
          by either. The fixture puts a three-item flex row inside @layer and a
          @supports nested inside it. The assertion is GEOMETRIC and it is the
          one a column cannot fake: the three coloured boxes must have THREE
          DIFFERENT left edges and ONE shared top edge. Laid out as a block they
          stack -- one left edge, three tops -- which is exactly what the
          pre-change browser drew.

  VARS    apple.com declares a 208-byte custom property. It was stored in a
          192-byte field, truncated before its closing `)`, and every later
          `calc(var(...))` expanded to text with an unclosed bracket -- so
          LibCSS tokenised its way inside and swallowed the remaining bytes of
          every remaining stylesheet. apple.com's cascade saw 4% of its own
          declarations. The fixture declares an over-long var, references it,
          and then -- AFTER it -- paints a box. That box's colour is the whole
          test: with the bug the rule never reaches the cascade and the box is
          never painted at all.

Both fixtures also carry a control colour that only the WRONG rendering can
produce, so a page that silently failed to load cannot pass by drawing nothing.
Screendumps are left in the temp directory named at the end.
"""

import os
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import (Session, PPM, dock_icon, BROWSER_SLOT,      # noqa: E402
                    configure, pt)

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# Odd values so no desktop chrome or font antialiasing can collide with them.
LA = (254, 1, 3)        # #fe0103  flex item A, inside @layer
LB = (1, 254, 3)        # #01fe03  flex item B, inside @layer
LC = (3, 1, 254)        # #0301fe  flex item C, inside a @supports in the @layer
LBAD = (254, 254, 3)    # #fefe03  the @supports NOT branch -- must never appear
VOK = (2, 253, 253)     # #02fdfd  the box declared AFTER the over-long var
VBAD = (253, 2, 253)    # #fd02fd  its var() fallback -- must never appear

# The whole stylesheet lives inside @layer, the way tailwind ships it, and the
# third item's rule is inside a @supports nested in that layer -- which needs
# both the group-rule branch AND the nested-at-rule dispatch.
PAGE_LAYER = """<!doctype html>
<html><head><title>cssmodern-layer</title><style>
@layer base, components;
@layer base {
  html, body { background: #ffffff; margin: 0; padding: 0; }
}
@layer components {
  #row { display: flex; width: 600px; margin: 0; }
  #row > div { width: 150px; height: 60px; flex-shrink: 0; }
  #la { background: #fe0103; }
  #lb { background: #01fe03; }
  @supports (display: flex) {
    #lc { background: #0301fe; }
  }
  @supports not (display: flex) {
    #lc { background: #fefe03; }
  }
}
</style></head><body>
<div id="row"><div id="la"></div><div id="lb"></div><div id="lc"></div></div>
</body></html>
"""

# 208 bytes of nested min()/calc(), the shape apple.com ships. The rule that
# paints #02fdfd comes AFTER it: with the truncating store the unbalanced paren
# swallowed everything from here to the end of the sheet.
PAGE_VARS = """<!doctype html>
<html><head><title>cssmodern-vars</title><style>
:root {
  --a: 2; --b: 5; --c: 1; --d: 3;
  --group-delay: min( (var(--a) * 80ms) + ((var(--b) - var(--c)) * 40ms), \
var(--d) * 80ms, (var(--a) + var(--b) + var(--c) + var(--d)) * 20ms );
}
html, body { background: #ffffff; margin: 0; padding: 0; }
#probe { transition-delay: calc(var(--group-delay) + 80ms); }
/* Everything below here is what the truncated var used to swallow. */
#vok { background: var(--never-declared, #fd02fd); width: 240px; height: 90px; }
#vok { background: #02fdfd; }
</style></head><body>
<div id="probe"></div><div id="vok"></div>
</body></html>
"""


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        raw = (PAGE_VARS if "vars" in self.path else PAGE_LAYER).encode()
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

tmp = tempfile.mkdtemp(prefix="cssmodern-")
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


def _bbox(img, rgb):
    """Bounding box of every pixel exactly equal to `rgb`, or None."""
    return img.find_color(rgb)


def block(img, rgb, what):
    box = _bbox(img, rgb)
    if box is None:
        die("no pixel of %s (%s) on screen" % (str(rgb), what))
    return box


def load(ui, path, bar_y):
    """Type a URL into the address bar and wait for the page.

    `bar_y` is a parameter and not a constant because the FIRST click happens
    while the window is still playing its open-pop animation, with the bar
    higher up the screen; by the second navigation the window has settled and
    145 is the title bar. Clicking 145 twice drags the window instead of
    focusing the bar, the second fixture never loads, and the assertions then
    measure the first fixture's pixels -- which is why the first check after
    every navigation is that the previous page is GONE."""
    ui.click_at(pt(420), pt(bar_y))        # address bar (points -> device px)
    for _ in range(80):
        ui.key("backspace")
    ui.typ("http://10.0.2.2:%d/%s" % (PORT, path))
    ui.key("ret")
    time.sleep(10)


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    time.sleep(6)

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    time.sleep(3.0)
    ui.screendump(os.path.join(tmp, "launch.ppm"), settle=0.4)

    # ---------------- @layer + nested @supports ----------------
    load(ui, "layer.html", 145)
    shot = os.path.join(tmp, "layer.ppm")
    ui.screendump(shot)
    img = PPM(shot)

    a = block(img, LA, "flex item A (inside @layer)")
    b = block(img, LB, "flex item B (inside @layer)")
    c = block(img, LC, "flex item C (inside a @supports inside the @layer)")
    print("A=%s  B=%s  C=%s" % (a, b, c))

    # THE assertion: a flex row puts them side by side. A block fallback --
    # which is what a discarded @layer leaves -- stacks them.
    ck(a[0] < b[0] < c[0],
       "@layer: the three items have three different left edges (a flex row)")
    ck(a[1] == b[1] == c[1],
       "@layer: ...and one shared top edge, so they are NOT stacked")
    ck(b[0] - a[0] == c[0] - b[0],
       "@layer: the two gaps are equal, so the row is evenly laid out")

    bad = _bbox(img, LBAD)
    ck(bad is None,
       "@supports: the `not (display:flex)` fallback branch is NOT painted")

    # ---------------- the over-long custom property ----------------
    load(ui, "vars.html", 175)
    shot2 = os.path.join(tmp, "vars.ppm")
    ui.screendump(shot2)
    img2 = PPM(shot2)

    gone = _bbox(img2, LA)
    ck(gone is None, "the second fixture really did load (the first is gone)")

    v = block(img2, VOK, "the rule declared AFTER the over-long var")
    print("post-var box = %s" % (v,))
    ck(v is not None,
       "a 208-byte custom property does not swallow the rest of the sheet")
    vbad = _bbox(img2, VBAD)
    ck(vbad is None,
       "...and the box takes its real colour, not the var() fallback")

    bad_count = sum(1 for ok, _ in checks if not ok)
    print("\nqmp_css_modern: %d checks, %d failures" % (len(checks), bad_count))
    print("artefacts in %s" % tmp)
    if bad_count:
        sys.exit(1)
finally:
    try:
        proc.kill()
    except Exception:
        pass
