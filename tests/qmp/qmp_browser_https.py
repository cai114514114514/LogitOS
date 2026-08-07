#!/usr/bin/env python3
"""The on-device proof that an https page's bytes reach the pixels.

    python3 tests/qmp/qmp_browser_https.py <iso> <disk.img> [url] [budget_s]

WHY THIS EXISTS, and why it is not another `test-https-smoke`.

`make test-https-smoke` drives the KERNEL's blocking client (`net get`, i.e.
SYS_HTTP_GET) and asserts a byte count on the serial console. It passed on
www.baidu.com the whole time the Browser could not open that page at all --
because the Browser does not use that client. It uses `bfetch` in ring 3, over
the non-blocking socket ABI plus c/net/http/http1.c, and the two paths differed
in exactly one place: http1.c would skip its read whenever the transport's poll
hint said "not ready". Our TLS is a layer under that hint. tls_rec_pull() drains
the whole TCP receive buffer into the session's record buffer and hands back one
record, so the socket then reports tcp_available() == 0 and tls_pending() == 0
with several complete records still undecrypted. MEASURED, in the pcap: every
byte of www.baidu.com's response arrived at t=15.10 and was ACKed; the Browser
parsed the status line and sat there until the server's close_notify at t=75.10
put one more byte on the wire and flipped SOCK_P_READABLE. Sixty seconds, for a
227-byte page that had already arrived.

So this test asserts the two things the smoke test structurally cannot:

  1. The BROWSER's path completes, and completes inside a budget the stall
     cannot fit inside. One stalled response costs ~60 s (the peer's idle
     timeout); the default budget is 45 s for the whole load, sub-resources
     included. There is no schedule on which the broken build passes this.
  2. The page's bytes reached the PIXELS -- counted as rendered ink inside the
     viewport, against the same viewport photographed before navigation as the
     in-run control. A fetch that succeeds and paints nothing is not a page.

WHICH URL, AND WHY IT IS NOT WIKIPEDIA. This was checked both ways against a
build with the bug reintroduced, and the choice is load-bearing:

  https://zh.wikipedia.org/  loads in 10 s WITH THE BUG. It is TLS 1.3 through
                             a CDN that sends 16 KiB records, so a 4 KiB read
                             always leaves tls_pending() > 0 and the readable
                             bit never goes false while a response is in
                             flight. It cannot see this bug at all.
  https://deepseek.com/      blows a 45 s budget with the bug and loads in ~3 s
                             without it.  It has the shape that breaks: TLS 1.2,
                             a dozen small responses each delivered in one burst
                             and split into records smaller than one read, so
                             the wire goes empty while records are still
                             buffered. It is also server-rendered, so its text
                             lands in the pixels rather than behind a framework.

A replacement URL must keep BOTH properties -- the failing shape and visible
text -- or this test silently stops discriminating, which is what a passing
wikipedia run looks like.

It needs outbound Internet, like test-https-smoke, and is likewise not part of
`make test-net`.  HTTPS_BROWSER_URL / the argv override point it elsewhere.
"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM, dock_icon, BROWSER_SLOT          # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else "build/logit.iso"
DISK = sys.argv[2] if len(sys.argv) > 2 else "build/disk.img"
URL = (sys.argv[3] if len(sys.argv) > 3
       else os.environ.get("HTTPS_BROWSER_URL", "https://deepseek.com/"))
BUDGET = float(sys.argv[4] if len(sys.argv) > 4
               else os.environ.get("HTTPS_BROWSER_BUDGET", "45"))

# The page area of the Browser window at 1280x800, inside the chrome and ABOVE
# THE DOCK -- the dock's hover tooltip lands around y=700 and is worth ~780 dark
# pixels all by itself, which is enough to make an empty viewport look like a
# rendered page. Deliberately generous otherwise: this asks "is there ink", not
# "is the layout byte-identical", which is a different and far more fragile test.
VIEWPORT = (110, 200, 1270, 655)
# Somewhere with no window and no dock, so the pointer is not part of the count.
PARK = (55, 400)
# "Ink" is a pixel darker than this in EVERY channel. 160 and not the 90 that
# qmp_ui's default suggests, because real pages do not set #000: deepseek's body
# text is slate (71, 85, 105) and its blue channel alone put every glyph on the
# page above a 90 threshold -- a rendered screen measured as blank. 160 keeps the
# empty viewport at exactly 0 (measured, both screenshots, every threshold up to
# 200), so the control below is what makes this number safe rather than lucky.
INK_THRESH = 160
# A rendered page of text is thousands of ink pixels; an empty viewport is zero.
INK_MIN = 400
# Paint lags the last byte: the page's scripts run, then style, layout, repaint.
# Poll for it rather than sleeping a guessed amount -- and keep it a separate
# budget from the network one, so a failure says which half was slow.
PAINT_BUDGET = 40.0

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
tmp = tempfile.mkdtemp(prefix="qmp_browser_https_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
pcap_path = os.path.join(tmp, "net.pcap")

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     # The pcap is the artefact that told us where the bytes were. Keep it: when
     # this test fails, "the response never arrived" and "the response arrived
     # and we did not read it" are the two answers, and only this separates them.
     "-object", "filter-dump,id=dump0,netdev=n0,file=%s" % pcap_path,
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
    print("----- artefacts in %s (serial.log, net.pcap, *.ppm) -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-6000:])
    print("-------------------------")
    proc.kill()
    sys.exit(1)


def ck(cond, name):
    checks.append((bool(cond), name))
    print(("ok:   " if cond else "FAIL: ") + name)
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
    time.sleep(6)                      # ~3 MB .aex off virtio-blk, ELF load, first paint

    # The control, taken in this same run: the viewport BEFORE any page. If the
    # window never opened, this is not empty for a reason that has nothing to do
    # with the page, and the ink assertion below would be measuring the desktop.
    before = os.path.join(tmp, "before.ppm")
    ui.goto(*PARK)
    ui.screendump(before, settle=0.5)
    ink_before = PPM(before).dark_pixels(VIEWPORT, INK_THRESH)
    ck(ink_before < INK_MIN,
       "the empty Browser viewport is blank (%d ink px, control)" % ink_before)

    mark = len(serial())
    ui.click_at(420, 145)
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ(URL)
    t0 = time.time()
    ui.key("ret")

    # THE DISCRIMINATING ASSERTION. One response stalled behind a TLS record
    # buffer costs the peer's idle timeout -- ~60 s, measured -- so a build with
    # that bug cannot reach `load done` inside BUDGET no matter how fast the
    # network is. A build without it loads this page in a few seconds.
    ok = False
    while time.time() - t0 < BUDGET:
        s = serial()[mark:]
        if "[browser] load done:" in s:
            ok = True
            break
        if "[browser] page fetch failed" in s:
            die("the Browser reported a failed fetch: " +
                [l for l in s.splitlines() if "page fetch failed" in l][-1])
        if proc.poll() is not None:
            die("QEMU exited during the load")
        time.sleep(0.5)
    elapsed = time.time() - t0
    ck(ok, "%s loaded in %.1fs (budget %.0fs)" % (URL, elapsed, BUDGET))

    line = [l for l in serial()[mark:].splitlines() if "[browser] load done:" in l][-1]
    print("      " + line.strip())

    # The load-done line is about the network. The pixels are what this test is
    # actually about, and they arrive after the scripts run and the page is laid
    # out again -- so wait for them, on their own clock.
    after = os.path.join(tmp, "after.ppm")
    ui.goto(*PARK)
    t1 = time.time()
    ink_after = 0
    while time.time() - t1 < PAINT_BUDGET:
        ui.screendump(after, settle=0.4)
        ink_after = PPM(after).dark_pixels(VIEWPORT, INK_THRESH)
        if ink_after >= INK_MIN:
            break
        if proc.poll() is not None:
            die("QEMU exited while waiting for the page to paint")
        time.sleep(2)
    ck(ink_after >= INK_MIN,
       "the page's bytes reached the pixels (%d ink px in the viewport after "
       "%.1fs, control was %d)" % (ink_after, time.time() - t1, ink_before))

    print("\nPASS: the Browser's own path opened %s and painted it" % URL)
    print("      artefacts in %s" % tmp)
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as e:                                    # noqa: BLE001
    die("driver error: %r" % (e,))
