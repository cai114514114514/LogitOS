#!/usr/bin/env python3
"""What does it cost to move the mouse?

The owner's report was "it got laggier", and the cause was that every pixel of
pointer motion recomposited the entire screen -- at a resolution that had just
grown 2.25x. This driver turns that sentence into numbers and then keeps them
honest, in three parts:

  1. COST. Sweep the pointer over the wallpaper (nothing there can change but
     the pointer) and read the compositor's own counters off the serial console
     before and after. The claim being tested is not "fewer composites" but
     "pure motion composites NOTHING": a sweep of hundreds of motion samples
     should leave the composite count at the idle clock floor of ~2/s.

  2. THE MEASUREMENT'S OWN CONTROL (--control). The identical sweep, over the
     DOCK, where hovering magnifies an icon and a recomposite is correct. If
     that sweep also showed ~zero composites the counter would be broken and
     part 1 would prove nothing. This runs against the shipped kernel every
     time, so the instrument is checked on every run rather than once.

  3. NO TRAIL, NO STALE REGIONS. This is the failure mode that killed the
     previous attempt at making motion cheap: a partial-render path with a
     cursor save-under restored stale pixels and smeared garbage across the
     screen. So: photograph the desktop, sweep the pointer across a deliberately
     busy area (a window, the dock, the menu bar), bring it back to exactly
     where it started, photograph again. The two frames must be IDENTICAL
     outside the menu-bar clock, which is the only thing entitled to have
     changed. A trail, a smear or a hole is a differing pixel.

Every number printed is a TCG number: QEMU is emulating x86 instruction by
instruction, so absolute milliseconds mean "on this host, under emulation". The
RATIO between before and after is the part that transfers.

Usage:
    tests/qmp/qmp_cursor.py [--xres W] [--yres H] [--seconds N]
                            [--iso PATH] [--control] [--no-assert]
"""

import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import (CURSOR_RGB, PPM, Session, configure, dock_icon,   # noqa: E402
                    parse_pointer, pt)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

WIDGETS_SLOT = 4          # scan_apps order: clock textedit monitor terminal widgets ...


def perf_samples(text):
    """Every `[wm] perf ...` line in a serial log, as dicts of ints."""
    out = []
    for line in text.splitlines():
        i = line.find("[wm] perf ")
        if i < 0:
            continue
        d = {}
        for tok in line[i + 10:].split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    d[k] = int(v)
                except ValueError:
                    pass
        if "composites" in d and "t" in d:
            out.append(d)
    return out


def delta(a, b):
    """b - a for every counter, plus the wall interval in seconds.

    `max` is deliberately NOT differenced: it is a running maximum, so its
    delta is 0 unless the interval happened to set a new record -- a number
    that reads like "the worst frame took no time at all"."""
    d = {k: b.get(k, 0) - a.get(k, 0) for k in b}
    d["max"] = b.get("max", 0)
    d["secs"] = (b["t"] - a["t"]) / 1000.0
    return d


def wait_sample(ui, serial, prev_n, timeout=6.0):
    """Make the compositor emit a counter line NOW, and return the samples.

    The counters are reported only when a hand is on the machine (see
    wm_perf_report), so bracketing an interval means provoking a line at each
    end -- otherwise the "before" sample is however many idle seconds old, and
    every rate is divided by the wrong interval. One pixel out and back is
    enough, and is two motion samples against the sweep's hundreds."""
    t_end = time.time() + timeout
    while time.time() < t_end:
        for v in (1, -1):
            ui._input([{"type": "rel", "data": {"axis": "x", "value": v}}])
            ui.cur[0] += v
        time.sleep(0.7)
        s = perf_samples(open(serial, errors="replace").read())
        if len(s) > prev_n:
            return s
    return perf_samples(open(serial, errors="replace").read())


