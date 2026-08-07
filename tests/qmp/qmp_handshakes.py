#!/usr/bin/env python3
"""Count the TLS handshakes ONE REAL PAGE costs, on the real machine.

    python3 tests/qmp/qmp_handshakes.py <iso> <disk.img> [url] [max] [out.ppm]

This is the number the whole connection-pool exercise is about, and it is
measured the same way before and after: the kernel's TLS layer prints one

    [tls] chain of N verified for <host>

line per completed handshake, so `grep -c 'chain of'` on the guest's serial log
IS the handshake count. Nothing in the browser produces that line, so the metric
cannot be gamed from the side being changed.

The baseline, measured on 21abe20 with en.wikipedia.org/wiki/Operating_system:
16 handshakes, zero reuse -- eight of them for eight images from one host,
because every sub-resource went through SYS_RES_FETCH, which sends
`Connection: close`.

The script also screendumps the page, because a handshake count that fell
because the page stopped loading its resources is not an improvement. Read the
picture, not just the number.

Exit status is 0 unless a `max` was given and the count exceeded it.
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM, dock_icon, BROWSER_SLOT      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
URL = sys.argv[3] if len(sys.argv) > 3 else "https://en.wikipedia.org/wiki/Operating_system"
MAXHS = int(sys.argv[4]) if len(sys.argv) > 4 else 0
OUT = sys.argv[5] if len(sys.argv) > 5 else None
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

tmp = tempfile.mkdtemp(prefix="qmp_hs_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot", "-rtc", "base=localtime",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    print("----- artefacts in %s -----" % tmp)
    print(serial()[-6000:])
    proc.kill()
    sys.exit(2)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    return False


def handshakes(log=None):
    return len(re.findall(r"chain of ", log if log is not None else serial()))


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

    hs_before_page = handshakes()

    # The browser starts with the address bar focused, so no click is needed --
    # and a click at a hardcoded coordinate would land on the title bar anyway.
    for _ in range(90):
        ui.key("backspace", settle=0.02)
    ui.typ(URL)
    ui.key("ret")
    t0 = time.time()

    # Wait for the page to go quiet rather than for a fixed time: "quiet" is
    # what the count is about, and a fixed sleep either truncates a slow load or
    # pads a fast one.
    last, stable_since = -1, time.time()
    while time.time() - t0 < 240:
        n = handshakes()
        if n != last:
            last, stable_since = n, time.time()
        elif time.time() - stable_since > 25:
            break
        if proc.poll() is not None:
            die("QEMU exited during the load")
        time.sleep(1.0)
    elapsed = time.time() - t0

    time.sleep(2)
    shot = OUT or os.path.join(tmp, "page.ppm")
    ui.screendump(shot)
    img = PPM(shot)

    log = serial()
    total = handshakes(log)
    page_hs = total - hs_before_page
    hosts = {}
    for h in re.findall(r"chain of \d+ verified for ([^\s,]+)", log):
        hosts[h] = hosts.get(h, 0) + 1
    rejected = len(re.findall(r"chain of \d+ rejected", log))

    stats = re.findall(r"\[browser\] load done: (\d+) requests, (\d+) connections "
                       r"dialled, (\d+) reused", log)

    print()
    print("URL                 : %s" % URL)
    print("TLS HANDSHAKES      : %d  (grep -c 'chain of')" % total)
    print("  during this page  : %d" % page_hs)
    print("  rejected chains   : %d" % rejected)
    for h, n in sorted(hosts.items(), key=lambda kv: -kv[1]):
        print("      %-40s %d" % (h, n))
    if stats:
        r, d, u = stats[-1]
        print("REQUESTS / DIALS    : %s requests, %s connections dialled, %s reused" % (r, d, u))
    else:
        print("REQUESTS / DIALS    : (this build does not report them)")
    print("load settled after  : %.0fs" % elapsed)
    print("screenshot          : %s (%dx%d)" % (shot, img.w, img.h))
    # A handshake count that fell because the page rendered nothing is not a
    # win, so report how much was actually painted.
    dark = img.dark_pixels((0, 0, img.w - 1, img.h - 1))
    print("near-black pixels   : %d  (how much text is on screen)" % dark)

    proc.kill()
    if MAXHS and total > MAXHS:
        print("\nFAIL: %d handshakes, expected at most %d" % (total, MAXHS))
        sys.exit(1)
    print("\nOK")
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
