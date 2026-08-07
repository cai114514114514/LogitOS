#!/usr/bin/env python3
"""Prove, on the real machine, that a loaded page is LIVE.

    python3 tests/qmp/qmp_live_page.py <iso> <disk.img>

Host assertions cannot settle this. The whole bug class being removed here is
"the JS runtime was destroyed before the user could click" -- which a host test
that drives dispatch directly cannot see, because it never navigates away from
the script that registered the handler. Only booting the OS, loading a page over
the network, waiting, and then clicking can.

The page under test carries four blocks, each painted an exact, deliberately odd
colour so the harness can locate it in a screendump without OCR or hardcoded
layout coordinates:

  #tgt   (red)     a click handler rewrites its text        -> DOM mutation from an event
  #timer (green)   a setTimeout rewrites its text           -> the timer queue runs
  #plink (blue)    a link whose handler calls preventDefault -> the DEFAULT ACTION is optional
  #golink(magenta) an identical link with NO handler         -> the control
  #cssom (cyan)    a handler READS getComputedStyle and then WRITES element.style
                   -> the CSSOM is connected to the pixels in both directions

Each assertion is made twice over, from two independent channels:

  * the SCREEN. The count of near-black (text) pixels inside a colour block must
    change, which is only true if the mutation reached layout and paint.
  * the SERIAL LOG. The page's console.log lines are printf'd by the browser.

And the preventDefault case gets a third, decisive channel: the host web server
records every path it is asked for. If "/navigated.html" is ever requested after
the blue link is clicked, preventDefault did not prevent anything. The magenta
link then proves the converse -- that clicking a link DOES navigate -- so the
blue link's silence cannot be explained by clicks simply not working.
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

RED, GREEN, BLUE, MAGENTA = (254, 1, 2), (2, 254, 1), (1, 2, 254), (254, 1, 254)
# The CSSOM block's before/after colours. The "after" one is never written in
# the page's stylesheet -- it exists only as the value a script assigns to
# el.style, so finding it on screen is proof the assignment reached the paint.
CYAN, ORANGE = (1, 254, 254), (254, 127, 1)

# Single-word strings on purpose: layout emits one text box PER WORD, so a click
# aimed at a glyph inside a multi-word run can land in the gap between boxes and
# hit the block background instead of the text (and, for a link, miss the href).
PAGE = """<!doctype html>
<html><head><title>live</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div, a { display: block; font-size: 30px; color: #000000; text-decoration: none; }
#tgt    { background: #fe0102; }
#timer  { background: #02fe01; }
#plink  { background: #0102fe; }
#golink { background: #fe01fe; }
#cssom  { background: #01fefe; padding: 4px; }
</style></head><body>
<div id="tgt">CLICKTARGETBEFORE</div>
<div id="timer">TIMERBEFOREWAITING</div>
<div id="cssom">CSSOMBLOCK</div>
<a id="plink" href="/navigated.html">PREVENTDEFAULTLINK</a>
<a id="golink" href="/navigated.html">GOLINK</a>
<script>
console.log('LIVE-SCRIPT-RAN');
document.getElementById('tgt').addEventListener('click', function (e) {
  e.currentTarget.textContent = 'X';
  console.log('LIVE-MARK-CLICK');
});
/* Long enough that the baseline screenshot is unambiguously taken BEFORE it
   fires. A short timeout races the harness's own click steps, and then "the
   pixels did not change" means "they had already changed", which is the
   opposite conclusion. */
setTimeout(function () {
  document.getElementById('timer').textContent = 'Y';
  console.log('LIVE-MARK-TIMER');
}, 12000);
document.getElementById('plink').addEventListener('click', function (e) {
  e.preventDefault();
  document.getElementById('plink').textContent = 'Z';
  console.log('LIVE-MARK-PREVENT');
});
/* The CSSOM, both directions, on the real machine. The read is logged verbatim
   so a mismatch is diagnosable from the serial log rather than only from a
   pixel count, and the write is GATED on the read being right -- so if
   getComputedStyle lied, the colour on screen never changes and the harness
   sees it. font-size is written alongside the colour because the two exercise
   different invalidation tiers: a colour moves no box, 60px moves every box
   below it. */
document.getElementById('cssom').addEventListener('click', function (e) {
  var el = e.currentTarget;
  var cs = getComputedStyle(el);
  console.log('LIVE-CSSOM-READ ' + cs.backgroundColor + ' / ' + cs.fontSize +
              ' / ' + cs.display + ' / ' + cs.paddingTop);
  if (cs.backgroundColor === 'rgb(1, 254, 254)' && cs.fontSize === '30px' &&
      cs.display === 'block' && cs.paddingTop === '4px') {
    el.style.backgroundColor = '#fe7f01';
    el.style.fontSize = '60px';
    console.log('LIVE-CSSOM-WROTE ' + el.style.cssText);
  } else {
    console.log('LIVE-CSSOM-BADREAD');
  }
});
</script>
</body></html>
"""

NAVIGATED = """<!doctype html><html><body style="background:#101010">
<div style="color:#ffffff;font-size:40px">NAVIGATEDPAGE</div></body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_live_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

requested = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        requested.append(self.path)
        body = (PAGE if self.path.startswith("/live") else
                NAVIGATED if self.path.startswith("/navigated") else "not found\n")
        raw = body.encode()
        self.send_response(200 if body != "not found\n" else 404)
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
        die("could not find the %s block on screen (page did not render?)" % what)
    return box


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    # The WM says when the desktop is composited; a fixed sleep here is a coin
    # flip on a loaded host, and a dock click that lands one frame early is
    # simply swallowed -- which then shows up much later as "the page never
    # loaded", pointing at entirely the wrong thing.
    if not wait_serial("desktop live", 60, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    for _ in range(4):                     # re-click: the first can still be too early
        if wait_serial("launched Browser", 15, "browser launch"):
            break
        ui.click_at(*dock_icon(BROWSER_SLOT))
    else:
        die("the Dock never launched the Browser")
    time.sleep(6)                          # ~2.8 MB .aex off virtio-blk, ELF load, first paint

    # Address bar, clear it, type the fixture URL. 10.0.2.2 is the SLIRP host.
    ui.click_at(420, 145)
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/live.html" % PORT)
    ui.key("ret")

    ck(wait_serial("LIVE-SCRIPT-RAN", 60, "page load"),
       "the page loaded and its inline <script> ran")
    time.sleep(2.0)
    p0 = PPM(ui.screendump(shot("p0")))

    b_tgt = block(p0, RED, "click-target")
    b_tim = block(p0, GREEN, "timer")
    b_pl = block(p0, BLUE, "preventDefault link")
    b_go = block(p0, MAGENTA, "control link")
    b_css = block(p0, CYAN, "CSSOM")
    ck(True, "all five fixture blocks are painted on screen")
    ck(p0.find_color(ORANGE) is None,
       "the colour only a script can produce is NOT on screen yet")

    # The timer baseline has to be read from THIS screenshot, before anything
    # else in the harness has had time to happen -- see the comment on the
    # setTimeout delay in the fixture.
    ck("LIVE-MARK-TIMER" not in serial(), "the setTimeout has not fired yet at baseline")
    tim_before = p0.dark_pixels(b_tim)

    # ---- 1. a click handler that mutates the DOM ----------------------------
    aim = p0.first_dark(b_tgt) or ((b_tgt[0] + b_tgt[2]) // 2, (b_tgt[1] + b_tgt[3]) // 2)
    before = p0.dark_pixels(b_tgt)
    ui.click_at(aim[0], aim[1])
    ck(wait_serial("LIVE-MARK-CLICK", 20, "click handler"),
       "clicking the element ran its addEventListener handler")
    time.sleep(1.5)
    p1 = PPM(ui.screendump(shot("p1")))
    after = p1.dark_pixels(block(p1, RED, "click-target"))
    ck(after != before,
       "the click handler's DOM mutation reached the screen (text pixels %d -> %d)"
       % (before, after))

    # ---- 1b. the CSSOM, both directions -------------------------------------
    # getComputedStyle has to answer with the page's real cascaded values (the
    # handler refuses to write anything unless all four properties come back
    # exactly right), and the element.style write has to survive the cascade,
    # layout and paint -- which is only demonstrable by a colour appearing on
    # screen that no stylesheet in the page contains.
    b_css = block(p1, CYAN, "CSSOM")           # re-located: step 1 changed the page
    css_h_before = b_css[3] - b_css[1]
    aim = p1.first_dark(b_css) or ((b_css[0] + b_css[2]) // 2, (b_css[1] + b_css[3]) // 2)
    ui.click_at(aim[0], aim[1])
    ck(wait_serial("LIVE-CSSOM-READ", 20, "getComputedStyle"),
       "the handler read getComputedStyle on the real machine")
    ck("LIVE-CSSOM-BADREAD" not in serial(),
       "getComputedStyle returned the page's real cascaded values "
       "(background-color, font-size, display and padding-top all matched)")
    ck(wait_serial("LIVE-CSSOM-WROTE", 20, "element.style write"),
       "and the handler then wrote element.style")
    time.sleep(2.0)
    p1b = PPM(ui.screendump(shot("p1b")))
    b_orange = p1b.find_color(ORANGE)
    ck(b_orange is not None,
       "el.style.backgroundColor reached the PIXELS -- a colour no stylesheet "
       "in the page mentions is now on screen")
    ck(p1b.find_color(CYAN) is None, "and the stylesheet's colour is gone from that box")
    css_h_after = b_orange[3] - b_orange[1]
    ck(css_h_after > css_h_before,
       "el.style.fontSize reached LAYOUT too: the box grew (%d -> %d px tall)"
       % (css_h_before, css_h_after))

    # ---- 2. setTimeout ------------------------------------------------------
    ck(wait_serial("LIVE-MARK-TIMER", 60, "setTimeout"),
       "a setTimeout registered at load fired later, from the main loop")
    time.sleep(1.5)
    p2 = PPM(ui.screendump(shot("p2")))
    tim_after = p2.dark_pixels(block(p2, GREEN, "timer"))
    ck(tim_after != tim_before,
       "the timer's DOM mutation reached the screen (text pixels %d -> %d)"
       % (tim_before, tim_after))

    # ---- 3. preventDefault suppresses the link's default action -------------
    requested[:] = [r for r in requested]
    ck(not any("navigated" in r for r in requested),
       "nothing has fetched /navigated.html yet")
    # Re-located from THIS screenshot: step 1b grew the block above it, so the
    # link has moved down since p0 was taken.
    b_pl = block(p2, BLUE, "preventDefault link")
    aim = p2.first_dark(b_pl) or ((b_pl[0] + b_pl[2]) // 2, (b_pl[1] + b_pl[3]) // 2)
    pl_before = p2.dark_pixels(b_pl)
    ui.click_at(aim[0], aim[1])
    ck(wait_serial("LIVE-MARK-PREVENT", 20, "link handler"),
       "clicking the link ran its handler")
    time.sleep(3.0)                        # generous: a navigation would show by now
    ck(not any("navigated" in r for r in requested),
       "preventDefault() SUPPRESSED the navigation -- /navigated.html was never fetched")
    p3 = PPM(ui.screendump(shot("p3")))
    pl_box = block(p3, BLUE, "preventDefault link")
    ck(p3.dark_pixels(pl_box) != pl_before,
       "the still-current page updated in place instead of navigating")

    # ---- 4. the control: an unhandled link DOES navigate --------------------
    # Without this, "no request for /navigated.html" would also be the reading
    # if clicks never reached the page at all.
    b_go = block(p3, MAGENTA, "control link")
    aim = p3.first_dark(b_go) or ((b_go[0] + b_go[2]) // 2, (b_go[1] + b_go[3]) // 2)
    ui.click_at(aim[0], aim[1])
    end = time.time() + 25
    while time.time() < end and not any("navigated" in r for r in requested):
        time.sleep(0.3)
    ck(any("navigated" in r for r in requested),
       "CONTROL: the identical link WITHOUT a handler did navigate")
    time.sleep(2.0)
    p4 = PPM(ui.screendump(shot("p4")))
    ck(p4.find_color(BLUE) is None and p4.find_color(RED) is None,
       "the browser really left the fixture page")

    print("\nPASS: the page is live -- events, timers and default actions all work on device")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