def report(name, d):
    secs = d["secs"] or 1e-9
    comp, mot = d["composites"], d["motions"]
    ms = (d["ns"] / comp / 1e6) if comp else 0.0
    print("     %-20s %6.2fs  motions %5d (%6.1f/s)  composites %5d (%5.1f/s)"
          % (name, secs, mot, mot / secs, comp, comp / secs))
    print("     %-20s composite %7.2f ms avg (%7.2f ms worst since boot)"
          "  ->  %5.1f fps if it composited back to back"
          % ("", ms, d["max"] / 1e6, 1000.0 / ms if ms else 0.0))
    if d.get("curmoves"):
        print("     %-20s cursor-plane moves %5d, %6.1f us each  (%.4f%% of a "
              "composite)" % ("", d["curmoves"], d["curns"] / d["curmoves"] / 1000.0,
                              100.0 * (d["curns"] / d["curmoves"]) / (ms * 1e6) if ms else 0.0))
    return comp, mot, ms


def diff_pixels(a, b, skip_top):
    """Count and bound the pixels that differ between two frames below `skip_top`."""
    n = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    row = a.w * 3
    for y in range(skip_top, min(a.h, b.h)):
        ra = a.px[y * row:(y + 1) * row]
        rb = b.px[y * row:(y + 1) * row]
        if ra == rb:
            continue
        for x in range(a.w):
            if ra[x * 3:x * 3 + 3] != rb[x * 3:x * 3 + 3]:
                n += 1
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return n, (None if x1 < 0 else (x0, y0, x1, y1))


def sweep(ui, y, x_lo, x_hi, seconds, step):
    """Drag the pointer back and forth along a row for `seconds`."""
    ui.goto(x_lo, y, settle=0.1)
    t_end = time.time() + seconds
    x, d = x_lo, step
    while time.time() < t_end:
        x += d
        if x >= x_hi:
            x, d = x_hi, -step
        elif x <= x_lo:
            x, d = x_lo, step
        ui._input([{"type": "rel", "data": {"axis": "x", "value": d}}])
        ui.cur[0] = x
        time.sleep(0.004)
    time.sleep(0.5)


