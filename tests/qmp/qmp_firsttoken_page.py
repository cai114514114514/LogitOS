#!/usr/bin/env python3
"""The FIRST TOKEN of a model response, rendered on the page, from a real
free no-registration LLM endpoint -- or from the recorded oracle of one.

    python3 tests/qmp/qmp_firsttoken_page.py <iso> <disk.img> [--live]

THE CLAIM.  Not "a chat UI works": the minimal honest proof that LLM streaming
works end-to-end in this browser -- a model somewhere emitted a token, it
crossed TLS + CORS + HTTP chunking into a fetch() ReadableStream, the page
parsed the SSE frame, and the FIRST token's characters became pixels while the
response was still open.  Token #1, on the glass.

MODES.  Default (no flags) is the REPLAY: a second local server on a different
port (a different ORIGIN, so the request is genuinely cross-origin and is
preflighted exactly as the real one is) answers the preflight with the headers
the real endpoint sends and then drips the frames recorded in
tests/fixtures/firsttoken/oracle_body.sse, one every GAP seconds, holding the
response open until the last.  This is the ci-boot half: it proves the
parsing/rendering/streaming path regardless of the endpoint's uptime, and its
negative control (tests/firsttoken.mk, test-firsttoken-page-negctl) is THIS
SAME DRIVER run against a browser built with -DWEBAPI_NO_STREAM -- the
pre-streaming behaviour, where fetch registers no body sink and settles only
when the message completes.  Under it the mid-stream assertions below must go
red; that is the control's whole job.

--live drives the REAL endpoint, https://api.llm7.io/v1/chat/completions,
anonymous, over the guest's TLS/DNS/ALPN stack.  It is the acceptance run of
the package: the serial log carries FT- markers whose host-clock arrivals give
the request-to-first-token number the report quotes.

WHICH CHECKPOINTS SIT ON WHICH CLOCK.  The mid-stream checkpoint waits for the
SERVER to have written N frames and NOT finished -- never for the guest to
have seen anything -- because waiting for the guest would let a fully buffered
browser pass simply by being waited out.  Same rule as qmp_sse_page.py; that
file explains the instrument in more detail.
"""

import os
import sys
import subprocess
import tempfile
import threading
import time
import json
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM      # noqa: E402
import qmp_addrbar                  # noqa: E402  caret-derived address-bar geometry

ISO, DISK = sys.argv[1], sys.argv[2]
LIVE = "--live" in sys.argv[3:]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

RED = (254, 1, 2)

# The real endpoint and the request the page makes. The replay server serves
# oracle bytes for ANY request; the prompt is recorded beside the oracle.
LIVE_URL = "https://api.llm7.io/v1/chat/completions"

ORACLE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "fixtures", "firsttoken", "oracle_body.sse")

# The replay drip. Slow enough that "rendered before the response finished" is
# checkable with one screendump, fast enough that the whole run stays ~15 s.
GAP = 0.45
LEAD_IN = 1.5                      # headers, then a pause before frame 1

