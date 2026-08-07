#!/usr/bin/env python3
"""Does the desktop still respond WHILE a page's fetch() is transferring?

    python3 tests/qmp/qmp_fetch_ui.py <iso> <disk.img>

The claim under test is the one js_webapi.c is built around: a fetch is a
non-blocking socket stepped from the browser's event loop, so the machine keeps
running during a transfer. That is a claim, so it is measured -- with the same
instrument tests/qmp/qmp_sock_ui.py used for the socket ABI, and against the
same control:

  TREATMENT  a loaded page whose script calls fetch() every second against a
             host server that dribbles 32 KiB out over about ten seconds, so
             there is always a request in flight.
  CONTROL    one `net get` over the old blocking SYS_HTTP_GET, which runs the
             whole fetch inside a syscall holding the big kernel lock.

During each window the harness injects real mouse clicks through QEMU's input
layer and times how long each takes to reach a ring-3 app
(/usr/as/examples/events.as, which prints every event it is handed to the
serial console). That latency IS the responsiveness of the desktop: PS/2 IRQ ->
the WM's deferred input queue -> wm_drain_input -> window routing -> the app.

The control is not decoration. A measurement that cannot detect the frozen case
proves nothing about the unfrozen one, so the old path is run through the
identical instrument and the two numbers are printed next to each other.

Ordering note: events.as is launched AFTER the browser, so its window is the
top one in the cascade and the clicks (which land at the screen centre) reach
it rather than the browser. The browser keeps fetching underneath -- it is a
separate ring-3 process with its own loop, which is exactly the property being
measured.
"""

import http.server
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, dock_icon, BROWSER_SLOT          # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

MAX_LATENCY = 2.0                 # same bar as qmp_sock_ui.py; see its comment
CLICKS = 4
BODY = 32768
CHUNK, DELAY = 2048, 0.6          # -> ~9.6 s per response

payload = bytes((i * 31 + 7) & 0xff for i in range(BODY))

served_lock = threading.Lock()
served_started = 0
served_finished = 0

PAGE = """<!doctype html>
<html><head><title>fetchloop</title></head><body>
<div id="out" style="font-size:24px">FETCHLOOP</div>
<script>
/* One slow fetch at a time, restarted the moment the previous one lands, so
   the machine is continuously mid-transfer for as long as the harness needs.
   The counter goes to the DOM as well as the console: if the browser's loop
   had stopped, neither would advance. */
var n = 0;
function go() {
  fetch('/slow.bin?' + (n++))
    .then(function (r) { return r.arrayBuffer(); })
    .then(function (b) {
      console.log('FETCHLOOP-DONE ' + n + ' ' + b.byteLength);
      document.getElementById('out').textContent = 'GOT' + n;
      setTimeout(go, 200);
    })
    .catch(function (e) { console.log('FETCHLOOP-FAIL ' + e); setTimeout(go, 1000); });
}
console.log('FETCHLOOP-READY typeof-fetch=' + (typeof fetch));
go();
</script>
</body></html>
"""


class Slow(http.server.BaseHTTPRequestHandler):
    """Dribbles the body out so a transfer is measurably in flight."""
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        global served_started, served_finished
        if self.path.startswith("/page"):
            raw = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            try:
                self.wfile.write(raw)
            except OSError:
                pass
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(BODY))
        self.end_headers()
        with served_lock:
            served_started += 1
        for off in range(0, BODY, CHUNK):
            try:
                self.wfile.write(payload[off:off + CHUNK])
                self.wfile.flush()
            except OSError:
                return
            time.sleep(DELAY)
        with served_lock:
            served_finished += 1

    def log_message(self, *_a):
        pass


def served():
    with served_lock:
        return served_started, served_finished


httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Slow)
PORT = httpd.server_port
threading.Thread(target=httpd.serve_forever, daemon=True).start()

