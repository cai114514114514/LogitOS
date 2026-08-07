#!/usr/bin/env python3
"""Prove, on the real machine, that the new DOM bindings reach the PIXELS.

    python3 tests/qmp/qmp_dom_bindings.py <iso> <disk.img>

A host unit test can only say that `struct node` changed. This project has
already shipped a 1723/1818 HTML tree-construction score next to a browser that
could not run one real site's script, so a number out of a host test is not
evidence that anything is on screen. The only evidence is a screendump.

The fixture builds a subtree ENTIRELY through the bindings added in this change
-- createDocumentFragment, createElement, className, createTextNode,
appendChild, insertBefore, nodeValue -- and every assertion here is made against
what the guest actually drew:

  1. THREE COLOURS THAT ONLY A SCRIPT CAN PRODUCE. .alpha/.beta/.gamma exist as
     stylesheet rules, and NO element in the HTML source carries those classes.
     The only path from the rule to a pixel is: the script creates an element,
     className= puts the class on it, the cascade matches the rule, layout gives
     it a box, the painter fills it. Finding those colours on screen exercises
     that whole path; the negative control (a build without the change) finds
     none of them.

  2. THE ORDER insertBefore ASKED FOR. The three blocks are appended to a
     fragment in the order beta, gamma and then alpha is spliced in FRONT of
     beta. On screen alpha must be above beta must be above gamma. Presence
     alone would pass even if insertBefore silently degraded to appendChild.

  3. A TEXT NODE REWRITTEN LATER. The gamma block's text is an object the script
     created with createTextNode and keeps a reference to; a timer rewrites it
     through nodeValue seconds after load. The count of near-black pixels inside
     the gamma block must change -- so the write went through the invalidation
     record, the cascade, layout and paint, not just into the DOM.

  4. getBoundingClientRect AGREEING WITH THE PAINTER. The script measures two
     boxes and logs the numbers; the harness locates the same two boxes in the
     screendump. The measured size must equal the painted size to the pixel, and
     the measured distance between them must equal the painted distance. This is
     the one assertion that cannot be satisfied by a plausible-looking stub.

Everything is also logged over serial, so a failure says which stage broke
rather than only "the pixels are wrong".

THE NEGATIVE CONTROL, and how to re-run it. None of the above is evidence until
the same harness FAILS against a browser built without the bindings. There is no
make target for it because the control has to be built from a commit that
predates the change, and any target pinned to "HEAD" stops being a control the
moment the change lands. Run it by hand, inside WSL (never a git worktree --
this filesystem gives new worktrees CRLF endings and every shell script breaks):

    git -c core.autocrlf=false clone --no-hardlinks /mnt/d/ststem /tmp/domctl
    cd /tmp/domctl && git checkout <commit-before-the-bindings>
    make -j8 && make build/disk.img
    cd /mnt/d/ststem && python3 tests/qmp/qmp_dom_bindings.py \
        /tmp/domctl/build/logit.iso /tmp/domctl/build/disk.img --expect-no-bindings

Run at 7131e34 (the commit this change was written on top of) it passes: the
script dies on the first missing binding, none of the three colours reach the
screen, and no rect is ever logged -- while the browser still paints the page's
own markup, so the absence is about the bindings and not a dead browser.
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
from qmp_ui import Session, PPM, dock_icon, BROWSER_SLOT      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# The NEGATIVE CONTROL. Run with --expect-no-bindings against a browser.aex
# built WITHOUT this change and the polarity flips: the script must die on the
# first missing binding, and none of the three script-only colours may be on
# screen. Without this, "the colours are there" is not evidence -- a page that
# somehow painted them for an unrelated reason would look identical.
CONTROL = "--expect-no-bindings" in sys.argv[3:]

# Deliberately odd triples: nothing in the UI chrome or the page background can
# collide with them, so find_color() locates a block without OCR.
ALPHA, BETA, GAMMA = (254, 1, 2), (2, 254, 1), (1, 2, 254)
PROBE = (254, 1, 254)

PROBE_W, PROBE_H = 300, 60

PAGE = """<!doctype html>
<html><head><title>dombind</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 30px; color: #000000; }
/* These three rules are the whole experiment. No element in the markup below
   carries alpha/beta/gamma -- the ONLY way one of these colours can appear on
   screen is a script creating an element and setting className on it. */
