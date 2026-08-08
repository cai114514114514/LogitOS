#!/usr/bin/env python3
"""What one frame costs ON THE MACHINE, with the engine's share separated out.

qmp_aui_bench.py answers "what does a frame cost". This answers the question
that matters for a rendering engine: HOW MUCH OF IT IS THE ENGINE. The gallery
is built against an aui.c compiled with -DAUI_COST, which brackets every drawing
syscall with CLOCK_MONOTONIC and sorts the time into four buckets --

    clear    aui_begin()'s unconditional gui_clear of the whole window
    text     gui_text_run + text_measure_px (the kernel's glyph rasterizer)
    shape    gui_rect / gui_blit / gui_rrect / gui_glass / gui_icon, i.e. every
             call Open Logit's masks and bands go out through
    other    gui_clip + gui_flush

-- and prints them on the serial console. The point of the split is on the
record: the toolkit line measured 24-27 ms for a full-window repaint and found
the dominant cost was the clear plus text, NOT the rasterized primitives. A
frame total that does not separate those credits the engine with a cost it does
not pay.

Read the buckets as a RATIO. The instrumentation costs one extra syscall per
drawing call, so the instrumented total is higher than the real one; `make
bench-aui` gives the uninstrumented total at the same resolution.

Three resolutions, because the clear and the glyph work scale with pixels while
the corner tiles do not -- so the ratio is the interesting part and it MOVES.
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
MODES = [(1280, 800), (1920, 1200), (2560, 1600)]


def run_one(iso, disk, xres, yres):
    scale = configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-gfxbench-")
    sock, serial = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "serial.log")

    print("\n=== %dx%d device px, scale %d%% (TCG) ===" % (xres, yres, scale))
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
        deadline = time.time() + 300
        while time.time() < deadline:
            if os.path.exists(serial) and "desktop live" in open(serial, errors="replace").read():
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early")
                return None
            time.sleep(0.2)
        time.sleep(4 * slow)

        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        ui.click_at_confirmed(probe, *dock_icon(GALLERY_SLOT))
        time.sleep(8 * slow)
        ui.goto(xres - pt(40), pt(40))          # pointer off every widget
        ui.key("tab")
        time.sleep(1.0)
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

    # [gfx] w=.. frames=.. clear_us=.. text_us=.. shape_us=.. other_us=..
    rows = []
    for line in log.splitlines():
        k = line.find("[gfx] ")
        if k < 0:
            continue
        d = {}
        for f in line[k + 6:].split():
            if "=" in f:
                a, b = f.split("=", 1)
                try:
                    d[a] = int(b)
                except ValueError:
                    pass
        if "frames" in d and d["frames"] > 0:
            rows.append(d)
    if not rows:
        print("FAIL the guest never reported a cost split")
        return None

    tot = {k: 0 for k in ("frames", "clear_us", "text_us", "shape_us", "other_us")}
    for d in rows:
        n = d["frames"]
        tot["frames"] += n
        for k in ("clear_us", "text_us", "shape_us", "other_us"):
            tot[k] += d.get(k, 0) * n          # each row is already a per-frame mean
    n = max(1, tot["frames"])
    per = {k: tot[k] / float(n) for k in ("clear_us", "text_us", "shape_us", "other_us")}
    total = sum(per.values())
    print("  %-12s %10s %8s" % ("bucket", "us/frame", "share"))
    for k, name in (("clear_us", "gui_clear"), ("text_us", "text"),
                    ("shape_us", "shapes (engine)"), ("other_us", "clip+flush")):
        print("  %-12s %10.0f %7.1f%%"
              % (name, per[k], 100.0 * per[k] / max(1.0, total)))
    print("  %-12s %10.0f          over %d frames" % ("TOTAL", total, tot["frames"]))
    return per


def main(argv):
    iso = argv[1] if len(argv) > 1 else os.path.join(ROOT, "build", "logit.iso")
    disk = argv[2] if len(argv) > 2 else os.path.join(ROOT, "build", "disk.img")
    print("=== Open Logit: where a frame's time actually goes ===")
    print("(instrumented build: one extra syscall per draw call, so read the")
    print(" SHARES, not the absolute total -- `make bench-aui` gives that)")
    out = []
    for xres, yres in MODES:
        r = run_one(iso, disk, xres, yres)
        if r is None:
            return 1
        out.append(((xres, yres), r))

    print("\n=== summary: the engine's share against the frame ===")
    print("%-12s %10s %10s %10s %10s" % ("mode", "clear", "text", "shapes", "shape %"))
    for (xres, yres), r in out:
        t = sum(r.values())
        print("%-12s %10.0f %10.0f %10.0f %9.1f%%"
              % ("%dx%d" % (xres, yres), r["clear_us"], r["text_us"],
                 r["shape_us"], 100.0 * r["shape_us"] / max(1.0, t)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
