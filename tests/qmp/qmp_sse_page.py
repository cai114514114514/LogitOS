#!/usr/bin/env python3
"""Prove, on the real machine and WITH TIMESTAMPS, that a token stream streams.

    python3 tests/qmp/qmp_sse_page.py <iso> <disk.img> [--expect-buffered]

WHY THIS IS TIMED.  "The tokens arrived" is not the claim.  A fetch that buffers
the entire body and resolves at the end also makes every token arrive -- it just
makes them all arrive at once, at the end, which is why a chat reply from Kimi
or DeepSeek is unusable against it even though every byte is correct.  The claim
is about WHEN bytes become visible, so this harness measures when.

The host fixture emits five SSE tokens with a deliberate gap between them and
does not close the response until the last one.  Three independent instruments
have to agree:

  1. THE SERIAL LOG, timestamped by the HOST as each line appears.  Token 1 must
     be logged many seconds before the response completes, and the spread
     between the first and last token must be most of the fixture's duration.
     Tokens that all appear inside one second are a buffered fetch.
  2. THE FRAMEBUFFER.  A screendump taken mid-stream -- while the server is
     provably still holding the connection open, which the fixture records --
     must differ from the baseline.  Pixels are the only channel that cannot be
     faked by a page lying to itself in console.log.
  3. THE SERVER'S OWN LOG.  A request the guest never made cannot appear there,
     and the moment the fixture finished writing is recorded from the host's
     clock, not the guest's.

--expect-buffered is the NEGATIVE CONTROL.  Run against a browser.aex whose
js_webapi.c was built with -DWEBAPI_NO_STREAM (make test-sse-page-control), the
same page must show NOTHING mid-stream and all its tokens must land in a burst
after the response completed.  If the control ever passes the positive
assertions, this harness is measuring something other than streaming.
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
EXPECT_BUFFERED = "--expect-buffered" in sys.argv[3:]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

RED = (254, 1, 2)

# The fixture's shape. Five tokens, GAP seconds apart; the response is not
# closed until after the last one. Generous because the guest is an emulated
# CPU doing layout and text rasterisation between tokens.
TOKENS = ["ALPHA", "BRAVO", "CHARLIE", "DELTA", "ECHO"]
GAP = 4.0
LEAD_IN = 2.0                    # headers, then a pause before token 1

PAGE = """<!doctype html>
<html><head><title>sse</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 28px; color: #000000; }
#out { background: #fe0102; min-height: 40px; }
</style></head><body>
<div id="out"></div>
<script>
console.log('SSE-START typeof-EventSource=' + (typeof EventSource) +
            ' typeof-ReadableStream=' + (typeof ReadableStream) +
            ' typeof-TextDecoderStream=' + (typeof TextDecoderStream) +
            ' typeof-AbortController=' + (typeof AbortController));

var out = document.getElementById('out');
var got = [];
var t0 = Date.now();
function ts() { return '@' + (Date.now() - t0); }

function paint() { out.textContent = got.join(' '); }

if (typeof EventSource !== 'function') {
  console.log('SSE-NO-EVENTSOURCE');
} else {
  var es = new EventSource('/stream');
  es.onopen = function () { console.log('SSE-OPEN ' + ts()); };
  es.onmessage = function (e) {
    got.push(e.data);
    paint();
    console.log('SSE-TOKEN ' + got.length + ' ' + e.data + ' id=' + e.lastEventId + ' ' + ts());
  };
  es.addEventListener('done', function (e) {
    console.log('SSE-DONE ' + got.length + ' ' + got.join(',') + ' ' + ts());
    es.close();
  });
  es.onerror = function () { console.log('SSE-ERROR state=' + es.readyState); };
}

/* Cookies, end to end on the real machine. The host tests drive document.cookie
   against a synthetic document object; here it is js_dom.c's real one, which is
   the only place the accessor pair can actually be wrong. */
if (typeof fetch === 'function') {
  fetch('/setcookie').then(function (r) { return r.text(); }).then(function () {
    console.log('COOKIE-JS ' + document.cookie);
    return fetch('/whatcookie').then(function (r) { return r.text(); });
  }).then(function (t) {
    console.log('COOKIE-WIRE ' + t);
    document.cookie = 'fromjs=42; Path=/';
    return fetch('/whatcookie').then(function (r) { return r.text(); });
  }).then(function (t) {
    console.log('COOKIE-WIRE2 ' + t);
  }).catch(function (e) { console.log('COOKIE-FAIL ' + e); });
}

/* CORS, enforced on the device and not merely in a unit test. __ALT__ is a
   SECOND server on a different port: same machine, different origin. Two
   fetches, and the pair is the point -- if only the refusal were checked, an
   unreachable host would pass it. The allowed one proves the origin is
   reachable, so the refusal is attributable to CORS and not to the network. */
