#!/usr/bin/env python3
"""Measure what one aui frame costs, on the machine, at a real resolution.

The gallery times its own repaint and prints it on the serial console; this
boots the desktop, opens Gallery, visits every page, and reports what the guest
said. Nothing here estimates anything -- the numbers come from
CLOCK_MONOTONIC inside the app, around exactly the code an app pays for.

The pages are deliberately unequal. Controls is a normal app frame. Shapes is
the pathological one: every primitive that rasterizes, several times, including
four elevations of shadow. Reporting only the average of the two would hide both.
"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, dock_icon, pt, GALLERY_SLOT  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAGES = ["Controls", "Shapes", "Data", "Overlays"]


def main(argv):
    xres, yres = 1920, 1200
    iso = os.path.join(ROOT, "build", "logit.iso")
    disk = os.path.join(ROOT, "build", "disk.img")
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":   xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres": yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--iso":  iso = argv[i + 1]; i += 2
        elif argv[i] == "--disk": disk = argv[i + 1]; i += 2
        else: print("unknown arg %r" % argv[i]); return 2

    scale = configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-auibench-")
    sock, serial = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "serial.log")

    print("=== aui frame cost: %dx%d device px, scale %d%% (TCG) ===" % (xres, yres, scale))
    qemu = subprocess.Popen(
        ["qemu-system-x86_64",
         "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
         "-rtc", "base=localtime",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (xres, yres),
         "-serial", "file:" + serial, "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 240
        while time.time() < deadline:
            if os.path.exists(serial) and "desktop live" in open(serial, errors="replace").read():
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early"); return 1
            time.sleep(0.2)
        time.sleep(4 * slow)

        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        ui.click_at_confirmed(probe, *dock_icon(GALLERY_SLOT))
        time.sleep(8 * slow)
        ui.goto(xres - pt(40), pt(40))          # pointer off every widget

        # Tab once to reach the tab strip, then walk the pages with the arrow
        # key, sitting on each long enough for two cost reports to land.
        ui.key("tab"); time.sleep(1.0)
        for p in range(4):
            if p:
                ui.key("right")
            time.sleep(7.0 * slow)

        log = open(serial, errors="replace").read()
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()

    per = {}
    for line in log.splitlines():
        k = line.find("[aui] page ")
        if k < 0:
            continue
        f = line[k:].split()
        try:
            page = int(f[2])
            avg = int(f[4].split("=")[1])
            mx = int(f[5].split("=")[1])
            n = int(f[3].split("=")[1])
        except (IndexError, ValueError):
            continue
        per.setdefault(page, []).append((n, avg, mx))

    if not per:
        print("FAIL the guest never reported a frame cost")
        return 1

    print("\n%-10s %8s %10s %10s" % ("page", "frames", "avg us", "max us"))
    worst = 0
    for page in sorted(per):
        rows = per[page]
        frames = sum(r[0] for r in rows)
        avg = sum(r[0] * r[1] for r in rows) // max(1, frames)
        mx = max(r[2] for r in rows)
        worst = max(worst, avg)
        print("%-10s %8d %10d %10d"
              % (PAGES[page] if page < len(PAGES) else str(page), frames, avg, mx))
    print("\nworst page average: %d us/frame  (%d frames/s if nothing else ran)"
          % (worst, 1000000 // max(1, worst)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
