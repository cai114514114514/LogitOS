#!/usr/bin/env python3
"""The one test that says the SYSTEM works, not that the kernel booted.

    python3 tests/qmp/qmp_fullsystem.py <iso> <disk.img> [root .aex ...]

`make test` asserts LOGIT_BOOT_OK. It asserted it happily on a build where no
application could load, which is how that regression reached HEAD: every check
in the tree was green and none of them ran a program. The bar here is the whole
machine, and every step below is a SEPARATE assertion with its own sentence, so
a failure names itself instead of leaving you to bisect a boolean:

  1  the kernel boots
  2  LogitFS mounts off the disk image
  3  the compositor brings the desktop up
  4  the backing scale the kernel picked is the one this harness modelled
     (the check that stops every coordinate below from rotting silently)
  5  the guest's own root listing equals the .aex the build packed there
  6  the serial console shell fork+execve's a coreutil
  7  a shell PIPELINE moves real bytes between two processes
  8  EVERY dock icon launches its app -- named, one assertion per app
  9  NEGATIVE CONTROL: a click on the dock that is not on an icon launches nothing
 10  no ring-3 app faulted while all of them were live
 11  ICMP reaches the gateway and comes back
 12  DNS resolves a name (skipped, loudly, if the HOST itself has no resolver)
 13  HTTP fetches the exact bytes a host fixture served -- length AND checksum
 14  the browser renders that fixture's colours to real PIXELS
 15  ... and rasterises TEXT inside it
 16  ... and fetches + decodes a PNG sub-resource to its exact colour
 17  ... and repaints when navigated, so 14-16 cannot be a frozen frame
 18  a key pressed AFTER all of that reaches a ring-3 app, and its Ctrl+S
     writes a file the shell can then read back on a different channel

Every network assertion is served by a fixture HTTP server on the host, over
SLIRP, so the test is hermetic: no outbound internet, and the expected byte
count and FNV-1a checksum are computed from the same bytes that were served.

The assertions do not stop at the first failure. A broken build should say
everything it is going to say in one run -- a harness that aborts on step 2
teaches you one fact per QEMU boot, and a QEMU boot here is a minute.

Fault-injection knobs (used by the harness's OWN verification -- a green test
that has never been shown to go red is exactly the failure mode this file
exists to eliminate):

    FS_NIC=off      boot with no network card at all
    FS_MODE=WxH     boot at another display mode (default 1280x800)
    FS_OUT=dir      keep screendumps and the serial log here
    QEMU / QEMU_CPU as elsewhere in the tree
"""

import http.server
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui                                             # noqa: E402
from qmp_ui import PPM, Session, dock_icon                # noqa: E402

# ---------------------------------------------------------------- arguments --

if len(sys.argv) < 3:
    sys.stderr.write(__doc__)
    sys.exit(2)
ISO, DISK = sys.argv[1], sys.argv[2]
ROOT_AEX = sys.argv[3:]          # host paths of the .aex the Makefile packs at /

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
QEMU_CPU = os.environ.get("QEMU_CPU", "max")
MODE_W, MODE_H = (int(v) for v in os.environ.get("FS_MODE", "1280x800").split("x"))
NIC_ON = os.environ.get("FS_NIC", "on") != "off"
OUT = os.environ.get("FS_OUT") or tempfile.mkdtemp(prefix="fullsystem_")
os.makedirs(OUT, exist_ok=True)

SCALE = qmp_ui.configure(MODE_W, MODE_H)

# ------------------------------------------------------------- the fixture ---
# Deliberately odd colours: an exact match on one of these cannot be confused
# with the wallpaper gradient, the glass chrome, an icon or a font's anti-
# aliasing. The PNG colour appears in NO stylesheet, so finding it on screen is
# proof the sub-resource was fetched AND decoded AND painted, not merely that a
# box was laid out where an image would go.
BLOCK_A = (253, 5, 9)
BLOCK_B = (9, 253, 5)
PNG_RGB = (5, 9, 253)
LINK_RGB = (5, 253, 253)
PAGE2_RGB = (253, 5, 253)

PROBE_BODY = (b"logitos-fullsystem-probe\n" + bytes(range(32, 127)) + b"\n") * 7


