#!/usr/bin/env python3
"""Prove, on the real machine, that `<script type="module">` runs and that its
`import` is fetched, resolved and evaluated -- and that the result reaches the
PIXELS.

    python3 tests/qmp/qmp_module_page.py <iso> <disk.img>

WHY THIS TEST EXISTS. Every Vite/Rollup/Next build on the web ships as an ES
module graph. The browser evaluated every <script> with JS_EVAL_TYPE_GLOBAL, and
a classic script whose first token is `import` is a SyntaxError at byte 0 -- so
the whole class of real sites had exactly zero JavaScript running, while the
project's HTML tree-construction score said 1723/1818. A score cannot see this.
A screenshot can.

THE DECISIVE ASSERTION, and it is deliberately not "a module ran". Two different
modules import the SAME specifier, "./lib.mjs":

    /js/main.mjs  (external module)  ->  must resolve to /js/lib.mjs
    the inline <script type="module"> ->  must resolve to /lib.mjs

Those are two different files with two different contents. If the loader
resolved specifiers against the DOCUMENT rather than against the importing
module's own URL -- the easy wrong implementation -- both would fetch /lib.mjs
and the test fails on the tag each one reports. The host server records every
path it is asked for, so this is checked from the network side as well as from
the page's own console.

/js/main.mjs additionally imports "../shared/util.mjs", which is only reachable
if dot segments are removed: url.c's url_resolve concatenates, so the unresolved
form is "/js/../shared/util.mjs".

The screen is read twice. The control page /mod.html?plain=1 is byte-identical
except that the module scripts are omitted, so it gives a genuine BEFORE for the
text-pixel count; and each block's final colour is one that appears in no
stylesheet on the page, so "the module reached the paint" is visible as a colour
that could not otherwise exist.
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

# Starting colours (from the page's own stylesheet) ...
RED, GREEN, BLUE, MAGENTA, CYAN = ((254, 1, 2), (2, 254, 1), (1, 2, 254),
                                   (254, 1, 254), (1, 254, 254))
# ... and the colours only a running module can produce. None of these appears
# anywhere in the page's CSS.
ORANGE, PURPLE, SPRING = (254, 127, 1), (127, 1, 254), (1, 254, 127)

STYLE = """<style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 30px; color: #000000; }
#ext  { background: #fe0102; }
#inl  { background: #02fe01; }
#ord  { background: #0102fe; }
#dyn  { background: #fe01fe; }
#data { background: #01fefe; }
</style>"""

# Long single-word strings: layout emits one text box PER WORD, and a long run
# gives a dark-pixel count that cannot be confused with the short one a module
# replaces it with.
BODY = """
<div id="ext">EXTBEFOREEXTBEFOREEXTBEFOREEXTBEFORE</div>
<div id="inl">INLBEFOREINLBEFOREINLBEFOREINLBEFORE</div>
<div id="ord">ORDBEFOREORDBEFOREORDBEFOREORDBEFORE</div>
<div id="dyn">DYNBEFOREDYNBEFOREDYNBEFOREDYNBEFORE</div>
<div id="data">DATABEFOREDATABEFOREDATABEFORE</div>
"""

# A data block. Its contents are not valid JavaScript by any reading, so if the
# browser executes it the serial log fills with a syntax error that is reported
# as the PAGE's -- which is exactly what used to happen to <script
# type="importmap"> and <script type="application/json"> on real sites.
DATA_BLOCK = """<script type="application/json">
{ "this": is not ]] javascript at all, function( }
</script>"""

CLASSIC = """<script>
console.log('MOD-CLASSIC-RAN');
document.getElementById('ord').textContent = 'CLASSIC';
</script>"""

MODULES = """<script type="module" src="/js/main.mjs"></script>
<script type="module">
/* base = the DOCUMENT, so this "./lib.mjs" is /lib.mjs -- a DIFFERENT file from
   the one /js/main.mjs gets for the same specifier. */
import { mark, tag } from './lib.mjs';
console.log('MOD-INLINE-RAN lib=' + tag);
mark('inl', 'INLINE-OK', '#7f01fe');
console.log('MOD-INLINE-DONE');
</script>"""

MAIN_MJS = """import { mark, tag } from './lib.mjs';
import { shout } from '../shared/util.mjs';
console.log('MOD-MAIN-RAN lib=' + tag);
console.log('MOD-DOTDOT ' + shout('ok'));
/* Deferred means: every classic script has already run by now. */
console.log('MOD-ORDER ' + document.getElementById('ord').textContent);
mark('ext', 'EXT-OK', '#fe7f01');
import('./dyn.mjs')
  .then(function (m) { m.go(mark); console.log('MOD-DYNAMIC-DONE'); })
  .catch(function (e) { console.log('MOD-DYNAMIC-FAILED ' + e); });
