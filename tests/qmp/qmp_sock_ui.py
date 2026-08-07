#!/usr/bin/env python3
"""Does the desktop still respond WHILE the network is busy?

That question is the entire reason the non-blocking socket layer exists, and
"the UI no longer freezes" is a claim, not a result. So this measures it, twice,
with the same instrument:

  TREATMENT  four concurrent /bin/socktest transfers over the new sockets.
  CONTROL    one `net get` over the old blocking SYS_HTTP_GET, which runs the
             whole fetch inside the syscall holding the big kernel lock.

Both fetch the same deliberately SLOW body -- a host server that dribbles 32 KiB
out over about ten seconds -- so there is a long, unambiguous window during which
the machine is mid-transfer. During that window the harness injects real mouse
clicks through QEMU's input layer and times how long each one takes to reach a
ring-3 app (/usr/as/examples/events.as, which prints every event it is handed to
the serial console). That latency IS the responsiveness of the desktop: PS/2
IRQ -> the WM's deferred input queue -> wm_drain_input in the WM loop -> window
routing -> the app. If the WM loop is not running, the number goes to seconds.

The control is not decoration. A measurement that cannot detect the frozen case
does not prove anything about the unfrozen one, so the old path is run through
the identical instrument and its latency printed next to the new one.

    python3 tests/qmp/qmp_sock_ui.py <iso> <disk.img>

Two unix sockets, like qmp_input.py: QMP to inject input, and a bidirectional
serial console to drive /bin/sh and read the guest back.
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

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# The bar for "responsive". A click has to cross a 100 Hz WM loop that also
# composites, under QEMU's TCG on a loaded host, so a few hundred milliseconds is
# normal and anything at that scale means the loop is alive. Seconds means it is
# not. 2.0 s is deliberately loose: this test must fail on a freeze, not on a
# slow morning.
MAX_LATENCY = 2.0
CLICKS = 4                        # injections per phase
BODY = 32768
CHUNK, DELAY = 2048, 0.6          # -> ~9.6 s per response

# ---------------------------------------------------------------- host server
payload = bytes((i * 31 + 7) & 0xff for i in range(BODY))

# The server is the clock for "is a transfer actually in flight". Waiting on a
# serial marker instead was the first attempt and it was wrong: the guest prints
# nothing when a fetch STARTS, so the harness ended up clicking before the fetch
# had begun and measured an idle machine. These counters are ground truth --
# they are incremented by the thread that is writing the bytes.
served_lock = threading.Lock()
served_started = 0                 # requests whose first chunk has gone out
served_finished = 0


class Slow(http.server.BaseHTTPRequestHandler):
    """Dribbles the body out so a transfer is measurably in flight."""
    protocol_version = "HTTP/1.0"     # close-delimited: the guest reads to EOF

    def do_GET(self):
        global served_started, served_finished
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


# Threading: four guest connections arrive at once, and a serialising host would
# hide the property under test.
httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Slow)
PORT = httpd.server_port
threading.Thread(target=httpd.serve_forever, daemon=True).start()

tmp = tempfile.mkdtemp(prefix="qmp_sock_ui_")
qmp_path = os.path.join(tmp, "qmp.sock")
ser_path = os.path.join(tmp, "serial.sock")

# 1280x800 so the desktop geometry matches what qmp_input.py relies on: Finder
# takes cascade slot 0, Clock slot 1, and events.as slot 2 -- a 900x600 window
# that covers the screen centre, which is where the cursor starts. The pointer
# therefore needs no walking to a target this harness cannot see.
#
# file.locking=off next to -snapshot: QEMU otherwise takes a write lock on the
# disk image and locks out every other harness on this tree.
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
    print(log[-6000:])
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
qmp = connect(qmp_path)
qf = qmp.makefile("rw")


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


def qcmd(d):
    qf.write(json.dumps(d) + "\n")
    qf.flush()
    while True:
        line = qf.readline()
        if not line:
            return None
        m = json.loads(line)
        if "return" in m or "error" in m:
            return m


def send(*events):
    qcmd({"execute": "input-send-event", "arguments": {"events": list(events)}})


def btn(name, down):
    return {"type": "btn", "data": {"button": name, "down": down}}


EV_RE = re.compile(r"^EV \d+", re.M)


def click_latency(timeout=MAX_LATENCY):
    """Inject one click and time how long it takes to reach the app.

    The clock starts at the injection and stops at the first NEW 'EV' line the
    app prints. Returns the latency in seconds, or None if nothing arrived
    within `timeout` -- and None is a RESULT, not an error. A frozen desktop
    mostly drops the event rather than delivering it late: the PS/2 controller
    has a one-byte buffer and nothing is draining the WM's input queue, so the
    packet is overwritten before anyone looks. `timeout` is MAX_LATENCY, so
    "delivered" and "delivered acceptably fast" are the same question."""
    global log
    pump(0.05)
    mark = len(log)
    t0 = time.time()
    send(btn("left", True))
    send(btn("left", False))
    end = t0 + timeout
    while time.time() < end:
        pump(0.05)
        if EV_RE.search(log[mark:]):
            return time.time() - t0
        if proc.poll() is not None:
            return None
    return None


def measure(label, start_cmd, want_conns, timeout):
    """Run a transfer and click at the desktop while it is genuinely in flight.

    The window is bounded at both ends by the HOST server's own counters: the
    clicks do not begin until `want_conns` responses have started streaming, and
    the phase reports how many finished, so a transfer that failed or completed
    early cannot masquerade as a measurement."""
    global log
    started0, finished0 = served()
    ser.sendall(start_cmd.encode())
    end = time.time() + timeout
    while served()[0] - started0 < want_conns:
        pump(0.2)
        if time.time() > end:
            die(f"{label}: the host server never saw {want_conns} request(s) "
                f"start streaming -- the guest did not begin the transfer")
        if proc.poll() is not None:
            die(f"{label}: qemu exited")
    t_inflight = time.time()
    if served()[1] != finished0:
        die(f"{label}: the transfer finished before a single click went in -- "
            f"the machine was never busy, so nothing below would mean anything")

    lats = []
    for _ in range(CLICKS):
        lats.append(click_latency())
        pump(0.8)
    window = time.time() - t_inflight

    got = [l for l in lats if l is not None]
    lost = len(lats) - len(got)
    worst = max(got) if got else None
    print(f"  {label}: {len(got)}/{CLICKS} clicks delivered within "
          f"{MAX_LATENCY:.0f}s, worst "
          f"{('%.2fs' % worst) if worst is not None else 'n/a'}"
          + (f", {lost} DROPPED" if lost else "")
          + f"  (sequence took {window:.1f}s)")
    return worst, lost


# ------------------------------------------------------------------ boot + app
if not wait_for("LOGIT_BOOT_OK", 240):
    die("kernel did not boot")
pump(6)                                       # Finder + Clock settle
qcmd({"execute": "qmp_capabilities"})

ser.sendall(b"as /usr/as/examples/events.as &\n")
if not wait_for("EVENTS-READY", 120):
    die("events.as did not open its window")
pump(1)

# A baseline with the network idle, so the two numbers below are read against
# what this machine does when nothing is happening at all.
idle = [click_latency() for _ in range(2)]
idle = [l for l in idle if l is not None]
base = max(idle) if idle else None
print(f"  idle baseline: worst latency "
      f"{('%.2fs' % base) if base is not None else 'n/a'}")
if base is None:
    die("clicks did not reach the app even with the network idle -- the "
        "instrument is broken, not the kernel")
pump(1)

# ------------------------------------------------- treatment: async sockets
print("measuring: 4 concurrent transfers over the non-blocking sockets")
new_worst, new_lost = measure(
    "async sockets",
    f"socktest 10.0.2.2 {PORT} /slow.bin 4 &\n", 4, 90)

if not wait_for("SOCKTEST_OK", 90):
    print("  note: socktest did not report SOCKTEST_OK within 90 s")
pump(2)

# ------------------------------------------- control: the old blocking path
print("measuring: the same clicks during a blocking SYS_HTTP_GET (control)")
old_worst, old_lost = measure(
    "blocking http_get",
    f"net get http://10.0.2.2:{PORT}/slow.bin\n", 1, 90)
pump(3)

# ----------------------------------------------------------------- verdict
print()
print(f"  idle           worst {('%.2fs' % base)}")
print(f"  async sockets  worst {('%.2fs' % new_worst) if new_worst else 'n/a'}"
      f"  dropped {new_lost}/{CLICKS}")
print(f"  blocking path  worst {('%.2fs' % old_worst) if old_worst else 'n/a'}"
      f"  dropped {old_lost}/{CLICKS}")
print()

if new_lost:
    die(f"{new_lost} of {CLICKS} clicks never reached the app during a "
        f"transfer over the async sockets -- the desktop was not responsive")
if new_worst > MAX_LATENCY:
    die(f"worst click latency during an async transfer was {new_worst:.2f}s, "
        f"over the {MAX_LATENCY:.1f}s bar")

# The control is a SANITY CHECK ON THE INSTRUMENT, not a claim about how bad the
# old path is. How long SYS_HTTP_GET freezes for depends on how fast the host
# dribbles and how TCG happens to schedule, so asserting a specific latency there
# would be flaky. What is asserted is only that the two are distinguishable: if
# the same clicks fare identically on a path that holds the big kernel lock for
# the whole fetch, then this harness is not measuring responsiveness at all and
# its PASS above is worthless.
if old_lost == 0 and old_worst is not None and old_worst <= new_worst * 2:
    die(f"the blocking control came out as responsive as the async path "
        f"({old_lost} dropped, worst {old_worst:.2f}s vs {new_worst:.2f}s) -- "
        f"this harness is not detecting the freeze it exists to detect, so its "
        f"PASS would mean nothing")

print(f"PASS: the desktop stayed responsive during four concurrent transfers "
      f"-- {CLICKS}/{CLICKS} injected clicks reached a ring-3 app, worst "
      f"{new_worst:.2f}s. The same clicks during one blocking SYS_HTTP_GET: "
      f"{CLICKS - old_lost}/{CLICKS} arrived"
      + (f", {old_lost} dropped by the frozen desktop" if old_lost else ""))
proc.kill()
sys.exit(0)