def png_solid(w, h, rgb):
    """A minimal, valid, non-interlaced 8-bit RGB PNG of one flat colour.

    Hand-rolled rather than pulled from an image library so the harness keeps
    its no-dependency property -- the same reason PPM is parsed by hand."""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(tag, data):
        body = tag + data
        return (len(data).to_bytes(4, "big") + body +
                (zlib.crc32(body) & 0xFFFFFFFF).to_bytes(4, "big"))

    ihdr = w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes((8, 2, 0, 0, 0))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


PNG_BYTES = png_solid(96, 64, PNG_RGB)

# Single-word text on the white block: layout emits one text box per word, and
# the assertion here is a pixel COUNT, so what matters is that the glyphs are
# large and black rather than what they say.
PAGE1 = """<!doctype html>
<html><head><title>fullsystem</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 36px; color: #000000; }
#a    { background: #fd0509; height: 90px; }
#b    { background: #09fd05; height: 90px; }
#text { background: #ffffff; color: #000000; font-size: 44px; }
#go   { display: block; background: #05fdfd; color: #000000; font-size: 40px;
        text-decoration: none; }
</style></head><body>
<div id="a">ALPHABLOCK</div>
<div id="text">RENDEREDTEXTHERE</div>
<div id="b">BETABLOCK</div>
<img src="/solid.png">
<a id="go" href="/page2.html">GONEXTPAGE</a>
</body></html>
"""

PAGE2 = """<!doctype html>
<html><head><title>second</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; }
div { display: block; font-size: 40px; background: #fd05fd; height: 200px; }
</style></head><body><div>SECONDPAGE</div></body></html>
"""

requested = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        requested.append(self.path)
        if self.path.startswith("/probe.txt"):
            body, ctype = PROBE_BODY, "text/plain"
        elif self.path.startswith("/solid.png"):
            body, ctype = PNG_BYTES, "image/png"
        elif self.path.startswith("/page2"):
            body, ctype = PAGE2.encode(), "text/html"
        elif self.path.startswith("/page"):
            body, ctype = PAGE1.encode(), "text/html"
        else:
            body, ctype = b"not found\n", "text/plain"
        self.send_response(200 if body != b"not found\n" else 404)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_a):
        pass


def fnv1a(data):
    h = 2166136261
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


# ------------------------------------------------------------- the report ----

ROWS = []


def record(state, name, detail=""):
    ROWS.append((state, name, detail))
    line = {"ok": "ok  ", "FAIL": "FAIL", "skip": "skip"}[state] + ": " + name
    if detail:
        line += "  -- " + detail
    print(line, flush=True)


def ck(cond, name, detail=""):
    record("ok" if cond else "FAIL", name, detail)
    return bool(cond)


def skip(name, why):
    record("skip", name, why)


# ------------------------------------------------------------ serial line ----

class Serial:
    """The guest's COM1, both directions.

    `-serial file:` is write-only, and every existing boot test therefore reads
    the machine without being able to talk to it. init runs /bin/sh on this
    port, so a socket chardev turns the serial log into an interactive channel:
    the same wire proves what the kernel printed AND what a program run from the
    shell answered."""

    def __init__(self, path):
        self.srv = socket.socket(socket.AF_UNIX)
        self.srv.bind(path)
        self.srv.listen(1)
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.conn = None
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        conn, _ = self.srv.accept()
        self.conn = conn
        while True:
            try:
                data = conn.recv(8192)
            except OSError:
                return
            if not data:
                return
            with self.lock:
                self.buf.extend(data)

    def text(self):
        with self.lock:
            return bytes(self.buf).decode("utf-8", "replace")

    def wait(self, needle, secs, alive=None):
        end = time.time() + secs
        while time.time() < end:
            if needle in self.text():
                return True
            if alive is not None and alive() is not None:
                return False
            time.sleep(0.2)
        return False

    def send(self, data):
        end = time.time() + 30
        while self.conn is None and time.time() < end:
            time.sleep(0.1)
        if self.conn is None:
            raise RuntimeError("the guest never opened the serial port")
        self.conn.sendall(data)

    _seq = [0]

    def sh(self, cmd, secs=30):
        """Run `cmd` in the serial shell; return just that command's output.

        The shell echoes what it is sent, so a sentinel typed as an ARGUMENT
        appears twice: once in the echo of the command line, once as the output
        of running it. Waiting for the second occurrence is what makes this
        synchronous without parsing the prompt."""
        self._seq[0] += 1
        mark = "FSDONE%03dZ" % self._seq[0]
        with self.lock:
            start = len(self.buf)
        self.send((cmd + "\necho " + mark + "\n").encode())
        end = time.time() + secs
        while time.time() < end:
            with self.lock:
                chunk = bytes(self.buf[start:]).decode("utf-8", "replace")
            if chunk.count(mark) >= 2:
                break
            time.sleep(0.2)
        with self.lock:
            chunk = bytes(self.buf[start:]).decode("utf-8", "replace")
        return chunk.split(mark)[0]


