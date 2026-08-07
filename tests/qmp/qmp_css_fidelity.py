#!/usr/bin/env python3
"""Prove, on the real machine, that the CSS properties layout newly reads are
actually reaching the screen.

    python3 tests/qmp/qmp_css_fidelity.py <iso> <disk.img>

Host unit tests assert on the display list, which is one step short of the
claim: a box can have the right geometry in `struct item` and still be painted
wrong, and a `struct cstyle` field can be read correctly and then dropped on the
floor between layout and paint. This boots the OS, serves a fixture page from
the host over SLIRP, loads it in the Browser and measures the PIXELS.

Every block is painted an exact, deliberately odd colour so the harness can
locate it in a screendump without OCR or hardcoded layout coordinates. Three
properties are under test, each chosen because it was completely absent before:

  border-box   two boxes with identical `width:300px;padding:20px;border:5px`,
               one content-box and one border-box. Their painted widths must
               DIFFER by exactly 50px (2*20 padding + 2*5 border). Before, both
               were painted 300 wide -- content-box lost its padding.

  <pre>        four lines of preformatted text, one of them blank. The block's
               painted height must cover four line boxes, and the text pixels
               must form four distinct rows. Before, all of it collapsed onto a
               single line.

  flex wrap    three 200px items in a 500px row with `flex-wrap:wrap` and
               `justify-content:flex-end`. Two must share the first row, the
               third must be on a second row, and the first row's leftmost item
               must be pushed right by the 100px of slack. Before, flex was a
               single unwrapped row that ignored justify-content entirely.

The screendumps are kept in the temp directory named at the end so a failure can
be looked at rather than guessed at.
"""

import os
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM, dock_icon, BROWSER_SLOT      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# Odd values so nothing in the desktop chrome or the font antialiasing can
# collide with them.
CONTENT_BOX = (254, 1, 2)      # #fe0102
BORDER_BOX = (2, 254, 1)       # #02fe01
PRE_BG = (1, 2, 254)           # #0102fe
FLEX_A = (254, 1, 254)         # #fe01fe
FLEX_B = (1, 254, 254)         # #01fefe
FLEX_C = (254, 254, 1)         # #fefe01

PAGE = """<!doctype html>
<html><head><title>cssfid</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
/* Same authored width, same padding, same border, different box-sizing. */
.box { width: 300px; padding: 20px; border: 5px solid #000000; }
#cb { box-sizing: content-box; background: #fe0102; }
#bb { box-sizing: border-box;  background: #02fe01; }
/* Preformatted: newlines, runs of spaces and a blank line all preserved. */
#pre { background: #0102fe; font-size: 20px; margin: 0; }
/* A wrapping, end-justified flex row. */
#row { display: flex; width: 500px; flex-wrap: wrap;
       justify-content: flex-end; margin: 0; }
#row > div { width: 200px; height: 40px; flex-shrink: 0; }
#fa { background: #fe01fe; }
#fb { background: #01fefe; }
#fc { background: #fefe01; }
</style></head><body>
<div id="cb" class="box">CB</div>
<div id="bb" class="box">BB</div>
<pre id="pre">AAAA
  BBBB

CCCC</pre>
<div id="row"><div id="fa"></div><div id="fb"></div><div id="fc"></div></div>
</body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_cssfid_")
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
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-4000:])
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


def block(img, colour, what):
    box = img.find_color(colour)
    if not box:
        die("could not find the %s block on screen (page did not render?)" % what)
    return box


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    time.sleep(6)                          # desktop + dock composited

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    time.sleep(3.0)                        # ~2.7 MB .aex off virtio-blk, then ELF load
    ui.screendump(os.path.join(tmp, "launch.ppm"), settle=0.4)

    ui.click_at(420, 145)                  # address bar
    for _ in range(60):
        ui.key("backspace")
    ui.typ("http://10.0.2.2:%d/cssfid.html" % PORT)
    ui.key("ret")
    time.sleep(10)
    shot = os.path.join(tmp, "page.ppm")
    ui.screendump(shot)
    img = PPM(shot)

    # ---- box-sizing ----
    cb = block(img, CONTENT_BOX, "content-box")
    bb = block(img, BORDER_BOX, "border-box")
    cbw = cb[2] - cb[0] + 1
    bbw = bb[2] - bb[0] + 1
    print("content-box painted width %d, border-box painted width %d" % (cbw, bbw))
    # The background is painted inside the border, so both measured runs are
    # the border box minus the two 5px borders. The DIFFERENCE is what matters
    # and it is exactly the padding+border content-box adds: 50px.
    ck(cbw - bbw == 50,
       "content-box box is exactly 50px wider than the identical border-box one")
    ck(bbw == 300 - 10,
       "border-box painted the authored 300px (less its two 5px borders)")

    # ---- <pre> ----
    pre = block(img, PRE_BG, "pre")
    preh = pre[3] - pre[1] + 1
    # Rows inside the block that contain near-black text pixels, grouped into
    # runs: one run per line of text. The blank line must leave a gap.
    rows = []
    for y in range(pre[1], pre[3] + 1):
        dark = 0
        for x in range(pre[0], pre[2] + 1):
            r, g, b = img.at(x, y)
            if r < 100 and g < 100 and b < 100:
                dark += 1
        rows.append(dark > 0)
    runs = 0
    for i, on in enumerate(rows):
        if on and (i == 0 or not rows[i - 1]):
            runs += 1
    print("pre block height %d px, %d text rows" % (preh, runs))
    ck(runs == 3, "pre painted 3 rows of text (AAAA / BBBB / CCCC), not one")
    # 20px font -> 25px line box; four line boxes including the blank one.
    ck(preh >= 90, "pre block is tall enough for four line boxes (blank line kept)")

    # ---- flex wrap + justify-content ----
    fa = block(img, FLEX_A, "flex item A")
    fb = block(img, FLEX_B, "flex item B")
    fc = block(img, FLEX_C, "flex item C")
    print("flex A %s  B %s  C %s" % (fa, fb, fc))
    ck(abs(fa[1] - fb[1]) <= 2, "flex items A and B share the first row")
    ck(fc[1] > fa[3], "flex item C wrapped onto a second row")
    ck(abs((fb[0] - fa[0]) - 200) <= 2, "flex items are 200px apart on the row")
    # 500 - 400 = 100px of slack, pushed to the LEFT of A by justify-content:flex-end
    ck(fa[0] - pre[0] >= 90, "justify-content:flex-end pushed the first row right by ~100px")
    ck(abs(fc[2] - fb[2]) <= 2, "the wrapped row is flex-end aligned too")

    print("\nALL PASS -- artefacts in %s" % tmp)
finally:
    proc.kill()