tmp = tempfile.mkdtemp(prefix="qmp_fetch_ui_")
qmp_path = os.path.join(tmp, "qmp.sock")
ser_path = os.path.join(tmp, "serial.sock")

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", f"file={DISK},format=raw,if=none,id=hd0,file.locking=off",
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", f"unix:{ser_path},server=on,wait=off",
     "-qmp", f"unix:{qmp_path},server,nowait"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

log = ""


def die(msg):
    print("FAIL: " + msg)
    print("----- serial output (tail) -----")
    print(log[-8000:])
    print("--------------------------------")
    proc.kill()
    sys.exit(1)


def connect(path):
    for _ in range(300):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(path)
            return s
        except OSError:
            if proc.poll() is not None:
                die("qemu exited before its sockets came up")
            time.sleep(0.1)
    die(f"could not connect to {path}")


ser = connect(ser_path)
ser.setblocking(False)
ui = Session(qmp_path)                 # the ONE QMP connection: input + nothing else


def pump(seconds):
    global log
    end = time.time() + seconds
    while time.time() < end:
        try:
            b = ser.recv(65536)
            if b:
                log += b.decode("utf-8", "replace")
                continue
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.02)


def wait_for(marker, timeout):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.2)
        if marker in log:
            return True
        if proc.poll() is not None:
            return marker in log
    return False


EV_RE = re.compile(r"^EV \d+", re.M)


def click_latency(timeout=MAX_LATENCY):
    """Inject one click at the pointer's current spot and time its arrival.

    None is a RESULT, not an error: a frozen desktop usually DROPS the event
    (the PS/2 controller has a one-byte buffer and nothing is draining the WM's
    queue) rather than delivering it late."""
    global log
    pump(0.05)
    mark = len(log)
    t0 = time.time()
    ui._input([{"type": "btn", "data": {"button": "left", "down": True}}])
    ui._input([{"type": "btn", "data": {"button": "left", "down": False}}])
    end = t0 + timeout
    while time.time() < end:
        pump(0.05)
        if EV_RE.search(log[mark:]):
            return time.time() - t0
        if proc.poll() is not None:
            return None
    return None


def clicks_during(label, t_inflight):
    lats = []
    for _ in range(CLICKS):
        lats.append(click_latency())
        pump(0.8)
    got = [l for l in lats if l is not None]
    lost = len(lats) - len(got)
    worst = max(got) if got else None
    print(f"  {label}: {len(got)}/{CLICKS} clicks delivered within {MAX_LATENCY:.0f}s, "
          f"worst {('%.2fs' % worst) if worst is not None else 'n/a'}"
          + (f", {lost} DROPPED" if lost else "")
          + f"  (window {time.time() - t_inflight:.1f}s)")
    return worst, lost


# ------------------------------------------------------------------ boot
if not wait_for("LOGIT_BOOT_OK", 240):
    die("kernel did not boot")
if not wait_for("desktop live", 90):
    die("the window manager never brought the desktop up")
pump(4)

# ------------------------------------------------------- the browser + page
ui.click_at(*dock_icon(BROWSER_SLOT))
for _ in range(4):
    if wait_for("launched Browser", 15):
        break
    ui.click_at(*dock_icon(BROWSER_SLOT))
else:
    die("the Dock never launched the Browser")
pump(6)

ui.click_at(420, 145)                      # address bar
for _ in range(70):
    ui.key("backspace", settle=0.02)
ui.typ("http://10.0.2.2:%d/page.html" % PORT)
ui.key("ret")

if not wait_for("FETCHLOOP-READY", 120):
    die("the fixture page never loaded")
if "typeof-fetch=function" not in log:
    die("the browser under test has no fetch() -- nothing to measure")

# ------------------------------------------------- the app the clicks reach
# Launched last, so its window sits on top of the browser at the screen centre
# where the pointer starts.
ser.sendall(b"as /usr/as/examples/events.as &\n")
if not wait_for("EVENTS-READY", 120):
    die("events.as did not open its window")