console.log('MOD-MAIN-DONE');
"""

# Same specifier, two different files -- this is the whole point.
LIB_IN_JS = """export const tag = 'JSDIR';
export function mark(id, text, colour) {
  var el = document.getElementById(id);
  el.textContent = text;
  el.style.backgroundColor = colour;
}
"""
LIB_AT_ROOT = LIB_IN_JS.replace("JSDIR", "ROOT")

UTIL_MJS = "export function shout(s) { return s.toUpperCase(); }\n"
DYN_MJS = """export function go(mark) {
  mark('dyn', 'DYN-OK', '#01fe7f');
  console.log('MOD-DYNAMIC-RAN');
}
"""

FILES = {
    "/js/main.mjs": MAIN_MJS,
    "/js/lib.mjs": LIB_IN_JS,
    "/lib.mjs": LIB_AT_ROOT,
    "/shared/util.mjs": UTIL_MJS,
    "/js/dyn.mjs": DYN_MJS,
}


def page(plain):
    return ("<!doctype html>\n<html><head><title>modules</title>" + STYLE +
            "</head><body>" + BODY + DATA_BLOCK + CLASSIC +
            ("" if plain else MODULES) + "</body></html>\n")


tmp = tempfile.mkdtemp(prefix="qmp_mod_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

requested = []


class Fixture(http.server.BaseHTTPRequestHandler):
    # HTTP/1.1 with keep-alive on purpose: the connection pool is the other half
    # of this change and a 1.0 server would hide it.
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.path.split("?")[0]
        requested.append(self.path)
        if path in ("/mod.html", "/"):
            body, ctype = page("plain=1" in self.path), "text/html"
        elif path in FILES:
            body, ctype = FILES[path], "text/javascript"
        else:
            body, ctype = "not found\n", "text/plain"
        raw = body.encode()
        self.send_response(200 if body != "not found\n" else 404)
        self.send_header("Content-Type", ctype)
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
    print("----- paths the fixture server was asked for -----")
    print("\n".join(requested[-40:]))
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


def block(img, colour, what):
    box = img.find_color(colour)
    if not box:
        die("could not find the %s block on screen (page did not render?)" % what)
    return box


def goto(ui, url, bar=None):
    """Type `url` into the address bar and press Enter.

    `bar` is where to click to focus it. It is DERIVED, not hardcoded: the WM
    places the window, and a fixed (420, 145) -- which is what the other drivers
    use -- lands on the title bar rather than in the URL field. That is invisible
    on the FIRST navigation, because the browser starts with the address bar
    already focused, and then silently fatal on every one after it: the
    keystrokes go to the page, Enter does nothing, and the failure reads as "the
    page never loaded".

    Pass bar=None for the first navigation, where focus is already there."""
    if bar:
        ui.click_at(bar[0], bar[1])
    for _ in range(90):
        ui.key("backspace", settle=0.02)
    ui.typ(url)
    ui.key("ret")


def bar_from(img):
    """The address bar's centre, worked out from the page below it.

    The fixture's first element sits at page y = 0 with no margin, so the top of
    the red block IS the top of the viewport -- which is the window's client
    origin plus BARH (30). The URL field is drawn at client y 5..25."""
    b = img.find_color(RED)
    if not b:
        die("cannot locate the page on screen to derive the address bar")
    return (b[0] + 320, b[1] - 30 + 15)


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

    base = "http://10.0.2.2:%d" % PORT

    # ---- 0. the CONTROL page: identical markup, module scripts omitted ------
    # This is where the "before" pixel counts come from. Without it the only
    # available claim would be "the page has some text on it".
    goto(ui, base + "/mod.html?plain=1")
    ck(wait_serial("MOD-CLASSIC-RAN", 90, "control page"),
       "the control page (no modules) loaded and its classic script ran")
    time.sleep(2.0)
    p0 = PPM(ui.screendump(shot("p0")))
    before = {"ext": p0.dark_pixels(block(p0, RED, "ext")),
              "inl": p0.dark_pixels(block(p0, GREEN, "inl")),
              "dyn": p0.dark_pixels(block(p0, MAGENTA, "dyn"))}
    ck(p0.find_color(ORANGE) is None and p0.find_color(PURPLE) is None and
       p0.find_color(SPRING) is None,
       "none of the three module-only colours is on screen for the control page")

    # ---- 1. the real page --------------------------------------------------
    goto(ui, base + "/mod.html", bar_from(p0))
    ck(wait_serial("MOD-MAIN-RAN", 120, "external module"),
       "the EXTERNAL <script type=\"module\"> was fetched and evaluated")
    ck(wait_serial("MOD-INLINE-RAN", 30, "inline module"),
       "the INLINE <script type=\"module\"> was evaluated")

    log = serial()
    ck("MOD-MAIN-RAN lib=JSDIR" in log,
       "the external module's './lib.mjs' resolved against ITS OWN URL "
       "(/js/lib.mjs, tag JSDIR) -- not against the document")
    ck("MOD-INLINE-RAN lib=ROOT" in log,
       "the inline module's identical './lib.mjs' resolved against the DOCUMENT "
       "(/lib.mjs, tag ROOT) -- the same specifier, a different file")
    ck("/js/lib.mjs" in requested and "/lib.mjs" in requested,
       "and the fixture server was really asked for BOTH files")
    ck("MOD-DOTDOT OK" in log and "/shared/util.mjs" in requested,
       "'../shared/util.mjs' resolved through the dot segment to /shared/util.mjs")

    # deferred: the classic script must have finished before any module started
    ck(log.index("MOD-CLASSIC-RAN") < log.index("MOD-MAIN-RAN"),
       "modules are DEFERRED: the classic script ran first")
    ck("MOD-ORDER CLASSIC" in log,
       "and the module observed the classic script's DOM mutation already applied")

    ck("skipping <script type=\"application/json\">" in log,
       "the <script type=\"application/json\"> data block was NOT executed")

    # ---- 2. the pixels -----------------------------------------------------
    time.sleep(2.5)
    p1 = PPM(ui.screendump(shot("p1")))

    b_ext = p1.find_color(ORANGE)
    ck(b_ext is not None,
       "the EXTERNAL module reached the PAINT -- a colour no stylesheet in the "
       "page mentions is on screen")
    ck(p1.find_color(RED) is None, "and the stylesheet's colour is gone from that box")
    after_ext = p1.dark_pixels(b_ext)
    ck(after_ext != before["ext"],
       "the imported function's textContent write reached the screen "
       "(text pixels %d -> %d)" % (before["ext"], after_ext))

    b_inl = p1.find_color(PURPLE)
    ck(b_inl is not None, "the INLINE module reached the PAINT too")
    after_inl = p1.dark_pixels(b_inl)
    ck(after_inl != before["inl"],
       "the inline module's DOM write reached the screen (text pixels %d -> %d)"
       % (before["inl"], after_inl))

    ck(p1.find_color(CYAN) is not None,
       "the data block's element is untouched -- nothing executed it")

    # ---- 3. dynamic import() ----------------------------------------------
    dyn_ok = wait_serial("MOD-DYNAMIC-RAN", 30, "dynamic import")
    if dyn_ok:
        time.sleep(2.0)
        p2 = PPM(ui.screendump(shot("p2")))
        b_dyn = p2.find_color(SPRING)
        ck(b_dyn is not None, "dynamic import() reached the PAINT")
        after_dyn = p2.dark_pixels(b_dyn)
        ck(after_dyn != before["dyn"],
           "the dynamically imported module's DOM write reached the screen "
           "(text pixels %d -> %d)" % (before["dyn"], after_dyn))
    else:
        print("NOTE: dynamic import() did not complete -- see the serial log")
        ck("MOD-DYNAMIC-FAILED" in serial(),
           "dynamic import() at least reported its own failure rather than hanging")

    # ---- 4. the connection pool -------------------------------------------
    # Seven resources over one origin. Without pooling that is seven connections.
    import re
    m = None
    for m in re.finditer(r"\[browser\] load done: (\d+) requests, (\d+) connections "
                         r"dialled, (\d+) reused", serial()):
        pass
    ck(m is not None, "the browser reported its per-load connection statistics")
    reqs, dials, reuses = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    print("    load: %d requests, %d dialled, %d reused" % (reqs, dials, reuses))
    ck(reuses > 0,
       "sub-resources rode POOLED connections (%d requests, %d dialled, %d reused)"
       % (reqs, dials, reuses))
    ck(dials < reqs,
       "fewer connections were opened than requests were made (%d < %d)" % (dials, reqs))

    print("\nPASS: ES modules run on the real machine, imports resolve against the "
          "importing module's URL, and the result is on screen")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
