#!/usr/bin/env python3
"""Prove on the real machine that the platform APIs reach the PIXELS.

    python3 tests/qmp/qmp_platform_page.py <iso> <disk.img> [--expect-none]

tests/unit/webapi_platform_test.c drives the same code host-side, which is
worth a lot and settles nothing about the machine: it does not go through the
.aex loader, the ring-3 heap, the browser's event loop, layout or the
framebuffer. This does.

The page is built so that the text on screen can ONLY appear if a chain of the
new APIs all worked: document.querySelectorAll finds the target,
performance.mark/measure produces a real duration, a MessageChannel delivers
the token as a macrotask, a MutationObserver observes the write, and
localStorage's named-property access carries the value between the two halves.
A screenshot before and after therefore measures the whole chain, not a
typeof.

--expect-none inverts it. That mode is the NEGATIVE CONTROL: run against a
browser.aex linked WITHOUT js_platform.o and js_select.o (make
test-platform-page-control), the page must report the globals missing and the
block on screen must not change.
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
EXPECT_NONE = "--expect-none" in sys.argv[3:]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

RED = (254, 1, 2)

# How long the page holds the chain before completing it, in ms. Long enough
# that the baseline screendump is unambiguously taken BEFORE the DOM write --
# see the comment on the chain in PAGE.
CHAIN_DELAY = 9000

# The whole page is one <script> with every section in its own try/catch, so
# the control run -- where the first section throws -- still reaches the last
# one and reports what it found rather than dying at line 3.
PAGE = """<!doctype html>
<html><head><title>platform</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 30px; color: #000000; }
#out { background: #fe0102; }
.probe { color: #000000; }
</style></head><body>
<div id="out" class="probe">WAITINGFORPLATFORM</div>
<div id="other" class="probe">second</div>
<script>
var CHAIN_DELAY = %d;
console.log('PLAT-START typeof-perfmark=' + (typeof (performance && performance.mark)) +
            ' typeof-readyState=' + (typeof document.readyState) +
            ' typeof-qsa=' + (typeof document.querySelectorAll) +
            ' typeof-MessageChannel=' + (typeof MessageChannel) +
            ' typeof-queueMicrotask=' + (typeof queueMicrotask) +
            ' typeof-structuredClone=' + (typeof structuredClone) +
            ' typeof-MutationObserver=' + (typeof MutationObserver));

/* The misses that must STAY misses -- reported so the machine confirms what
   the host test asserts: these are how a page detects IE, and defining them
   would send it down a path this browser cannot follow. */
console.log('PLAT-ABSENT ActiveXObject=' + (typeof window.ActiveXObject) +
            ' documentMode=' + (typeof document.documentMode) +
            ' indexedDB=' + (typeof window.indexedDB) +
            ' subtle=' + (typeof (window.crypto && window.crypto.subtle)));

try {
  console.log('PLAT-READY ' + document.readyState + '|' + document.visibilityState);
  document.addEventListener('DOMContentLoaded', function () {
    console.log('PLAT-DCL ' + document.readyState);
  });
} catch (e) { console.log('PLAT-READY-FAIL ' + e); }

try {
  console.log('PLAT-TIMING ' + (performance.timing.navigationStart > 1500000000000) + '|' +
              (performance.timing.responseEnd >= performance.timing.navigationStart));
} catch (e) { console.log('PLAT-TIMING-FAIL ' + e); }

try {
  console.log('PLAT-SEL ' + document.querySelectorAll('div.probe').length + '|' +
              document.getElementsByTagName('div').length + '|' +
              (document.getElementById('out').matches('div#out.probe')) + '|' +
              document.querySelectorAll('#out, #other').length);
} catch (e) { console.log('PLAT-SEL-FAIL ' + e); }

try {
  localStorage.clear();
  localStorage.platToken = 'PLATFORMOK';
  console.log('PLAT-STORE ' + localStorage.getItem('platToken') + '|' + localStorage.length);
} catch (e) { console.log('PLAT-STORE-FAIL ' + e); }

try {
  var c = structuredClone({ a: [1, 2], d: new Date(7), m: new Map([['k', 'v']]) });
  console.log('PLAT-CLONE ' + c.a[1] + '|' + c.d.getTime() + '|' + c.m.get('k'));
} catch (e) { console.log('PLAT-CLONE-FAIL ' + e); }

try {
  var r1 = new Uint8Array(8), r2 = new Uint8Array(8);
  crypto.getRandomValues(r1); crypto.getRandomValues(r2);
  var same = 0, zero = 0;
  for (var i = 0; i < 8; i++) { if (r1[i] === r2[i]) same++; if (r1[i] === 0) zero++; }
  console.log('PLAT-CRYPTO ' + (same < 6) + '|' + (zero < 4) + '|' +
              (crypto.randomUUID().length === 36));
} catch (e) { console.log('PLAT-CRYPTO-FAIL ' + e); }

try {
  var mu = 0;
  new MutationObserver(function (recs) {
    mu += recs.length;
    console.log('PLAT-MUT ' + mu + '|' + recs[0].type);
  }).observe(document.body, { childList: true, subtree: true, attributes: true });
  document.getElementById('other').setAttribute('data-x', '1');
} catch (e) { console.log('PLAT-MUT-FAIL ' + e); }

try {
  new IntersectionObserver(function (rs) {
    console.log('PLAT-IO ' + rs.length + '|' + rs[0].target.id + '|' +
                (rs[0].intersectionRatio >= 0));
  }).observe(document.getElementById('out'));
} catch (e) { console.log('PLAT-IO-FAIL ' + e); }

try {
  fetch('data:text/plain;base64,ZGF0YXVybG9r').then(function (r) { return r.text(); })
    .then(function (t) { console.log('PLAT-DATAURL ' + t); })
    .catch(function (e) { console.log('PLAT-DATAURL-FAIL ' + e); });
} catch (e) { console.log('PLAT-DATAURL-FAIL ' + e); }

/* THE CHAIN THAT HAS TO REACH THE SCREEN.
   performance.mark/measure -> queueMicrotask -> MessageChannel (a macrotask)
   -> localStorage named read -> querySelectorAll -> textContent. Every link is
   one of the APIs this change added; take any of them away and nothing is
   painted. */
try {
  performance.mark('chain-start');
  var ch = new MessageChannel();
  ch.port2.onmessage = function (ev) {
    performance.mark('chain-end');
    var m = performance.measure('chain', 'chain-start', 'chain-end');
    var token = localStorage.platToken;
    document.querySelectorAll('#out')[0].textContent = token;
    console.log('PLAT-PAINTED ' + token + '|' + ev.data + '|' +
                (typeof m.duration === 'number') + '|' + (m.duration >= CHAIN_DELAY));
  };
  /* The DELAY is what makes the screenshot mean something. Without it the whole
     chain finishes in under a frame, the baseline screendump is already the
     final text, and "the pixels did not change" would be indistinguishable
     from "they had already changed". The delay is also the measurement:
     performance.measure across it must report at least CHAIN_DELAY, which a
     mark/measure pair that returns 0 would not. */
  setTimeout(function () {
    queueMicrotask(function () { ch.port1.postMessage('viaport'); });
  }, CHAIN_DELAY);
} catch (e) { console.log('PLAT-CHAIN-FAIL ' + e); }
</script></body></html>
""" % CHAIN_DELAY

tmp = tempfile.mkdtemp(prefix="qmp_platform_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

requested = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        requested.append(self.path)
        raw = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
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


def die(msg):
    print("FAIL: " + msg)
    for ok, name in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-8000:])
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


def line_after(marker):
    for ln in serial().splitlines():
        i = ln.find(marker)
        if i >= 0:
            return ln[i + len(marker):].strip()
    return None


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
    ui.typ("http://10.0.2.2:%d/page.html" % PORT)
    ui.key("ret")

    ck(wait_serial("PLAT-START", 120, "page load"),
       "the page loaded and its inline <script> ran")
    start = line_after("PLAT-START ")
    print("   " + str(start))

    # The baseline screenshot, taken before the chain can have completed on a
    # working build and at any time on a broken one.
    time.sleep(1.0)
    p0 = PPM(ui.screendump(shot("p0")))
    box = p0.find_color(RED)
    ck(box is not None, "the fixture block is painted on screen")
    before = p0.dark_pixels(box)
    ck("PLAT-PAINTED" not in serial(),
       "the chain has NOT completed at baseline (the page holds it %.0fs)"
       % (CHAIN_DELAY / 1000.0))

    absent = line_after("PLAT-ABSENT ")
    ck(absent == "ActiveXObject=undefined documentMode=undefined "
                 "indexedDB=undefined subtle=undefined",
       "the four misses that must stay misses are still absent (%s)" % absent)

    if EXPECT_NONE:
        # ---- the negative control -------------------------------------
        ck("typeof-perfmark=undefined" in start,
           "CONTROL: a browser without js_platform.o has no performance.mark")
        ck("typeof-MessageChannel=undefined" in start,
           "CONTROL: ...no MessageChannel")
        ck("typeof-queueMicrotask=undefined" in start, "CONTROL: ...no queueMicrotask")
        ck("typeof-structuredClone=undefined" in start, "CONTROL: ...no structuredClone")
        ck("typeof-MutationObserver=undefined" in start, "CONTROL: ...no MutationObserver")
        ck("typeof-qsa=undefined" in start,
           "CONTROL: a browser without js_select.o has no querySelectorAll")
        ck("PLAT-PAINTED" not in serial(), "CONTROL: the chain never completed")
        time.sleep(12)
        p1 = PPM(ui.screendump(shot("c1")))
        after = p1.dark_pixels(p1.find_color(RED))
        ck(after == before,
           "CONTROL: the block never changed (text pixels %d -> %d)" % (before, after))
        print("\nPASS (negative control): without js_platform.o and js_select.o the "
              "page cannot complete the chain, so the positive run is measuring "
              "exactly this change")
        proc.kill()
        sys.exit(0)

    # ---- 1. the surface exists ----------------------------------------
    for name in ("perfmark=function", "readyState=string", "qsa=function",
                 "MessageChannel=function", "queueMicrotask=function",
                 "structuredClone=function", "MutationObserver=function"):
        ck("typeof-" + name in start, name.split("=")[0] + " is present")

    # ---- 2. what each one actually answered ---------------------------
    rd = line_after("PLAT-READY ")
    ck(rd == "loading|visible",
       "readyState is 'loading' while the page's own script runs (%s)" % rd)
    ck(wait_serial("PLAT-DCL", 40, "DOMContentLoaded"),
       "DOMContentLoaded fired and the state machine moved")
    ck(line_after("PLAT-DCL ") in ("interactive", "complete"),
       "...to interactive/complete (%s)" % line_after("PLAT-DCL "))

    tm = line_after("PLAT-TIMING ")
    ck(tm == "true|true",
       "performance.timing carries a real epoch and ordered phases (%s)" % tm)

    sel = line_after("PLAT-SEL ")
    ck(sel == "2|2|true|2",
       "the selector engine answers over the real DOM (%s)" % sel)

    st = line_after("PLAT-STORE ")
    ck(st == "PLATFORMOK|1",
       "localStorage.<name> = v really wrote through setItem (%s)" % st)

    cl = line_after("PLAT-CLONE ")
    ck(cl == "2|7|v", "structuredClone kept Date and Map (%s)" % cl)

    cr = line_after("PLAT-CRYPTO ")
    ck(cr == "true|true|true",
       "getRandomValues differs between calls on the machine (%s)" % cr)

    ck(wait_serial("PLAT-MUT", 40, "MutationObserver"),
       "a MutationObserver saw the attribute write")
    ck(line_after("PLAT-MUT ") == "1|attributes",
       "...as one attributes record (%s)" % line_after("PLAT-MUT "))

    ck(wait_serial("PLAT-IO", 40, "IntersectionObserver"),
       "an IntersectionObserver delivered its initial entry")
    io = line_after("PLAT-IO ")
    ck(io == "1|out|true", "...for the observed target, with a ratio (%s)" % io)

    ck(wait_serial("PLAT-DATAURL", 40, "data: URL"), "fetch() of a data: URL resolved")
    ck(line_after("PLAT-DATAURL ") == "dataurlok",
       "...to the right bytes, with no socket involved (%s)" % line_after("PLAT-DATAURL "))
    ck(not any("dataurl" in r for r in requested),
       "the host server was never asked for the data: URL")

    # ---- 3. the chain, on the screen ----------------------------------
    ck(wait_serial("PLAT-PAINTED", 60, "the chain"),
       "the mark -> microtask -> MessagePort -> storage -> selector chain completed")
    pn = line_after("PLAT-PAINTED ")
    ck(pn == "PLATFORMOK|viaport|true|true",
       "every link carried its value, and performance.measure spans the real "
       "delay rather than returning 0 (%s)" % pn)

    time.sleep(2.0)
    p1 = PPM(ui.screendump(shot("p1")))
    box1 = p1.find_color(RED)
    ck(box1 is not None, "the block is still on screen after the mutation")
    after = p1.dark_pixels(box1)
    ck(after != before,
       "THE CHAIN'S OUTPUT REACHED THE PIXELS (text pixels %d -> %d)" % (before, after))

    print("\nPASS: performance, the document lifecycle, the task queues, Storage "
          "named properties, crypto, structuredClone, the observers, data: URLs and "
          "the selector engine all work on the real machine, and their output "
          "reached the framebuffer")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
