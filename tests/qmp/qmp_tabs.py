#!/usr/bin/env python3
"""Two real pages open at once, on the real machine.

    python3 tests/qmp/qmp_tabs.py <iso> <disk.img>

WHAT IS BEING MEASURED, and why a host test could not settle it.

tests/unit/loader_test.c part 3 links the real loader against a fake network
and proves the model: exactly one tab is live, a background tab keeps the BYTES
it was built from, and switching back replays them without a connection. What
it cannot show is the machine -- a real window, a real HTTP server over SLIRP, a
real repaint -- and two of the claims are only interesting there:

  1. BOTH PAGES ARE STILL PAGES. Each fixture's visible block is coloured ONLY
     by an EXTERNAL stylesheet, so finding that colour in a screendump proves
     the document was parsed, its sub-resource fetched, the cascade applied and
     the result painted. Switching tabs and finding the OTHER colour proves the
     same for the other document. Nothing in either page's own markup can
     produce the other's colour, so the two cannot be confused.

  2. SWITCHING DOES NOT RE-FETCH. The fixture server logs every request. The
     count is taken before the switch and after it, and it must not move. This
     is the claim tabs would break first: the connection pool landed today with
     measurable reuse, and a tab implementation that reloads on every switch
     hands all of that back. The server log is the honest channel for it --
     the browser's own counters could be wrong in the same direction as the
     bug.

  3. THE SESSION SURVIVES THE APP. Cmd+W closes the window; the Dock launches
     it again; the browser must come back with both tabs. Asserted from the
     browser's own startup line on the serial console, because the tab strip's
     text is too small to read out of a screendump reliably.

The screenshots (tab-a, tab-b, back-to-a) are the deliverable; the checks are
what make them evidence.
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

# Deliberately odd colours so find_color cannot match anything the desktop or
# the browser chrome draws.
GREEN, RED = (2, 254, 1), (254, 1, 2)

PAGE_A = """<!doctype html><html><head><title>ALPHA</title>
<link rel="stylesheet" href="/a.css"></head>
<body style="margin:0"><div id="d">ALPHAPAGE</div>
<script>console.log('TAB-A-SCRIPT-RAN');</script></body></html>
"""
A_CSS = "#d { background: #02fe01; color: #000000; font-size: 30px; display: block; }\n"

PAGE_B = """<!doctype html><html><head><title>BETA</title>
<link rel="stylesheet" href="/b.css"></head>
<body style="margin:0"><div id="d">BETAPAGE</div>
<script>console.log('TAB-B-SCRIPT-RAN');</script></body></html>
"""
B_CSS = "#d { background: #fe0102; color: #000000; font-size: 30px; display: block; }\n"

PAGES = {"/a.html": PAGE_A, "/a.css": A_CSS, "/b.html": PAGE_B, "/b.css": B_CSS}

tmp = tempfile.mkdtemp(prefix="qmp_tabs_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
SHOTS = os.environ.get("TAB_SHOTS", tmp)
os.makedirs(SHOTS, exist_ok=True)
shot = lambda n: os.path.join(SHOTS, n + ".ppm")

requested = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        requested.append(self.path)
        path = self.path.split("?")[0]
        body = PAGES.get(path, "not found\n")
        raw = body.encode()
        self.send_response(200 if path in PAGES else 404)
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


def boot():
    return subprocess.Popen(
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


proc = boot()
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
    print("----- artefacts in %s -----" % SHOTS)
    print("----- requested -----")
    print(requested)
    print("----- serial (tail) -----")
    print(serial()[-8000:])
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


# ---- modified keys -------------------------------------------------------
# qmp_ui.Session has key() and key_shift() and nothing else, so the two
# modifiers this test needs are built here out of raw input events.
#
# WHICH MODIFIER FOR WHAT is not arbitrary -- see the shortcut table above
# app_main in c/apps/browser/browser.c. Cmd is the system modifier and the
# window manager claims a closed list of it (Cmd+W/Q/M/Tab/`), so the browser
# takes Cmd+T for a new tab and CANNOT take Cmd+W or Cmd+Tab. Closing a tab is
# Ctrl+W and cycling tabs is Ctrl+Tab. Cmd+W in this file therefore closes the
# WINDOW, which is exactly what the restart check wants.
def _hold(ui, mod, qcode, settle=0.35):
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": mod}, "down": True}}])
    time.sleep(0.05)
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": True}}])
    time.sleep(0.05)
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": False}}])
    time.sleep(0.05)
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": mod}, "down": False}}])
    time.sleep(settle)


def cmd_key(ui, qcode):  _hold(ui, "meta_l", qcode)
def ctrl_key(ui, qcode): _hold(ui, "ctrl", qcode)


# WHERE THE ADDRESS BAR IS, asked rather than assumed.
#
# Every other browser driver here hard-codes a screen coordinate for the URL
# field, and they get away with it because the Browser comes up with `editing`
# ALREADY TRUE -- the first URL needs no click. This test navigates several
# times, and it has moved the field down by the height of the tab strip, so a
# guess is not good enough: a click 30 px too high lands on the tab strip and
# the typing goes nowhere, which is indistinguishable from the browser
# refusing to navigate.
#
# The window manager prints its own geometry, so use it:
#   [wm] win 1 frame X Y W H content CW CH pt ...
# The content origin is (X, Y + (H - CH)) -- the difference is the title bar,
# which is the WM's and not the app's.
TABH = 30
_origin = None


def content_origin():
    global _origin
    if _origin:
        return _origin
    import re
    m = None
    for m in re.finditer(r"\[wm\] win \d+ frame (-?\d+) (-?\d+) (\d+) (\d+) "
                         r"content (\d+) (\d+)", serial()):
        pass
    if not m:
        die("the window manager never printed the Browser's frame")
    fx, fy, fw, fh, cw, chh = (int(g) for g in m.groups())
    _origin = (fx, fy + (fh - chh))
    print("   browser content origin: %r (frame %dx%d, content %dx%d)"
          % (_origin, fw, fh, cw, chh))
    return _origin


def win(x, y):
    ox, oy = content_origin()
    return (ox + x, oy + y)


# window-local: the URL field is y TABH+5 .. TABH+25, x 10 .. w-10
def bar_point():
    return win(420, TABH + 15)


def goto(ui, path, secs=90, tries=3):
    """Type a URL and CONFIRM the guest asked for it.

    Same retry as qmp_script_nav.py and for the same reason: the PS/2 keyboard
    has a one-byte buffer, so on a loaded host an injected key is dropped, and
    a dropped key yields an unparseable URL and NO REQUEST -- indistinguishable
    from the browser ignoring the navigation."""
    for attempt in range(tries):
        ui.click_at(*bar_point())
        for _ in range(90):
            ui.key("backspace", settle=0.02)
        ui.typ("http://10.0.2.2:%d%s" % (PORT, path))
        ui.key("ret")
        if wait_req(path, secs):
            return True
        ui.screendump(shot("retype-%s-%d" % (path.strip("/."), attempt)))
        print("   (no request for %s; retyping)" % path)
    return False


def launch_browser(ui):
    ui.click_at(*dock_icon(BROWSER_SLOT))
    for _ in range(4):
        if "launched Browser" in serial():
            return True
        time.sleep(4)
        ui.click_at(*dock_icon(BROWSER_SLOT))
    return "launched Browser" in serial()


try:
    if not wait_serial("LOGIT_BOOT_OK", 240, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 90, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path, serial=serial_path)
    ck(launch_browser(ui), "the Dock launched the Browser")
    time.sleep(6)

    # ---- 1. tab one ---------------------------------------------------------
    ck(goto(ui, "/a.html"), "tab 1 asked for page A")
    ck(wait_serial("TAB-A-SCRIPT-RAN", 90, "page A"), "page A loaded and ran its script")
    ck(wait_req("/a.css", 30), "and fetched its external stylesheet")
    time.sleep(3)
    pa = PPM(ui.screendump(shot("tab-a")))
    box_a = pa.find_color(GREEN)
    ck(box_a is not None,
       "TAB 1 IS PAINTED, in a colour only its external stylesheet contains")
    ck(pa.dark_pixels(box_a) > 0, "and its text reached the pixels")

    # ---- 2. a second tab ----------------------------------------------------
    cmd_key(ui, "t")
    time.sleep(1.5)
    ck(goto(ui, "/b.html"), "Cmd+T opened a tab and it asked for page B")
    ck(wait_serial("TAB-B-SCRIPT-RAN", 90, "page B"), "page B loaded and ran its script")
    time.sleep(3)
    pb = PPM(ui.screendump(shot("tab-b")))
    ck(pb.find_color(RED) is not None, "TAB 2 IS PAINTED, by ITS external stylesheet")
    ck(pb.find_color(GREEN) is None,
       "and tab 1 is not on the screen -- one document is live, which is the model")

    # ---- 3. THE MEASUREMENT: switching back must not re-fetch ---------------
    before = len(requested)
    ctrl_key(ui, "tab")
    time.sleep(6)
    p1 = PPM(ui.screendump(shot("back-to-a")))
    ck(p1.find_color(GREEN) is not None,
       "SWITCHING BACK RE-RENDERED TAB 1 -- a background tab is still a page")
    ck(p1.find_color(RED) is None, "and tab 2 is gone from the screen")
    new = requested[before:]
    print("   requests during the switch: %r" % (new,))
    ck(len(new) == 0,
       "AND IT COST ZERO NETWORK REQUESTS: the switch replayed the tab's own "
       "bytes, so N tabs do not multiply the handshakes the pool removed")

    # forward again, so "tab 1 came back" is not "tab 1 never left"
    before = len(requested)
    ctrl_key(ui, "tab")
    time.sleep(6)
    p2 = PPM(ui.screendump(shot("back-to-b")))
    ck(p2.find_color(RED) is not None, "and forward to tab 2 again")
    ck(len(requested[before:]) == 0, "also with no network")

    ck("from tab" in serial(),
       "the browser's own counter agrees (it prints resources from tab vs network)")

    # ---- 4. the session across a restart of the app -------------------------
    # Cmd+W is the window manager's, not the browser's -- see the note on
    # _hold above. It closes the WINDOW, which is the restart this checks.
    marker = serial()
    cmd_key(ui, "w")
    time.sleep(4)
    ck(launch_browser(ui), "the Dock launched the Browser a second time")
    time.sleep(8)
    tail = serial()[len(marker):]
    ck("session restored 2 tabs" in tail or "restored 2 tabs" in tail,
       "SESSION RESTORE: the browser came back with both tabs after a restart")
    ui.screendump(shot("restored"))

    print("\nPASS: two real pages open at once, switching between them costs no "
          "network, and the session survives the app")
    print("screenshots: %s" % SHOTS)
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
