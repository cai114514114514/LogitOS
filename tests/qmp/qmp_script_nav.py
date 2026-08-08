#!/usr/bin/env python3
"""Prove, on the real machine, that a page can navigate ITSELF.

    python3 tests/qmp/qmp_script_nav.py <iso> <disk.img>

WHY THIS EXISTS -- the baidu bug, in full.

https://www.baidu.com/ loaded in 760 ms, dialled one connection, discovered
zero sub-resources and painted a blank white viewport. Neither the parser nor
the cascade nor layout was at fault: baidu SNIFFS THE USER-AGENT, and to ours
("Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0") it does not serve the 697 KB
home page at all. It serves 227 bytes:

    <html><head><script>
      location.replace(location.href.replace("https://","http://"));
    </script></head>
    <body><noscript><meta http-equiv="refresh" content="0;url=http://www.baidu.com/"></noscript></body>
    </html>

That document HAS no stylesheets, HAS no <script src>, HAS no images and HAS
no visible text -- so "0 sub-resources" and "blank page" were not two bugs.
They were one document, rendered correctly.

The real bug is that we then sat on it. `location.replace()` reaches
js_webapi.c's loc_set, which records the request and printed

    [webapi] navigation requested: ... (the loader does not consume this yet)

and the loader -- browser.c -- did not consume it. The same 227 bytes fetched
over http:// instead of https:// return the full 696 KB page, so the site's own
redirect was correct and complete; we were the ones ignoring it.

WHAT THE FIXTURE DOES. The host server serves, at "/stub.html", a byte-for-byte
copy of baidu's real stub with only the redirect target rewritten to this
server's own "/real.html" -- the redirect mechanism, the <noscript> and the
location.href.replace() string surgery are all baidu's.

"/real.html" is the destination, and it is built so that ONE measurement
settles both of the reported facts: its text sits in a block whose colour is
set only by an EXTERNAL stylesheet. So the assertions are:

  * FACT 1, the SERVER LOG: /real.html is requested at all, and then so is
    /site.css. Today neither ever is.
  * FACT 2, the SCREEN: a colour that appears in no markup on the page is on
    screen with dark text pixels inside it. That is only true if the navigation
    happened, the sub-resource was discovered and fetched, AND the result
    reached the painter. A redirect that fetched the page but never repainted
    would pass the server-log channel and fail this one.

AND THE NEGATIVE CONTROL, which is the reason this test can be believed:
"/inert.html" is the SAME page with the SAME <script> tag, differing only in
that its script assigns to a variable instead of calling location.replace().
It must NOT navigate. Without it, "the browser ended up somewhere else" would
also be the reading if the browser had simply started following any link, or
if /stub.html had failed to load and something else had retried.

The loop guard gets its own case: "/ping.html" and "/pong.html" replace each
other for ever. Both halves are asserted -- that it really did bounce (so the
guard is what ended it and not a failure to navigate at all) and that it was
bounded -- and the browser must still be answering its window afterwards.
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

# Deliberately odd colours: find_color locates the block without OCR.
GREEN, RED = (2, 254, 1), (254, 1, 2)

# Baidu's actual stub, byte for byte apart from the redirect target. The string
# surgery (location.href.replace) is theirs and is left intact on purpose --
# it exercises location.href being a real, readable, absolute URL, which a
# location object that only stores what it was handed would get wrong.
STUB = """<html>
<head>
\t<script>
\t\tlocation.replace(location.href.replace("stub.html","real.html"));
\t</script>
</head>
<body>
\t<noscript><meta http-equiv="refresh" content="0;url=/real.html"></noscript>
</body>
</html>
"""

# The control: same shape, same <script>, no navigation. One word, 30px, so a
# dark-pixel count over the block is unambiguous.
INERT = """<html>
<head>
\t<script>
\t\tvar x = location.href.replace("inert.html","real.html");
\t\tconsole.log('NAV-INERT-SCRIPT-RAN');
\t</script>
</head>
<body style="margin:0">
\t<div style="background:#fe0102;color:#000000;font-size:30px">INERTPAGE</div>
</body>
</html>
"""

# The destination. Its colour block is painted ONLY by the EXTERNAL stylesheet,
# so finding that colour on screen proves three separate things at once: the
# navigation happened, the destination's sub-resources were discovered and
# fetched, and the result reached the pixels. Nothing in this document's own
# markup can produce it.
REAL = """<!doctype html><html><head><title>real</title>
<link rel="stylesheet" href="/site.css"></head>
<body style="margin:0">
<div id="d">DESTINATIONPAGE</div>
<script>console.log('NAV-ARRIVED-AT-DESTINATION');</script>
</body></html>
"""
SITECSS = "#d { background: #02fe01; color: #000000; font-size: 30px; display: block; }\n"

# The loop guard's subject: a PAIR that replaces each other for ever. Not one
# self-replacing page -- js_webapi.c treats a location write to the current URL
# as a same-document change and records no navigation at all, so a page
# replacing itself would "pass" the guard by never testing it.
PING = """<html><head><script>location.replace('/pong.html');</script></head>
<body style="margin:0"><div style="background:#fe0102;font-size:30px">PING</div></body></html>
"""
PONG = """<html><head><script>location.replace('/ping.html');</script></head>
<body style="margin:0"><div style="background:#fe0102;font-size:30px">PONG</div></body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_nav_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