def main(argv):
    xres, yres, seconds = 1920, 1200, 8.0
    iso = None
    control = False
    do_assert = True
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":     xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres":   yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--seconds": seconds = float(argv[i + 1]); i += 2
        elif argv[i] == "--iso":    iso = argv[i + 1]; i += 2
        elif argv[i] == "--control": control = True; i += 1
        elif argv[i] == "--no-assert": do_assert = False; i += 1
        else: print("unknown arg %r" % argv[i]); return 2

    if iso is None:
        iso = os.path.join(ROOT, "build", "logit.iso")
    scale = configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-cursor-")
    sock, serial = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "serial.log")
    fails = []

    def ck(cond, what):
        print("%-4s %s" % ("ok" if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    print("=== %dx%d device px (scale %d%%), %s ===  [all timings are TCG]"
          % (xres, yres, scale, os.path.relpath(iso, ROOT)))

    qemu = subprocess.Popen(
        ["qemu-system-x86_64",
         "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off"
                   % os.path.join(ROOT, "build", "disk.img"),
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
         "-rtc", "base=localtime",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (xres, yres),
         "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         "-serial", "file:" + serial, "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 180
        while time.time() < deadline:
            if os.path.exists(serial) and "desktop live" in open(serial, errors="replace").read():
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early"); return 1
            time.sleep(0.2)
        time.sleep(4 * slow)

        log = open(serial, errors="replace").read()
        plane = "[wm] pointer: display cursor plane" in log
        for tag in ("[virtio-gpu]", "[wm] pointer:"):
            for l in log.splitlines():
                if l.startswith(tag):
                    print("     " + l.strip())
        print("     pointer path: %s" % ("DISPLAY CURSOR PLANE" if plane
                                         else "composited into the frame"))

        ui = Session(sock, serial=serial)

        # An app window, so the sweep in part 3 crosses real content rather than
        # a flat wallpaper -- a stale region over a gradient can hide.
        ui.click_at(*dock_icon(WIDGETS_SLOT))
        time.sleep(8 * slow)

        # ---- 1/2. Cost of motion ------------------------------------------
        # Wallpaper row: below the menu bar, above the dock, to the LEFT of
        # where the WM cascades its windows. Nothing there can repaint.
        if control:
            row = dock_icon(0)[1]                     # straight through the dock
            lo, hi = dock_icon(0)[0], dock_icon(6)[0]
            what = "sweep over the DOCK (hover magnifies -- must composite)"
        else:
            row = yres - pt(220)
            lo, hi = pt(20), pt(240)
            what = "sweep over the WALLPAPER (nothing but the pointer moves)"
        print("     %s, %.0fs" % (what, seconds))

        n0 = len(perf_samples(open(serial, errors="replace").read()))
        before = wait_sample(ui, serial, n0)
        sweep(ui, row, lo, hi, seconds, 6)
        after = wait_sample(ui, serial, len(before))
        ck(len(after) > len(before), "the compositor reported its counters during the sweep")
        if not after or not before:
            return 1
        d = delta(before[-1], after[-1])
        comp, mot, ms = report("during the sweep", d)
        ck(mot > 100, "the sweep really moved the pointer (%d samples)" % mot)

        if control:
            # The instrument's own control: this sweep MUST composite.
            ck(comp > d["secs"] * 5,
               "CONTROL: a hover sweep composites (%d frames in %.1fs) -- the "
               "counter can see a recompositing sweep" % (comp, d["secs"]))
        elif do_assert:
            # The idle floor is the 2 Hz menu clock; allow a little slack for
            # the launch bounce and for entering/leaving the sweep.
            budget = int(d["secs"] * 4) + 10
            ck(comp <= budget,
               "pure pointer motion composites nothing: %d frames for %d motion "
               "samples in %.1fs (idle floor allows %d)"
               % (comp, mot, d["secs"], budget))

        # ---- 3. No trail, no stale regions --------------------------------
        rest = (pt(40), pt(120))                 # wallpaper, far from everything
        ui.settle_pointer(os.path.join(tmp, "p.ppm"), *rest)
        time.sleep(1.5 * slow)
        a_p = os.path.join(tmp, "rest0.ppm")
        ui.screendump(a_p, settle=1.0 * slow)

        # Busy area: across the window, through the dock, along the menu bar.
        for (tx, ty) in [(xres // 2, yres // 2), (xres // 2, yres - pt(60)),
                         (dock_icon(2)[0], dock_icon(2)[1]),
                         (dock_icon(5)[0], dock_icon(5)[1]),
                         (xres // 2, pt(12)), (xres - pt(60), pt(12)),
                         (xres // 2, yres // 2)]:
            ui.goto(tx, ty, settle=0.15)
        ui.settle_pointer(os.path.join(tmp, "p.ppm"), *rest)
        time.sleep(1.5 * slow)
        b_p = os.path.join(tmp, "rest1.ppm")
        ui.screendump(b_p, settle=1.0 * slow)

        a, b = PPM(a_p), PPM(b_p)
        # The menu-bar strip holds the clock, which is the one thing allowed to
        # differ between two frames taken seconds apart.
        n, box = diff_pixels(a, b, pt(24))
        print("     frames before/after the busy sweep differ in %d px below the "
              "menu bar%s" % (n, "" if box is None else " (bbox %r)" % (box,)))
        ck(n == 0, "the pointer left NO trail and NO stale region: the frame "
                   "after a busy sweep is identical to the frame before it")

        if plane:
            ck(b.find_color(CURSOR_RGB) is None,
               "the pointer is not in the scanout at all (it is a display plane)")

        ck(ui.guest_pointer() == rest,
           "the guest reports the pointer where the harness put it %r" % (rest,))
    finally:
        qemu.kill()
        qemu.wait()

    if fails:
        print("\nqmp_cursor: %d FAILED" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("\nqmp_cursor: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