# --------------------------------------------------- where the pointer IS -----
# On virtio-gpu the arrow lives on the device's cursor plane, so a screendump
# does not contain it and dead reckoning off `rel` deltas is the only model the
# harness has. The guest prints `[wm] ptr X Y` when the pointer settles, which
# turns that model into something checkable: aim, read back, correct. A click
# that lands somewhere else is then a message instead of "the app did not open".

def aim(ui, serial, x, y, tries=4):
    for _ in range(tries):
        ui.goto(x, y)
        time.sleep(0.4)
        got = qmp_ui.parse_pointer(serial.text())
        if got is None or got == (x, y):
            return True                      # agreed, or the guest is not saying
        ui.cur = [got[0], got[1]]            # believe the guest, re-aim
    return False


def click(ui, serial, x, y):
    ok = aim(ui, serial, x, y)
    ui.click()
    return ok


# ---------------------------------------------- where the WINDOWS land --------
# wm.c places a new window at S(110 + cascade*28), S(70 + cascade*28) with
# cascade counting SYS_GUI_CREATE calls mod 6, then clamps it on-screen. Which
# value the browser gets depends on how many windows opened before it, and this
# test opens every app -- so rather than hardcode one coordinate (the mistake
# that made qmp_freeze.py inert), enumerate every position the rule can produce
# and let the fixture server say which one was right.

def window_slots(cw_pt, ch_pt):
    """Every (x, y) wm.c can place a cw_pt x ch_pt window at, de-duplicated."""
    mbh, tbh = qmp_ui.pt(24), qmp_ui.pt(30)
    w, h = qmp_ui.pt(cw_pt), tbh + qmp_ui.pt(ch_pt)
    out = []
    for c in range(6):
        x, y = qmp_ui.pt(110 + c * 28), qmp_ui.pt(70 + c * 28)
        if x + w > qmp_ui.SCREEN_W:
            x = qmp_ui.SCREEN_W - w
        x = max(0, x)
        if y + h > qmp_ui.SCREEN_H - qmp_ui.pt(4):
            y = qmp_ui.SCREEN_H - qmp_ui.pt(4) - h
        y = max(mbh, y)
        if (x, y) not in out:
            out.append((x, y))
    return out


# -------------------------------------------------------------- host names ---

def aex_name(path):
    """The display name inside an .aex header (aex.h: char name[32] at +8)."""
    with open(path, "rb") as fh:
        head = fh.read(64)
    if head[:4] != b"AEX1":
        return None
    return head[8:40].split(b"\x00")[0].decode("ascii", "replace")


HOST_APPS = {}                  # "clock.aex" -> "Clock"
for p in ROOT_AEX:
    nm = aex_name(p)
    if nm:
        HOST_APPS[os.path.basename(p)] = nm

# ------------------------------------------------------------------- boot ----

tmp = tempfile.mkdtemp(prefix="fullsystem_run_")
ser_path = os.path.join(tmp, "ser.sock")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_log = os.path.join(OUT, "fullsystem-serial.log")

srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

serial = Serial(ser_path)

net_args = (["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"] if NIC_ON
            else [])
cmdline = [QEMU, "-cpu", QEMU_CPU, "-cdrom", ISO,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
           "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none",
           "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (MODE_W, MODE_H),
           "-display", "none", "-no-reboot",
           "-chardev", "socket,id=ser0,path=%s" % ser_path,
           "-serial", "chardev:ser0",
           "-qmp", "unix:%s,server,nowait" % qmp_path] + net_args

print("fullsystem: %dx%d, scale %d%%, fixture http://10.0.2.2:%d/, nic=%s"
      % (MODE_W, MODE_H, SCALE, PORT, "on" if NIC_ON else "OFF"), flush=True)
print("fullsystem: artefacts in " + OUT, flush=True)

proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)


def finish(code):
    with open(serial_log, "w", encoding="utf-8") as fh:
        fh.write(serial.text())
    try:
        proc.kill()
    except OSError:
        pass
    print("\n---------------- full-system assertions ----------------")
    for state, name, detail in ROWS:
        print("  %-4s %s%s" % (state, name, ("  -- " + detail) if detail else ""))
    bad = [r for r in ROWS if r[0] == "FAIL"]
    print("--------------------------------------------------------")
    print("serial log: " + serial_log)
    if bad:
        print("\nFAIL: %d of %d assertions failed" % (len(bad), len(ROWS)))
        print("----- serial (tail) -----")
        print(serial.text()[-6000:])
    else:
        print("\nPASS: the whole system works end to end (%d assertions)" % len(ROWS))
    sys.exit(1 if bad else code)


def shot(name):
    return os.path.join(OUT, name + ".ppm")


# 1 --------------------------------------------------------------------------
booted = ck(serial.wait("LOGIT_BOOT_OK", 300, alive=proc.poll),
            "boot: the kernel reached 64-bit C and printed LOGIT_BOOT_OK")
if not booted and proc.poll() is not None:
    record("FAIL", "boot: QEMU exited before the kernel booted",
           "exit %r" % proc.poll())
    finish(1)

# 2 --------------------------------------------------------------------------
log = serial.text()
ck("[fs] mounted" in log and "[fs] mount FAILED" not in log,
   "fs: LogitFS mounted off the disk image")

# 3 --------------------------------------------------------------------------
desktop = ck(serial.wait("desktop live", 120, alive=proc.poll),
             "wm: the compositor brought the desktop up")

# 4 --------------------------------------------------------------------------
# The check that keeps every coordinate below honest. The dock is centred and
# measured in POINTS scaled by a factor the kernel derives from the mode; a
# driver that hardcodes device pixels goes quietly inert the day either changes.
# Here the kernel's own number is compared with the harness's model of it, so a
# divergence is an error message instead of a click on the wallpaper.
m = re.search(r"\[wm\] display (\d+)x(\d+) px, scale (\d+)%", serial.text())
if m:
    gw, gh, gs = int(m.group(1)), int(m.group(2)), int(m.group(3))
    ck((gw, gh, gs) == (MODE_W, MODE_H, SCALE),
       "wm: the backing scale the kernel picked is the one this harness models",
       "guest %dx%d @ %d%%, harness %dx%d @ %d%%" % (gw, gh, gs, MODE_W, MODE_H, SCALE))
else:
    ck(False, "wm: the backing scale the kernel picked is the one this harness models",
       "the guest never printed its display geometry")

time.sleep(4)                      # let the auto-launched apps finish painting

# 5 --------------------------------------------------------------------------
# The guest's own root listing, which is also the DOCK ORDER: scan_apps() walks
# the same vfs enumeration ls does, so index i here is dock slot i.
root_ls = serial.sh("ls /", 40) if desktop else ""
guest_aex = [ln.strip() for ln in root_ls.splitlines()
             if ln.strip().endswith(".aex")]
if ROOT_AEX:
    ck(guest_aex == list(HOST_APPS.keys()),
       "fs: the guest lists exactly the .aex the build packed at the disk root",
       "guest %r vs build %r" % (guest_aex, list(HOST_APPS.keys())))
else:
    skip("fs: the guest lists exactly the .aex the build packed at the disk root",
         "no root .aex paths were passed, so there is nothing to compare against")

# 6 --------------------------------------------------------------------------
out = serial.sh("uname", 30)
ck("LogitOS x86_64" in out,
   "sh: the serial console shell fork+execve'd a coreutil (uname)",
   repr(out.strip()[-80:]))

# 7 --------------------------------------------------------------------------
# A pipeline is two processes and a pipe, not one program: `echo` writes into a
# pipe whose read end is `wc`'s stdin, and the byte count is arithmetic on data
# that actually crossed it. "The shell did not crash" is not this.
token = "fullsystempipe"
out = serial.sh("echo %s | wc" % token, 30)
nums = re.findall(r"\d+", out)
ck(str(len(token) + 1) in nums,
   "sh: a pipeline moved real bytes between two processes (echo | wc)",
   "wanted %d bytes, wc said %r" % (len(token) + 1, nums))

