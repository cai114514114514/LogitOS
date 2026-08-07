#!/usr/bin/env python3
"""Prove, on the real machine, that a page's fetch() reaches the PIXELS.

    python3 tests/qmp/qmp_webapi_page.py <iso> <disk.img> [--expect-no-webapi]

The host tests in tests/unit/webapi_test.c drive the same code against an
in-memory server, which is worth a lot and settles nothing about the machine:
they do not go through the socket syscalls, the TCP stack, the e1000, the
browser's event loop, layout or the framebuffer. This does. A page loaded over
the network calls fetch(), writes the response into the DOM, and the harness
requires the text pixels inside a known-coloured block to CHANGE -- which is
only true if the promise resolved from the browser's main loop, the DOM
mutation invalidated layout, and the repaint happened.

Every claim is made from two independent channels: the screen, and the host web
server's own request log (a request the guest never made cannot appear there,
and console.log alone could be a page lying to itself).

--expect-no-webapi inverts the fetch assertions. That mode is the NEGATIVE
CONTROL: run against a browser.aex linked WITHOUT js_webapi.c (make
test-webapi-page-control), the page must report `typeof fetch === "undefined"`
and the block on screen must not change. If the control ever passes the
positive assertions, this harness is measuring something other than the change.
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
EXPECT_NONE = "--expect-no-webapi" in sys.argv[3:]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

RED = (254, 1, 2)

# The response the page has to go and get. It is deliberately delayed so the
# baseline screenshot is unambiguously taken BEFORE it arrives -- otherwise
# "the pixels did not change" could mean "they had already changed".
DATA_DELAY = 7.0
DATA = '{"word":"FETCHEDOK","n":41}'

PAGE = """<!doctype html>
<html><head><title>webapi</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 30px; color: #000000; }
#out { background: #fe0102; }
</style></head><body>
<div id="out">WAITINGFORDATA</div>
<script>
console.log('WEBAPI-START typeof-fetch=' + (typeof fetch) +
            ' typeof-localStorage=' + (typeof localStorage) +
            ' typeof-URL=' + (typeof URL) +
            ' typeof-history-pushState=' + (typeof (window.history && history.pushState)));

/* location, parsed -- an SPA routes on these before it renders anything. */
try {
  console.log('WEBAPI-LOCATION ' + location.protocol + '|' + location.hostname + '|' +
              location.port + '|' + location.pathname + '|' + location.origin);
} catch (e) { console.log('WEBAPI-LOCATION-FAIL ' + e); }

/* URL + URLSearchParams over the page's own address. */
try {
  var u = new URL('/a/b?x=1&y=2#frag', location.href);
  console.log('WEBAPI-URL ' + u.pathname + '|' + u.searchParams.get('y') + '|' + u.hash +
              '|' + u.host);
} catch (e) { console.log('WEBAPI-URL-FAIL ' + e); }

/* Storage. */
try {
  localStorage.clear();
  localStorage.setItem('tok', 'abc');
  localStorage.setItem('n', 7);
  console.log('WEBAPI-STORAGE ' + localStorage.getItem('tok') + '|' +
              localStorage.getItem('n') + '|' + localStorage.length + '|' +
              localStorage.getItem('missing'));
} catch (e) { console.log('WEBAPI-STORAGE-FAIL ' + e); }

/* history + popstate: the SPA routing path. */
try {
  window.onpopstate = function (e) {
    console.log('WEBAPI-POPSTATE ' + (e.state ? e.state.n : 'null') + '|' + location.pathname);
  };
  history.pushState({ n: 1 }, '', '/r1');
  history.pushState({ n: 2 }, '', '/r2');
  console.log('WEBAPI-HISTORY ' + location.pathname + '|' + history.length + '|' +
              (history.state && history.state.n));
  history.back();
} catch (e) { console.log('WEBAPI-HISTORY-FAIL ' + e); }

/* try/catch on every section, so the --expect-no-webapi run reaches the fetch
   branch below instead of dying on the first missing global. */
try {
  console.log('WEBAPI-MEDIA ' + matchMedia('(min-width: 400px)').matches + '|' +
              matchMedia('(min-width: 9000px)').matches);
} catch (e) { console.log('WEBAPI-MEDIA-FAIL ' + e); }

/* The one that has to reach the screen. */
if (typeof fetch !== 'function') {
  console.log('WEBAPI-NO-FETCH');
} else {
  fetch('/data.json', { headers: { 'X-From': 'logit' } })
    .then(function (r) {
      console.log('WEBAPI-STATUS ' + r.status + '|' + r.ok + '|' +
                  r.headers.get('Content-Type') + '|' + r.url);
      return r.json();
    })
    .then(function (j) {
      document.getElementById('out').textContent = j.word;
      console.log('WEBAPI-FETCHED ' + j.word + '|' + j.n);
    })
    .catch(function (e) { console.log('WEBAPI-FETCH-FAIL ' + e); });
}
</script>
</body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_webapi_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

requested = []
req_headers = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        requested.append(self.path)
        if self.path.startswith("/data.json"):
            # Lower-cased keys: Headers normalizes names on the way out, so the
            # request carries `x-from`, not `X-From`.
            req_headers.append({k.lower(): v for k, v in self.headers.items()})
            time.sleep(DATA_DELAY)             # see DATA_DELAY
            raw = DATA.encode()
            ctype = "application/json"
        elif self.path.startswith("/page"):
            raw = PAGE.encode()
            ctype = "text/html"
        else:
            raw = b"not found\n"
            ctype = "text/plain"
        self.send_response(200 if raw != b"not found\n" else 404)
        self.send_header("Content-Type", ctype)
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