.alpha { background: #fe0102; }
.beta  { background: #02fe01; }
.gamma { background: #0102fe; }
#probe { background: #fe01fe; width: 300px; height: 60px; }
</style></head><body>
<div id="slot"></div>
<div id="probe">PROBE</div>
<script>
/* Single-word text on purpose: layout emits one text box per word. */
function mk(cls, txt) {
  var d = document.createElement('div');
  d.className = cls;
  d.appendChild(document.createTextNode(txt));
  return d;
}
var slot = document.getElementById('slot');
var frag = document.createDocumentFragment();
var a = mk('alpha', 'AAAA'), b = mk('beta', 'BBBB'), g = mk('gamma', 'GG');

frag.appendChild(b);
frag.appendChild(g);
frag.insertBefore(a, b);          /* alpha jumps in FRONT of beta */
if (frag.nodeType !== 11) console.log('DOMAPI-BADFRAG ' + frag.nodeType);
slot.appendChild(frag);

/* Report what the DOM says, so a pixel failure can be told apart from a DOM
   failure without guessing. */
console.log('DOMAPI-BUILT ' + slot.children.length + ' ' +
            slot.children[0].className + ',' +
            slot.children[1].className + ',' +
            slot.children[2].className + ' frag=' + frag.childNodes.length +
            ' txt=' + slot.children[0].firstChild.nodeType);

/* Measured from a TIMER, not inline. getBoundingClientRect reads the last
   COMPLETED layout, and inline script runs before the browser has re-laid out
   the subtree it just built -- so measuring here would report the pre-mutation
   geometry (zeros for a brand-new element). That is a real limitation of this
   binding, documented on el_getBoundingClientRect in js_dom.c: there is no
   forced synchronous reflow. Measuring one tick later is what a page has to do
   today, and it is what is asserted. */
var probe = document.getElementById('probe');
setTimeout(function () {
  var ra = a.getBoundingClientRect(), rp = probe.getBoundingClientRect();
  console.log('DOMAPI-RECT ' + ra.x + ' ' + ra.y + ' ' + ra.width + ' ' + ra.height +
              ' | ' + rp.x + ' ' + rp.y + ' ' + rp.width + ' ' + rp.height);
}, 3000);

/* The text node the script owns, rewritten long after load. Long enough that
   the baseline screendump is unambiguously taken BEFORE it fires -- a short
   timeout races the harness and then "the pixels did not change" means "they
   had already changed", which is the opposite conclusion. */
var tnode = g.firstChild;
setTimeout(function () {
  tnode.nodeValue = 'GGGGGGGG';
  console.log('DOMAPI-TEXT ' + g.textContent + ' ' + (g.firstChild === tnode));
}, 12000);
</script>
</body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_dombind_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")


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
    for ok, name in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-4000:])
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


def block(img, colour, what):
    box = img.find_color(colour)
    if not box:
        die("could not find the %s block on screen "
            "(the script did not build it, or it never reached the paint)" % what)
    return box


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 60, "desktop"):
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
    ui.typ("http://10.0.2.2:%d/dom.html" % PORT)
    ui.key("ret")

    built = wait_serial("DOMAPI-BUILT", 60, "page load")

    if CONTROL:
        # The page still LOADS -- the probe block is in the markup, so a
        # screendump proves the browser rendered it. What must not happen is the
        # script completing, or any of the three script-only colours appearing.
        ck(not built,
           "CONTROL: without the change the script never completes "
           "(the first missing binding throws)")
        time.sleep(4.0)
        pc = PPM(ui.screendump(shot("control")))
        ck(pc.find_color(PROBE) is not None,
           "CONTROL: the browser still rendered the page's own markup, so the "
           "absence below is about the bindings and not about a dead browser")
        for colour, name in ((ALPHA, "alpha"), (BETA, "beta"), (GAMMA, "gamma")):
            ck(pc.find_color(colour) is None,
               "CONTROL: the %s block is NOT on screen" % name)
        ck("DOMAPI-RECT" not in serial(), "CONTROL: no getBoundingClientRect result")
        print("\nPASS (control): without this change every assertion the positive "
              "run makes is false -- so the positive run is measuring this change")
        proc.kill()
        sys.exit(0)

    ck(built,
       "the page loaded and its script ran to completion "
       "(every new binding was called without throwing)")

    log = serial()
    ck("DOMAPI-BADFRAG" not in log, "createDocumentFragment reports nodeType 11")
    m = re.search(r"DOMAPI-BUILT (\d+) (\S+) frag=(\d+) txt=(\d+)", log)
    ck(m is not None, "the script's DOM report reached the serial log")
    ck(m.group(1) == "3", "the fragment moved three children into the document")
    ck(m.group(2) == "alpha,beta,gamma",
       "insertBefore put alpha in front of beta INSIDE the fragment (order %s)" % m.group(2))
    ck(m.group(3) == "0", "and the fragment itself was left empty, as the DOM says")
    ck(m.group(4) == "3", "createTextNode produced a real TEXT node (nodeType 3)")

    ck(wait_serial("DOMAPI-RECT", 60, "the deferred measurement"),
       "a timer measured both boxes one tick after the build")
    log = serial()
    time.sleep(2.0)
    p0 = PPM(ui.screendump(shot("p0")))

    b_a = block(p0, ALPHA, "alpha")
    b_b = block(p0, BETA, "beta")
    b_g = block(p0, GAMMA, "gamma")
    b_p = block(p0, PROBE, "probe")
    ck(True, "all three script-built blocks are PAINTED -- three colours that "
             "no element in the page's markup can have")

    # ---- order ------------------------------------------------------------
    ck(b_a[1] < b_b[1] < b_g[1],
       "and they are in the order insertBefore asked for: alpha(y=%d) above "
       "beta(y=%d) above gamma(y=%d)" % (b_a[1], b_b[1], b_g[1]))

    # ---- getBoundingClientRect vs the painter ------------------------------
    r = re.search(r"DOMAPI-RECT (-?\d+) (-?\d+) (-?\d+) (-?\d+) \| "
                  r"(-?\d+) (-?\d+) (-?\d+) (-?\d+)", log)
    ck(r is not None, "the script logged both getBoundingClientRect results")
    ax, ay, aw, ah, px, py, pw, ph = (int(v) for v in r.groups())

    ck((pw, ph) == (PROBE_W, PROBE_H),
       "getBoundingClientRect returned the probe's CSS size exactly "
       "(%dx%d, wanted %dx%d)" % (pw, ph, PROBE_W, PROBE_H))
    # find_color's box is INCLUSIVE at both ends, so the span is x1-x0+1.
    painted_w, painted_h = b_p[2] - b_p[0] + 1, b_p[3] - b_p[1] + 1
    ck((painted_w, painted_h) == (pw, ph),
       "and the painter drew exactly that many pixels (%dx%d) -- what the "
       "script MEASURES and what the screen SHOWS are the same numbers"
       % (painted_w, painted_h))
    # Absolute origins differ by the window chrome, so the DISTANCE between two
    # measured boxes is what can be compared against the screen without
    # hardcoding where the browser window happens to be.
    ck(abs((py - ay) - (b_p[1] - b_a[1])) <= 1,
       "the measured vertical distance alpha->probe (%d px) equals the painted "
       "distance (%d px)" % (py - ay, b_p[1] - b_a[1]))
    ck(abs((px - ax) - (b_p[0] - b_a[0])) <= 1,
       "and so does the horizontal distance (%d vs %d px)"
       % (px - ax, b_p[0] - b_a[0]))

    # ---- the text node, rewritten later ------------------------------------
    ck("DOMAPI-TEXT" not in serial(), "the text rewrite has not fired yet at baseline")
    g_before = p0.dark_pixels(b_g)
    ck(wait_serial("DOMAPI-TEXT", 90, "the nodeValue rewrite"),
       "a timer rewrote the script-created text node through nodeValue")
    t = re.search(r"DOMAPI-TEXT (\S+) (\S+)", serial())
    ck(t is not None and t.group(1) == "GGGGGGGG",
       "textContent reads back the rewritten data")
    ck(t is not None and t.group(2) == "true",
       "and it is the SAME text node object -- the write went in place, so a "
       "held reference (React's textInstance) does not go stale")
    time.sleep(2.0)
    p1 = PPM(ui.screendump(shot("p1")))
    g_after = p1.dark_pixels(block(p1, GAMMA, "gamma"))
    ck(g_after != g_before,
       "the nodeValue write reached the SCREEN (text pixels %d -> %d)"
       % (g_before, g_after))

    print("\nPASS: the new DOM bindings build a subtree that reaches the pixels, "
          "in the right order, at the geometry the script measured")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