# 8 --------------------------------------------------------------------------
# Every dock icon launches its app. This is the assertion `make test` did not
# have on the day a build shipped where no application could load.
ui = None
launch_re = re.compile(r"\[wm\] launched (.+)")


def launched_names():
    return launch_re.findall(serial.text())


if desktop:
    try:
        ui = Session(qmp_path)
    except OSError as exc:
        ck(False, "dock: connected to QEMU's QMP monitor", repr(exc))

if ui is not None and guest_aex:
    n = len(guest_aex)

    # 9 ----------------------------------------------------------------------
    # The control for 8, and it runs FIRST -- while only the two auto-launched
    # windows exist, so the point is over the wallpaper and the click cannot
    # land inside an app. (Run last, after every app is open, it lands in the
    # Browser's viewport instead and quietly steals the address bar's focus,
    # which then reads as "the browser cannot fetch". A control that damages
    # the run it is controlling is not a control.)
    #
    # Without it, "a launch line appeared" would also be the reading if the WM
    # launched things for reasons unrelated to the click -- a driver whose
    # coordinates all miss but whose apps auto-start would still look green.
    # The point is on the dock PANEL, in its left padding, between the panel
    # edge and the first icon: on the dock, on no icon.
    isz, gap = qmp_ui.pt(qmp_ui.DOCK_ISZ_PT), qmp_ui.pt(qmp_ui.DOCK_GAP_PT)
    dw = gap + n * (isz + gap)
    x0 = (qmp_ui.SCREEN_W - dw) // 2
    before = len(launched_names())
    focus_before = serial.text().count("already live, focusing")
    click(ui, serial, x0 + gap // 2, dock_icon(0, n)[1])
    time.sleep(4)
    ck(len(launched_names()) == before and
       serial.text().count("already live, focusing") == focus_before,
       "dock: NEGATIVE CONTROL -- a click on the dock but not on an icon "
       "launches nothing")

    for i, fname in enumerate(guest_aex):
        want = HOST_APPS.get(fname)
        before = len(launched_names())
        focus_before = serial.text().count("already live, focusing")
        x, y = dock_icon(i, n)
        click(ui, serial, x, y)
        # The browser is a ~3 MB image off virtio-blk with a 96 MiB arena; the
        # small apps land in a second. One budget, sized for the slowest.
        deadline = time.time() + 90
        got = None
        while time.time() < deadline:
            names = launched_names()
            if len(names) > before:
                got = names[before]
                break
            if serial.text().count("already live, focusing") > focus_before:
                got = "(already running)"
                break
            time.sleep(0.3)
        if want is None:
            ck(got is not None,
               "dock: icon %d (%s) launched" % (i, fname),
               "guest name unknown to the build; saw %r" % (got,))
        else:
            ck(got is not None and got in (want, "(already running)"),
               "dock: icon %d launched %s (%s)" % (i, want, fname),
               "the click at (%d,%d) produced %r" % (x, y, got))
        time.sleep(1.0)
else:
    why = ("the desktop never came up, so nothing could be clicked"
           if ui is None else
           "the guest listed no .aex at the disk root, so the Dock is empty -- "
           "see the filesystem assertions above")
    for name in ("dock: every dock icon launches its app",
                 "dock: NEGATIVE CONTROL -- a click that is not on an icon "
                 "launches nothing"):
        skip(name, why)

# 10 -------------------------------------------------------------------------
# Every app is live at once at this point, which is the state that finds the
# leaks. A ring-3 fault is contained by design (the app dies, the desk lives),
# so nothing else in this test would notice one.
faults = re.findall(r"\[fault\] app exception: [^\n]*", serial.text())
ck(not faults,
   "apps: no ring-3 app faulted while all of them were running",
   ("; ".join(faults[:3])) if faults else "")

# 11..13 ---------------------------------------------------------------------
if NIC_ON:
    out = serial.sh("net ping", 40)
    ck(re.search(r"reply: \d+ ms", out) is not None,
       "net: an ICMP echo reached the gateway and came back",
       repr(out.strip()[-90:]))

    try:
        socket.gethostbyname("example.com")
        host_dns = True
    except OSError:
        host_dns = False
    if host_dns:
        out = serial.sh("net dns example.com", 40)
        # `net dns` prints the address on a line of its own; kernel log lines
        # share the wire, so match a bare dotted quad anywhere rather than
        # assuming it is the last thing printed.
        addr = re.search(r"^\s*(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})\s*$", out, re.M)
        ck(addr is not None and "lookup failed" not in out,
           "net: DNS resolved a hostname to an address",
           ("resolved to " + addr.group(1)) if addr else repr(out.strip()[-90:]))
    else:
        skip("net: DNS resolved a hostname to an address",
             "the HOST cannot resolve either, so SLIRP has nothing to forward to")

    url = "http://10.0.2.2:%d/probe.txt" % PORT
    out = serial.sh("net get " + url, 60)
    m = re.search(r"http bytes (\d+) fnv1a (\d+)", out)
    ck(m is not None and (int(m.group(1)), int(m.group(2))) ==
       (len(PROBE_BODY), fnv1a(PROBE_BODY)),
       "net: HTTP returned the EXACT bytes the host served (length + FNV-1a)",
       "wanted %d/%d, guest said %r"
       % (len(PROBE_BODY), fnv1a(PROBE_BODY), m.groups() if m else out.strip()[-90:]))
else:
    for name in ("net: an ICMP echo reached the gateway and came back",
                 "net: DNS resolved a hostname to an address",
                 "net: HTTP returned the EXACT bytes the host served "
                 "(length + FNV-1a)"):
        ck(False, name, "booted with FS_NIC=off -- there is no network card")

# 14..17 ---------------------------------------------------------------------
BROWSER_CHECKS = (
    "browser: the fixture page's CSS colours reached the PIXELS",
    "browser: text inside the page was rasterised (dark pixels on the white block)",
    "browser: a PNG sub-resource was fetched and decoded to its exact colour",
    "browser: clicking a LINK navigated and repainted, so the above is not a "
    "frozen frame",
)

browser_live = "Browser" in launched_names()

# c/apps/browser/browser.c: the window is 1180x620 POINTS and its address bar is
# the top BARH=30 points of the CONTENT area -- a click there is what sets
# `editing`, and without it every keystroke goes to the page instead of the URL.
BROWSER_WIN = (1180, 620)
BROWSER_BARH = 30


def type_url(url):
    for _ in range(90):
        ui.key("backspace", settle=0.015)
    ui.typ(url)
    ui.key("ret")


def load_url(url, want, secs=25):
    """Put `url` in the address bar and wait for the fixture to be asked.

    browser.c starts with `editing = 1`, so the address bar already has focus
    and the first load needs no click at all -- which is worth knowing, because
    five drivers in this tree "click the address bar" at a coordinate that has
    been inside the window-manager TITLEBAR for some time and they pass anyway.

    If typing alone does not produce a request, fall back to actually clicking
    the bar -- at every position wm.c's cascade rule can put the window in,
    since this test opens every app and cannot know which one the browser got.
    Success is defined by the HOST seeing the request, not by the harness
    believing it clicked in the right place. If nothing works, that IS the
    finding."""
    dock_slot = guest_aex.index("browser.aex") if "browser.aex" in guest_aex else None

    def arrived(deadline):
        while time.time() < deadline:
            if any(r.startswith(want) for r in requested):
                return True
            time.sleep(0.4)
        return False

    type_url(url)
    if arrived(time.time() + secs):
        return True
    for wx, wy in window_slots(*BROWSER_WIN):
        if dock_slot is not None:              # re-raise: a stray probe click may
            click(ui, serial, *dock_icon(dock_slot, len(guest_aex)))   # have covered it
            time.sleep(1.5)
        click(ui, serial, wx + qmp_ui.pt(400),
              wy + qmp_ui.pt(30) + qmp_ui.pt(BROWSER_BARH // 2))
        time.sleep(0.4)
        type_url(url)
        if arrived(time.time() + secs):
            return True
    return False


if ui is not None and NIC_ON and browser_live:
    time.sleep(8)                    # ~3 MB .aex off virtio-blk, ELF load, paint
    if not load_url("http://10.0.2.2:%d/page.html" % PORT, "/page.html"):
        for name in BROWSER_CHECKS:
            ck(False, name,
               "the browser never requested the page -- neither by typing into "
               "the focused address bar nor by clicking it at any of the %d "
               "window positions wm.c's cascade can produce"
               % len(window_slots(*BROWSER_WIN)))
    else:
        time.sleep(10)             # fetch sub-resources, style, lay out, paint
        p1 = PPM(ui.screendump(shot("page1")))
        box_a = p1.find_color(BLOCK_A)
        box_b = p1.find_color(BLOCK_B)
        ck(box_a is not None and box_b is not None,
           BROWSER_CHECKS[0],
           "block A %r, block B %r" % (box_a, box_b))

        if box_a and box_b:
            # The white text block sits BETWEEN the two coloured bands, by
            # construction. Counting near-black pixels in that gap is a claim
            # about GLYPHS: an empty layout, a missing font, or a text run
            # painted in the background colour all give zero, and only a
            # rasterised string gives hundreds.
            strip = (box_a[0], box_a[3] + 1, box_a[2], box_b[1] - 1)
            dark = p1.dark_pixels(strip) if strip[3] > strip[1] else 0
            ck(dark > 100, BROWSER_CHECKS[1],
               "%d near-black pixels in the %r gap between the colour blocks"
               % (dark, strip))
        else:
            skip(BROWSER_CHECKS[1],
                 "the colour blocks were not found to measure between")

        got_png = any(r.startswith("/solid.png") for r in requested)
        png_box = p1.find_color(PNG_RGB)
        ck(got_png and png_box is not None, BROWSER_CHECKS[2],
           "the fixture was asked for the PNG: %s; its exact colour on screen: %r"
           % (got_png, png_box))

        # Navigate by CLICKING A LINK, not by retyping the URL. It is the
        # stronger statement -- hit-testing, the href, and the fetch all have to
        # be right -- and it needs no address-bar coordinate at all: the link is
        # found by its colour and the click is aimed at a glyph inside it, so
        # the harness reads the position off the picture the guest drew.
        link = p1.find_color(LINK_RGB)
        target = p1.first_dark(link) if link else None
        if target is None:
            ck(False, BROWSER_CHECKS[3],
               "the link block was not on screen to click (colour box %r)" % (link,))
        else:
            click(ui, serial, target[0], target[1])
            end = time.time() + 60
            while time.time() < end and not any(r.startswith("/page2")
                                                for r in requested):
                time.sleep(0.4)
            time.sleep(8)
            p2 = PPM(ui.screendump(shot("page2")))
            ck(any(r.startswith("/page2") for r in requested) and
               p2.find_color(PAGE2_RGB) is not None and
               p2.find_color(BLOCK_A) is None,
               BROWSER_CHECKS[3],
               "second page requested: %s; its colour on screen: %r; the first "
               "page's colour gone: %r"
               % (any(r.startswith("/page2") for r in requested),
                  p2.find_color(PAGE2_RGB), p2.find_color(BLOCK_A) is None))
else:
    why = ("booted with FS_NIC=off" if not NIC_ON else
           "the Browser never launched" if not browser_live else
           "the desktop never came up")
    for name in BROWSER_CHECKS:
        ck(False, name, why)

# 18 -------------------------------------------------------------------------
# The last thing, deliberately: after nine apps, a browser, a TLS-capable net
# stack and a full compositor have all run, is the PS/2 keyboard still wired to
# a ring-3 process? Proved on a channel the GUI has nothing to do with -- the
# app's Ctrl+S writes a file, and the SHELL reads it back.
KEY_CHECK = ("input: a key pressed after all of that reached a ring-3 app, "
             "and its Ctrl+S wrote a file the shell reads back")
if ui is not None and "TextEdit" in launched_names():
    click(ui, serial, *dock_icon(guest_aex.index("textedit.aex"), len(guest_aex)))
    time.sleep(2.5)
    stamp = "fullsysalive"
    ui.typ(stamp)
    time.sleep(1.0)
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "ctrl"},
                                        "down": True}},
               {"type": "key", "data": {"key": {"type": "qcode", "data": "s"},
                                        "down": True}}])
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "s"},
                                        "down": False}},
               {"type": "key", "data": {"key": {"type": "qcode", "data": "ctrl"},
                                        "down": False}}])
    time.sleep(3.0)
    out = serial.sh("cat /untitled.txt", 30)
    ck(stamp in out, KEY_CHECK,
       "TextEdit saved %r" % (out.strip()[-90:],))
else:
    ck(False, KEY_CHECK,
       "TextEdit never launched, so there was no ring-3 app to type into")

finish(0)