def line_after(marker):
    """The rest of the serial line that begins with `marker`."""
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
    time.sleep(6)                          # ~2.8 MB .aex off virtio-blk + first paint

    ui.click_at(420, 145)                  # the address bar
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/page.html" % PORT)
    ui.key("ret")

    ck(wait_serial("WEBAPI-START", 90, "page load"),
       "the page loaded and its inline <script> ran")
    start = line_after("WEBAPI-START ")
    print("   " + str(start))

    if EXPECT_NONE:
        # ---- the negative control -------------------------------------
        ck("typeof-fetch=undefined" in start,
           "CONTROL: a browser without js_webapi.c has no fetch")
        ck(wait_serial("WEBAPI-NO-FETCH", 20, "no-fetch marker"),
           "CONTROL: the page took its no-fetch branch")
        time.sleep(3)
        p0 = PPM(ui.screendump(shot("c0")))
        box = p0.find_color(RED)
        ck(box is not None, "CONTROL: the fixture block is on screen")
        before = p0.dark_pixels(box)
        time.sleep(DATA_DELAY + 12)
        p1 = PPM(ui.screendump(shot("c1")))
        after = p1.dark_pixels(p1.find_color(RED))
        ck(after == before,
           "CONTROL: the block never changed (text pixels %d -> %d)" % (before, after))
        ck(not any("data.json" in r for r in requested),
           "CONTROL: /data.json was never requested")
        print("\nPASS (negative control): without js_webapi.c the page cannot fetch, "
              "so the positive run below is measuring exactly this change")
        proc.kill()
        sys.exit(0)

    # ---- 1. the surface exists ----------------------------------------
    ck("typeof-fetch=function" in start, "fetch is a function")
    ck("typeof-localStorage=object" in start, "localStorage is an object")
    ck("typeof-URL=function" in start, "URL is a constructor")
    ck("typeof-history-pushState=function" in start, "history.pushState exists")
    ck("WEBAPI-NO-FETCH" not in serial(), "the page did NOT take its no-fetch branch")

    # ---- 2. location, URL, storage, history, media --------------------
    loc = line_after("WEBAPI-LOCATION ")
    ck(loc == "http:|10.0.2.2|%d|/page.html|http://10.0.2.2:%d" % (PORT, PORT),
       "location is parsed into components on the real page URL (%s)" % loc)

    url = line_after("WEBAPI-URL ")
    ck(url == "/a/b|2|#frag|10.0.2.2:%d" % PORT,
       "URL + URLSearchParams resolve against the document (%s)" % url)

    st = line_after("WEBAPI-STORAGE ")
    ck(st == "abc|7|2|null",
       "localStorage stores, coerces to string, counts and misses (%s)" % st)

    hs = line_after("WEBAPI-HISTORY ")
    ck(hs == "/r2|3|2", "pushState rewrote the path without navigating (%s)" % hs)
    ck(not any(r.startswith("/r1") or r.startswith("/r2") for r in requested),
       "...and the server was never asked for the pushed URLs")
    ck(wait_serial("WEBAPI-POPSTATE", 20, "popstate"), "history.back() fired popstate")
    ck(line_after("WEBAPI-POPSTATE ") == "1|/r1",
       "popstate carried the entry's state and the URL went back (%s)"
       % line_after("WEBAPI-POPSTATE "))

    md = line_after("WEBAPI-MEDIA ")
    ck(md == "true|false", "matchMedia answers against the real viewport (%s)" % md)

    # ---- 3. the fetch, on the screen ----------------------------------
    time.sleep(1.5)
    p0 = PPM(ui.screendump(shot("p0")))
    box = p0.find_color(RED)
    ck(box is not None, "the fixture block is painted on screen")
    before = p0.dark_pixels(box)
    ck("WEBAPI-FETCHED" not in serial(),
       "the fetch has NOT resolved yet at baseline (the server holds it %.0fs)" % DATA_DELAY)

    ck(wait_serial("WEBAPI-STATUS", 60, "fetch response"), "the fetch got a response")
    stat = line_after("WEBAPI-STATUS ")
    ck(stat == "200|true|application/json|http://10.0.2.2:%d/data.json" % PORT,
       "status/ok/headers.get/url are all right (%s)" % stat)
    ck(any("data.json" in r for r in requested),
       "the HOST SERVER saw the request -- it really left the machine")
    ck(req_headers and req_headers[0].get("x-from") == "logit",
       "the caller's request header arrived at the server")
    ck(req_headers and req_headers[0].get("host") == "10.0.2.2:%d" % PORT,
       "Host was built from the URL")
    ck(req_headers and "gzip" in (req_headers[0].get("accept-encoding") or ""),
       "and Accept-Encoding advertises what the client can actually decode")

    ck(wait_serial("WEBAPI-FETCHED", 30, "json + DOM write"),
       "json() parsed the body and the handler wrote it into the DOM")
    ck(line_after("WEBAPI-FETCHED ") == "FETCHEDOK|41", "the parsed values are right")

    time.sleep(2.0)
    p1 = PPM(ui.screendump(shot("p1")))
    box1 = p1.find_color(RED)
    ck(box1 is not None, "the block is still on screen after the mutation")
    after = p1.dark_pixels(box1)
    ck(after != before,
       "THE FETCHED TEXT REACHED THE PIXELS (text pixels %d -> %d)" % (before, after))

    print("\nPASS: fetch(), Storage, history, location, URL and matchMedia all work "
          "on the real machine, and the fetched body reached the framebuffer")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