PAGE = """<!doctype html>
<html><head><title>firsttoken</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
#wrap { background: #fe0102; min-height: 48px; }
#out { display: block; margin: 0; font-size: 28px; color: #000000; }
</style></head><body>
<div id="wrap"><pre id="out"></pre></div>
<script>
console.log('FT-START fetch=' + (typeof fetch) + ' td=' + (typeof TextDecoder));
var out = document.getElementById('out');
var t0 = Date.now(), n = 0;
function ts() { return '@' + (Date.now() - t0); }
fetch('__URL__', { method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ model: 'meta-Llama-3.1-8B-Instruct-Turbo',
      messages: [{ role: 'user', content: 'Count from 1 to 8 slowly, comma separated' }],
      stream: true }) })
  .then(function (r) {
    console.log('FT-HEADERS ' + r.status + ' ' + (r.headers.get('content-type') || '?') + ' ' + ts());
    var rd = r.body.getReader(), dec = new TextDecoder(), buf = '';
    (function loop() {
      rd.read().then(function (c) {
        if (c.done) {
          console.log('FT-DONE ' + n + ' ' + ts());
          console.log('FT-TEXT ' + out.textContent);
          return;
        }
        var s = dec.decode(c.value, { stream: true });
        buf += s; var i;
        while ((i = buf.indexOf('\\n')) >= 0) {
          var ln = buf.slice(0, i); buf = buf.slice(i + 1);
          if (ln.charCodeAt(ln.length - 1) === 13) ln = ln.slice(0, -1);
          if (ln.indexOf('data:') !== 0) continue;
          var p = ln.slice(5).trim();
          if (!p || p === '[DONE]') continue;
          try {
            var d = JSON.parse(p);
            var tok = d.choices && d.choices[0] && d.choices[0].delta
                    ? d.choices[0].delta.content : null;
            if (tok) { n += 1; out.textContent += tok;
              console.log('FT-TOKEN ' + n + ' ' + JSON.stringify(tok) + ' ' + ts()); }
          } catch (e) { console.log('FT-PARSE-ERR ' + e); }
        }
        loop();
      }, function (e) { console.log('FT-READ-ERR ' + e); });
    })();
  }, function (e) { console.log('FT-FAIL ' + e.name + ': ' + e.message + ' ' + ts()); });
</script>
</body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_ft_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")   # noqa: E731

# ---- server state, all host-clock -------------------------------------------
seen = []            # (time, method, path) on the API server -- replay mode
frames_written = []  # host time each SSE frame was flushed -- replay mode
finished = {}        # name -> host time the response completed

ORACLE_BYTES = open(ORACLE, "rb").read()
FRAMES = [ln.encode() + b"\n\n" for ln in
          ORACLE_BYTES.decode().split("\n") if ln.startswith("data:")]
TOKENS = []          # the content tokens the oracle carries, for the assertion
for _f in FRAMES:
    p = _f.decode()[5:].strip()
    if not p or p == "[DONE]":
        continue
    _d = json.loads(p)
    _delta = (_d.get("choices") or [{}])[0].get("delta") or {}
    if _delta.get("content"):
        TOKENS.append(_delta["content"])
TOKEN_TEXT = "".join(TOKENS)


class PageSrv(http.server.BaseHTTPRequestHandler):
    """Origin A: serves the page with the right endpoint URL spliced in."""
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        url = LIVE_URL if LIVE else "http://10.0.2.2:%d/v1/chat/completions" % API_PORT
        raw = PAGE.replace("__URL__", url).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def log_message(self, *_a):
        pass


class ApiSrv(http.server.BaseHTTPRequestHandler):
    """Origin B: llm7's CORS shape, replaying the recorded oracle bytes.

    Deliberately on a SECOND PORT of the same machine: scheme+host+port is
    what an origin is, so the page's fetch is genuinely cross-origin, gets
    preflighted (the JSON content-type is not safelisted), and is only
    readable because this server opts in -- the same three facts the live
    endpoint exercises, minus TLS and the real model."""
    protocol_version = "HTTP/1.1"

    def _chunk(self, payload):
        self.wfile.write(b"%x\r\n" % len(payload) + payload + b"\r\n")
        self.wfile.flush()

    def do_OPTIONS(self):
        seen.append((time.time(), "OPTIONS", self.path))
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods",
                         "DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT")
        self.send_header("Access-Control-Allow-Headers", "content-type")
        self.send_header("Access-Control-Max-Age", "600")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        seen.append((time.time(), "POST", self.path))
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length:
            self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        try:
            time.sleep(LEAD_IN)
            for fr in FRAMES:
                self._chunk(fr)
                frames_written.append(time.time())
                time.sleep(GAP)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except OSError:
            pass
        finished["api"] = time.time()

    def log_message(self, *_a):
        pass


api = http.server.ThreadingHTTPServer(("0.0.0.0", 0), ApiSrv)
API_PORT = api.server_port
threading.Thread(target=api.serve_forever, daemon=True).start()
srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), PageSrv)
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
seen_at = {}


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def poll_markers():
    for ln in serial().splitlines():
        for key in ("FT-START", "FT-HEADERS", "FT-TOKEN", "FT-DONE",
                    "FT-FAIL", "FT-READ-ERR", "FT-PARSE-ERR"):
            if ln.startswith(key) and key not in seen_at:
                seen_at[key] = time.time()


def die(msg):
    print("FAIL: " + msg)
    for ok, name in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
    if seen_at:
        t0 = min(seen_at.values())
        print("----- marker arrivals (host clock, relative) -----")
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


def wait_frames(n, secs):
    """Checkpoint on the SERVER's clock: it has flushed n frames and has not
    finished. Waiting on the guest instead would wait out a buffered browser
    and then both implementations pass."""
    end = time.time() + secs
    while time.time() < end:
        poll_markers()
        if len(frames_written) >= n and "api" not in finished:
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for the replay frames")
        time.sleep(0.2)
    poll_markers()
    return False


try:
    if not wait_serial("LOGIT_BOOT_OK", 240, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 90, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path, serial=serial_path)
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(6)                          # ~3 MB .aex off virtio-blk + first paint

    bar = qmp_addrbar.focus(ui)
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/page.html" % PORT)
    qmp_addrbar.typed_echo(ui, bar)
    ui.key("ret")

    ck(wait_serial("FT-START", 120, "page load"),
       "the page loaded and its inline <script> ran")
    ck(line_after("FT-START ") and "fetch=function" in line_after("FT-START "),
       "fetch and TextDecoder exist")

    budget_hdr, budget_tok = (150.0, 90.0) if LIVE else (60.0, 60.0)

    ck(wait_serial("FT-HEADERS", int(budget_hdr), "the response headers"),
       "fetch() settled at the headers")
    hdr = line_after("FT-HEADERS ") or ""
    ck(hdr.startswith("200 "), "the endpoint answered 200 (%s)" % hdr)
    ck("text/event-stream" in hdr,
       "...as text/event-stream -- this is an SSE stream (%s)" % hdr)
    ck("FT-FAIL" not in serial() and "FT-READ-ERR" not in serial(),
       "no fetch/stream error on the way")

    if not LIVE:
        # The request really was cross-origin and really was preflighted:
        # replay origin B is a different port, so the JSON POST needed an
        # OPTIONS first. Order is asserted from the server's own log.
        methods = [(t, m) for (t, m, _p) in seen]
        pre = [t for (t, m) in methods if m == "OPTIONS"]
        post = [t for (t, m) in methods if m == "POST"]
        ck(len(pre) >= 1 and len(post) >= 1,
           "the replay API saw an OPTIONS preflight AND the POST (%d/%d)"
           % (len(pre), len(post)))
        ck(pre[0] < post[0],
           "...and the preflight happened BEFORE the POST")

    # ---- MID-STREAM: the first token, on the glass --------------------------
    p0 = PPM(ui.screendump(shot("p0")))
    box0 = p0.find_color(RED)
    ck(box0 is not None, "the token block is painted (empty) on screen")
    baseline = p0.dark_pixels(box0)

    if not LIVE:
        ck(wait_frames(3, 120),
           "the replay has flushed 3 of %d frames, response STILL OPEN"
           % len(FRAMES))
    else:
        end = time.time() + budget_tok
        while time.time() < end and "FT-TOKEN" not in serial():
            poll_markers()
            time.sleep(0.2)

    got_mid = "FT-TOKEN" in serial()
    time.sleep(1.2)                        # one repaint
    p1 = PPM(ui.screendump(shot("p1")))
    box1 = p1.find_color(RED)
    mid = p1.dark_pixels(box1) if box1 else -1
    print("   text pixels: baseline %d -> mid %d" % (baseline, mid))

    ck(got_mid, "the FIRST TOKEN had already reached the page by then")
    ck(mid > baseline,
       "THE FIRST TOKEN IS ON THE FRAMEBUFFER (text pixels %d -> %d)"
       % (baseline, mid))

    # ---- the end of the stream ----------------------------------------------
    ck(wait_serial("FT-DONE", 120, "the stream end"),
       "the response completed and the reader saw done")
    done = line_after("FT-DONE ") or ""
    ck(done.split() and done.split()[0].isdigit(),
       "FT-DONE carries a token count (%s)" % done)

    if not LIVE:
        txt = line_after("FT-TEXT ") or ""
        ck(txt == TOKEN_TEXT,
           "the rendered text is the oracle's reply, byte for byte (%r)" % txt)
    else:
        txt = line_after("FT-TEXT ") or ""
        ck(len(txt) > 0, "a non-empty model reply was rendered (%r)" % txt[:60])

    # ---- the timing ----------------------------------------------------------
    poll_markers()
    t_start = seen_at.get("FT-START")
    t_tok1 = seen_at.get("FT-TOKEN")
    t_done = seen_at.get("FT-DONE")
    if t_start and t_tok1 and t_done:
        print("\n   ---- timing (host clock, marker arrivals) ----")
        print("   page script started        : t+0.00s")
        print("   first token on the page    : t+%.2fs" % (t_tok1 - t_start))
        print("   stream complete            : t+%.2fs" % (t_done - t_start))
        if not LIVE:
            print("   (the replay drips %d frames over %.1fs)"
                  % (len(FRAMES), (len(FRAMES) - 1) * GAP))
            ck(t_tok1 < t_done - GAP,
               "the first token rendered BEFORE the response completed")

    if LIVE:
        print("\nPASS (live): a real model's FIRST TOKEN was fetched over TLS,")
        print("preflighted cross-origin, streamed, parsed and rendered in the")
        print("guest browser.")
    else:
        print("\nPASS (replay): the recorded oracle's first token crossed CORS +")
        print("chunked SSE + the fetch stream and reached the framebuffer while")
        print("the response was still open.")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