pump(2)
# Park the pointer over that window before any click is timed. It was last left
# on the browser's address bar by the typing above, and a click there measures
# the browser's loop rather than the desktop's -- and prints nothing.
ui.goto(640, 400, settle=0.4)

# ------------------------------------------------------------------ idle base
# The page is still fetching here, so this is not a true idle baseline -- it is
# read only to prove the instrument works at all before the numbers below.
base_l = [click_latency() for _ in range(2)]
base_l = [l for l in base_l if l is not None]
base = max(base_l) if base_l else None
print(f"  instrument check: worst latency {('%.2fs' % base) if base else 'n/a'}")
if base is None:
    die("clicks never reached the app at all -- the instrument is broken, and "
        "nothing below would mean anything")

# ------------------------------------------------- treatment: the page's fetch
print("measuring: clicks while the page's fetch() is mid-transfer")
started0, finished0 = served()
end = time.time() + 90
while served()[0] - started0 < 1:
    pump(0.2)
    if time.time() > end:
        die("the host server never saw a slow.bin request start streaming -- "
            "the page's fetch loop is not running")
    if proc.poll() is not None:
        die("qemu exited")
t_inflight = time.time()
if served()[1] != finished0:
    die("the transfer finished before a single click went in -- the machine was "
        "never busy, so nothing below would mean anything")
new_worst, new_lost = clicks_during("page fetch()", t_inflight)
loops_done = log.count("FETCHLOOP-DONE")
pump(2)

# ------------------------------------------- control: the old blocking path
print("measuring: the same clicks during a blocking SYS_HTTP_GET (control)")
started1, finished1 = served()
ser.sendall(f"net get http://10.0.2.2:{PORT}/slow.bin\n".encode())
end = time.time() + 90
while served()[0] - started1 < 1:
    pump(0.2)
    if time.time() > end:
        die("control: the guest never started the blocking fetch")
t_inflight = time.time()
old_worst, old_lost = clicks_during("blocking http_get", t_inflight)
pump(3)

# ----------------------------------------------------------------- verdict
print()
print(f"  page fetch()   worst {('%.2fs' % new_worst) if new_worst else 'n/a'}"
      f"  dropped {new_lost}/{CLICKS}")
print(f"  blocking path  worst {('%.2fs' % old_worst) if old_worst else 'n/a'}"
      f"  dropped {old_lost}/{CLICKS}")
print(f"  fetch() completions during the run: {loops_done}")
print()

if new_lost:
    die(f"{new_lost} of {CLICKS} clicks never reached the app while the page was "
        f"fetching -- the desktop was not responsive")
if new_worst > MAX_LATENCY:
    die(f"worst click latency during a page fetch was {new_worst:.2f}s, over the "
        f"{MAX_LATENCY:.1f}s bar")
if loops_done < 1:
    die("no fetch() ever completed during the measurement, so the transfers "
        "being 'in flight' cannot be attributed to the page")

# A sanity check on the INSTRUMENT, not a claim about how bad the old path is:
# if the same clicks fare identically during a fetch that holds the big kernel
# lock, this harness is not measuring responsiveness and its PASS is worthless.
if old_lost == 0 and old_worst is not None and old_worst <= new_worst * 2:
    die(f"the blocking control came out as responsive as fetch() "
        f"({old_lost} dropped, worst {old_worst:.2f}s vs {new_worst:.2f}s) -- "
        f"this harness is not detecting the freeze it exists to detect")

print(f"PASS: the desktop stayed responsive while the page fetched -- "
      f"{CLICKS}/{CLICKS} injected clicks reached a ring-3 app, worst "
      f"{new_worst:.2f}s, {loops_done} fetch() completions. The same clicks during "
      f"one blocking SYS_HTTP_GET: {CLICKS - old_lost}/{CLICKS} arrived"
      + (f", {old_lost} dropped by the frozen desktop" if old_lost else ""))
proc.kill()
sys.exit(0)