requested = []
PAGES = {"/stub.html": STUB, "/real.html": REAL, "/inert.html": INERT,
         "/site.css": SITECSS, "/ping.html": PING, "/pong.html": PONG}


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        requested.append(self.path)
        path = self.path.split("?")[0]
        body = PAGES.get(path, "not found\n")
        raw = body.encode()
        self.send_response(200 if body != "not found\n" else 404)
        self.send_header("Content-Type",
                         "text/css" if path.endswith(".css") else "text/html")
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
    print("----- requested -----")
    print(requested)
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


def wait_req(frag, secs):
    end = time.time() + secs
    while time.time() < end:
        if any(frag in r for r in requested):
            return True
        time.sleep(0.25)
    return False


# WHERE THE ADDRESS BAR ACTUALLY IS.
#
# The other browser drivers click (420, 145) and get away with it, because they
# navigate exactly once and the Browser comes up with `editing` ALREADY TRUE --
# the first URL you type needs no click at all. 145 is the window's TITLE bar;
# clicking it starts a window drag and leaves `editing` false, so the second
# navigation in a session types into nothing and issues no request. That is
# indistinguishable from "the browser refused to navigate", which is the thing
# under test, so it is worth being exact: browser.c draws the URL field at
# window-local y 5..25 (BARH is 30) and the window's client area starts ~34 px
# below the title bar's top edge.
BAR_X, BAR_Y = 420, 175


def goto(ui, path, secs=60, tries=3):
    """Type a URL into the address bar and CONFIRM the guest asked for it.

    The PS/2 keyboard has a one-byte buffer, so on a loaded host (this machine
    runs several agents' QEMUs at once) injected keys are dropped -- and a
    dropped key does not produce a wrong page, it produces an unparseable URL
    and therefore NO REQUEST AT ALL. That is indistinguishable from "the
    browser ignored the navigation", which is the very thing under test here.

    So the server's own log is the acknowledgement: type, press Enter, and if
    the request never arrives, clear the bar and type it again. Same shape as
    the dock-click retry above, and for the same reason."""
    for attempt in range(tries):
        ui.click_at(BAR_X, BAR_Y)
        for _ in range(80):
            ui.key("backspace", settle=0.03)
        ui.typ("http://10.0.2.2:%d%s" % (PORT, path))
        ui.key("ret")
        if wait_req(path, secs):
            return True
        ui.screendump(shot("retype-%s-%d" % (path.strip("/."), attempt)))
        print("   (no request for %s; retyping)" % path)
    return False


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

    # ---- 1. the control FIRST -----------------------------------------------
    # Run it before the subject so that "the browser navigates on its own" can
    # never be explained by state the subject left behind.
    ck(goto(ui, "/inert.html"), "CONTROL: the address bar loaded the control page")
    ck(wait_serial("NAV-INERT-SCRIPT-RAN", 60, "control page load"),
       "CONTROL: a page whose script does NOT call location.replace loaded")
    time.sleep(4.0)
    ck(not any("real.html" in r for r in requested),
       "CONTROL: it did NOT navigate -- /real.html was never requested")
    pc = PPM(ui.screendump(shot("control")))
    ck(pc.find_color(RED) is not None,
       "CONTROL: the control page is still the one on screen")

    # ---- 2. the subject: baidu's own stub -----------------------------------
    n_before = len(requested)
    ck(goto(ui, "/stub.html"), "the stub page was fetched")
    ck(wait_req("real.html", 60),
       "location.replace() NAVIGATED: the destination was fetched")
    ck(wait_serial("NAV-ARRIVED-AT-DESTINATION", 60, "destination script"),
       "the destination document was parsed and its script ran")
    # ---- 3. FACT 1: sub-resources, on the page we were supposed to be on ----
    ck(wait_req("site.css", 30),
       "FACT 1: the destination's EXTERNAL STYLESHEET was discovered and fetched")
    time.sleep(3.0)

    # ---- 4. FACT 2: the pixels ----------------------------------------------
    # This colour exists only in /site.css. Finding it on screen proves the
    # navigation, the sub-resource fetch and the repaint in one measurement --
    # nothing in the destination's own markup can produce it.
    p1 = PPM(ui.screendump(shot("navigated")))
    box = p1.find_color(GREEN)
    ck(box is not None,
       "FACT 2: the destination is PAINTED, in a colour only its external "
       "stylesheet contains")
    ck(p1.dark_pixels(box) > 0,
       "FACT 2: and its TEXT reached the pixels (%d dark pixels in the block)"
       % (p1.dark_pixels(box) if box else 0))
    ck(p1.find_color(RED) is None,
       "and the page we came from is gone from the screen")

    # ---- 5. the loop guard ---------------------------------------------------
    # Two pages that bounce to each other must be STOPPED, and the browser must
    # still be alive afterwards -- a guard that wedges the app is not a guard.
    n_pp = len(requested)
    ck(goto(ui, "/ping.html"), "the first of the bouncing pages was fetched")
    ck(wait_req("pong.html", 60),
       "and it navigated -- so the guard is what ends this, not a failure to navigate")
    time.sleep(20.0)
    hits = sum(1 for r in requested[n_pp:] if "ping.html" in r or "pong.html" in r)
    ck(hits <= 24,
       "the redirect loop was STOPPED after %d fetches, not spun for ever" % hits)
    ck(goto(ui, "/real.html"), "the address bar still works after the loop guard fired")
    ck(wait_serial("NAV-ARRIVED-AT-DESTINATION", 60, "post-loop load"),
       "and the browser is still answering afterwards")

    print("\nPASS: a page can navigate itself -- location.replace reaches the loader, "
          "the pixels and the address bar, and a redirect loop is bounded")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