if (typeof fetch === 'function') {
  fetch('http://__ALT__/nocors')
    .then(function (r) { return r.text(); })
    .then(function (t) { console.log('CORS-LEAKED ' + t); },
          function (e) { console.log('CORS-REFUSED ' + e.name); });
  fetch('http://__ALT__/withcors')
    .then(function (r) { return r.text(); })
    .then(function (t) { console.log('CORS-ALLOWED ' + t); },
          function (e) { console.log('CORS-ALLOWED-FAIL ' + e.name); });
}

/* The same stream read through fetch + a reader, so the ReadableStream half is
   exercised on the device too rather than only in the host tests. */
if (typeof fetch === 'function') {
  fetch('/stream2').then(function (r) {
    console.log('SSE-FETCH-HEADERS ' + r.status + ' ' + (r.body ? 'stream' : 'nobody') + ' ' + ts());
    var rd = r.body.getReader(), dec = new TextDecoder(), n = 0;
    (function loop() {
      rd.read().then(function (c) {
        if (c.done) { console.log('SSE-FETCH-END ' + n); return; }
        n += 1;
        console.log('SSE-FETCH-CHUNK ' + n + ' ' + dec.decode(c.value, { stream: true }).replace(/\\r?\\n/g, '|'));
        loop();
      }, function (e) { console.log('SSE-FETCH-ERR ' + e); });
    })();
  }, function (e) { console.log('SSE-FETCH-FAIL ' + e); });
}
</script>
</body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_sse_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

requested = []
# When the fixture finished writing each stream, by the HOST clock. Until a
# name appears here, that response is still open -- which is what makes
# "partial content was on screen before the response completed" checkable.
stream_started = {}
stream_finished = {}
stream_token_at = {}


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _sse(self, name):
        stream_started[name] = time.time()
        stream_token_at[name] = []
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        def chunk(payload):
            raw = payload.encode()
            self.wfile.write(b"%x\r\n" % len(raw) + raw + b"\r\n")
            self.wfile.flush()

        try:
            time.sleep(LEAD_IN)
            for i, tok in enumerate(TOKENS):
                chunk("id: %d\ndata: %s\n\n" % (i + 1, tok))
                stream_token_at[name].append(time.time())
                if i + 1 < len(TOKENS):
                    time.sleep(GAP)
            chunk("event: done\ndata: end\n\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except OSError:
            pass
        stream_finished[name] = time.time()

    def do_GET(self):
        requested.append(self.path)
        if self.path.startswith("/stream2"):
            self._sse("stream2")
            return
        if self.path.startswith("/stream"):
            self._sse("stream")
            return
        if self.path.startswith("/setcookie"):
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            # One readable, one HttpOnly: document.cookie must see the first and
            # not the second, and the WIRE must carry both.
            self.send_header("Set-Cookie", "sid=on-device; Path=/")
            self.send_header("Set-Cookie", "hidden=secret; Path=/; HttpOnly")
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")
            return
        if self.path.startswith("/whatcookie"):
            seen = self.headers.get("Cookie", "<none>")
            raw = seen.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return
        if self.path.startswith("/page"):
            raw = PAGE.replace("__ALT__", "10.0.2.2:%d" % ALT_PORT).encode()
            ctype = "text/html"
            code = 200
        else:
            raw = b"not found\n"
            ctype = "text/plain"
            code = 404
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def log_message(self, *_a):
        pass


# The SECOND origin. Same machine, different port -- which is a different origin
# by the only definition that matters (scheme, host, port), and reachable, which
# is what makes a refusal attributable to CORS rather than to the network.
alt_requested = []


class AltOrigin(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        alt_requested.append(self.path)
        if self.path.startswith("/withcors"):
            raw, extra = b"CROSSORIGINALLOWED", [("Access-Control-Allow-Origin", "*")]
        else:
            raw, extra = b"CROSSORIGINSECRET", []      # deliberately no ACAO
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        for k, v in extra:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def log_message(self, *_a):
        pass


alt = http.server.ThreadingHTTPServer(("0.0.0.0", 0), AltOrigin)
ALT_PORT = alt.server_port
threading.Thread(target=alt.serve_forever, daemon=True).start()

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
# When each serial marker FIRST appeared, by the host clock. This is the
# instrument: the guest's own clock has 1-second resolution and can be set
# backwards, so the timing claim is made from outside the machine.
seen_at = {}


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def poll_markers():
    """Record the host-clock arrival of every marker line we care about."""
    txt = serial()
    now = time.time()
    for ln in txt.splitlines():
        for key in ("SSE-START", "SSE-OPEN", "SSE-DONE", "SSE-ERROR",
                    "SSE-NO-EVENTSOURCE", "SSE-FETCH-HEADERS", "SSE-FETCH-END"):
            if key in ln and key not in seen_at:
                seen_at[key] = now
        if "SSE-TOKEN " in ln:
            n = ln.split("SSE-TOKEN ", 1)[1].split()[0]
            seen_at.setdefault("TOKEN" + n, now)
        if "SSE-FETCH-CHUNK " in ln:
            n = ln.split("SSE-FETCH-CHUNK ", 1)[1].split()[0]
            seen_at.setdefault("CHUNK" + n, now)


def die(msg):
    print("FAIL: " + msg)
    for ok, name in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
    print("----- marker arrival times (host clock, relative) -----")
    if seen_at:
        t0 = min(seen_at.values())
        for k, v in sorted(seen_at.items(), key=lambda kv: kv[1]):
            print("   %+7.2fs  %s" % (v - t0, k))
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
        poll_markers()
        if needle in serial():
            poll_markers()
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.2)
    poll_markers()
    return False


def line_after(marker):
    for ln in serial().splitlines():
        i = ln.find(marker)
        if i >= 0:
            return ln[i + len(marker):].strip()
    return None


def token_lines():
    return [ln for ln in serial().splitlines() if "SSE-TOKEN " in ln]


def wait_server_tokens(n, secs):
    """Wait until the FIXTURE has written n tokens. The checkpoint has to be on
    the server's clock: a checkpoint that waits for the guest would wait out a
    buffered browser too, and then both implementations pass."""
    end = time.time() + secs
    while time.time() < end:
        poll_markers()
        if len(stream_token_at.get("stream", [])) >= n:
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for the fixture's tokens")
        time.sleep(0.2)
    return False


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
    time.sleep(6)                          # ~3 MB .aex off virtio-blk + first paint

    ui.click_at(420, 145)                  # the address bar
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/page.html" % PORT)
    ui.key("ret")

    ck(wait_serial("SSE-START", 120, "page load"),
       "the page loaded and its inline <script> ran")
    start = line_after("SSE-START ")
    print("   " + str(start))
    ck("typeof-EventSource=function" in start, "EventSource exists")
    ck("typeof-ReadableStream=function" in start, "ReadableStream exists")
    ck("typeof-TextDecoderStream=function" in start, "TextDecoderStream exists")
    ck("typeof-AbortController=function" in start, "AbortController exists")

    # The baseline is gated on the SERVER having the request, NOT on the page's
    # onopen. A buffered browser does not fire onopen until the response
    # completes, so waiting for it here would sit out the entire stream and
    # every mid-stream check below would be taken after the end.
    end = time.time() + 60
    while time.time() < end and not any("/stream" in r for r in requested):
        time.sleep(0.2)
    ck(any("/stream" in r for r in requested),
       "the HOST SERVER saw the request -- it really left the machine")

    # ---- the baseline, taken while the block is still empty ------------
    p0 = PPM(ui.screendump(shot("p0")))
    box = p0.find_color(RED)
    ck(box is not None, "the fixture block is painted on screen")
    baseline = p0.dark_pixels(box)
    print("   baseline text pixels in the block: %d" % baseline)

    # ---- MID-STREAM: the whole point ----------------------------------
    # The checkpoint is on the SERVER's clock, not the guest's, and it is the
    # same checkpoint in both directions: "the fixture has written three of five
    # tokens and is still holding the response open". Asking instead whether the
    # guest has seen a token would let a buffered browser pass simply by being
    # waited for long enough -- which is precisely the failure mode this control
    # exists to rule out.
    ck(wait_server_tokens(3, 120), "the fixture has written three of five tokens")
    poll_markers()
    ck("stream" not in stream_finished,
       "...and the SERVER HAS NOT FINISHED WRITING the response")

    got_mid = any("SSE-TOKEN " in ln for ln in serial().splitlines())
    time.sleep(1.2)                        # one repaint
    p1 = PPM(ui.screendump(shot("p1" if not EXPECT_BUFFERED else "c1")))
    mid = p1.dark_pixels(p1.find_color(RED))
    print("   mid-stream text pixels: %d (baseline %d)" % (mid, baseline))
    still_open = "stream" not in stream_finished

    if not EXPECT_BUFFERED:
        ck(got_mid, "tokens had already reached the page at that moment")
        ck(still_open, "the response was STILL OPEN when the screenshot was taken")
        ck(mid > baseline,
           "PARTIAL CONTENT IS ON THE FRAMEBUFFER WHILE THE RESPONSE IS STILL OPEN "
           "(text pixels %d -> %d)" % (baseline, mid))
    else:
        ck(not got_mid,
           "CONTROL: not one token had reached the page while the response was open")
        ck(mid == baseline,
           "CONTROL: the block never changed mid-stream (text pixels %d -> %d)"
           % (baseline, mid))

    # ---- let it finish -------------------------------------------------
    # ---- cookies + CORS on the real machine ---------------------------
    ck(wait_serial("COOKIE-JS ", 60, "document.cookie"),
       "document.cookie is readable on js_dom's real document object")
    cj = line_after("COOKIE-JS ")
    ck(cj and "sid=on-device" in cj,
       "...and it shows the cookie the response set (%s)" % cj)
    ck(cj and "hidden" not in cj,
       "...and NOT the HttpOnly one (%s)" % cj)

    ck(wait_serial("COOKIE-WIRE ", 60, "the Cookie header"), "a later request carried Cookie")
    cw = line_after("COOKIE-WIRE ")
    ck(cw and "sid=on-device" in cw and "hidden=secret" in cw,
       "...with BOTH cookies -- the network may see HttpOnly (%s)" % cw)

    ck(wait_serial("COOKIE-WIRE2 ", 60, "the document.cookie write"),
       "document.cookie can be written")
    cw2 = line_after("COOKIE-WIRE2 ")
    ck(cw2 and "fromjs=42" in cw2,
       "...and the write reached the wire on the next request (%s)" % cw2)

    ck(wait_serial("CORS-ALLOWED ", 60, "the allowed cross-origin fetch"),
       "the second origin is reachable from the guest")
    ck(line_after("CORS-ALLOWED ") == "CROSSORIGINALLOWED",
       "...and a cross-origin response that DOES send Allow-Origin is readable")
    ck(wait_serial("CORS-REFUSED ", 60, "the refused cross-origin fetch"),
       "the cross-origin fetch without Allow-Origin settled")
    ck("CORS-LEAKED" not in serial(),
       "CORS IS ENFORCED ON THE DEVICE: a cross-origin response with no "
       "Access-Control-Allow-Origin did not reach script")
    ck(line_after("CORS-REFUSED ") == "TypeError",
       "...it was refused with a TypeError, as fetch specifies")
    ck(any("/nocors" in r for r in alt_requested),
       "...and the request really WAS made -- the refusal is CORS, not the network")

    ck(wait_serial("SSE-OPEN", 120, "the SSE connection"),
       "EventSource connected and its onopen fired")
    ck(wait_serial("SSE-DONE", 120, "the end of the stream"),
       "the stream ended and the page saw its 'done' event")
    poll_markers()
    done_line = line_after("SSE-DONE ")
    ck(done_line and done_line.startswith(str(len(TOKENS))),
       "every token arrived, in order (%s)" % done_line)
    ck("stream" in stream_finished, "the server finished writing the response")

    # ---- the timing, stated in seconds ---------------------------------
    t_tok1 = seen_at.get("TOKEN1")
    t_tok5 = seen_at.get("TOKEN%d" % len(TOKENS))
    t_end = stream_finished["stream"]
    ck(t_tok1 is not None and t_tok5 is not None, "both end tokens were timed")

    lead = t_end - t_tok1
    spread = t_tok5 - t_tok1
    fixture_span = stream_token_at["stream"][-1] - stream_token_at["stream"][0]
    print("\n   ---- timing (host clock) ----")
    print("   token 1 visible to the page  : t+0.00s")
    print("   token %d visible to the page  : t+%.2fs" % (len(TOKENS), spread))
    print("   server finished the response : t+%.2fs" % lead)
    print("   (the fixture spread its tokens over %.2fs)" % fixture_span)

    if not EXPECT_BUFFERED:
        ck(lead > fixture_span * 0.6,
           "TOKEN 1 REACHED THE PAGE %.1fs BEFORE THE RESPONSE COMPLETED" % lead)
        ck(spread > fixture_span * 0.5,
           "the tokens arrived SPREAD OVER %.1fs, not in one burst -- a buffered "
           "fetch delivers them all within a frame of each other" % spread)
        ck(wait_serial("SSE-FETCH-CHUNK 1", 30, "a fetch() body chunk"),
           "fetch() + Response.body.getReader() also delivered chunks incrementally")
        ck(seen_at.get("SSE-FETCH-HEADERS") is not None and
           seen_at["SSE-FETCH-HEADERS"] < seen_at.get("CHUNK1", 1e18),
           "...and its promise settled at the HEADERS, before any body chunk")
    else:
        ck(spread < 2.0,
           "CONTROL: every token appeared within %.2fs of the first -- one burst, "
           "at the end" % spread)
        ck(t_tok1 > t_end - 2.0,
           "CONTROL: the first token was not visible until the response had "
           "completed (%.2fs after)" % (t_tok1 - t_end))

    if EXPECT_BUFFERED:
        print("\nPASS (negative control): with -DWEBAPI_NO_STREAM the same page shows "
              "nothing until the response completes, so the positive run above is "
              "measuring exactly this change")
    else:
        print("\nPASS: an SSE endpoint delivers tokens to the DOM and to the "
              "framebuffer while the response is still open")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
