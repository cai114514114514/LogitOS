#!/usr/bin/env python3
"""What does a REPAINT cost, per kind of interaction?

The pointer moved to the display's cursor plane, and pure motion stopped costing
anything. The owner's answer to that was that the repaint problem was not
fundamentally solved -- and it was not, because everything that changes the
PICTURE still recomposited the whole screen: a titlebar drag, a dock hover, an
app repainting after a keystroke, a theme flip, a scroll. Each of those cost one
full frame of wallpaper + every window + the frosted menu bar + the frosted dock,
plus a full-framebuffer transfer to the host.

"It still feels laggy" is not a number. This driver turns the sentence into one
table, per EVENT CLASS, at three display modes:

    drag    a window dragged by its titlebar
    dock    the pointer swept across the dock (hover magnifies an icon)
    type    keystrokes into TextEdit (the app repaints and flushes)
    theme   the menu-bar dark-mode switch (every window must repaint)
    scroll  wheel notches over the Terminal's scrollback

For each it reports, from the compositor's OWN counters (wm.c wm_perf_report):

  * composites -- how many full recomposites the interaction provoked
  * ms/composite -- what one of them cost. This is the number a dirty-rectangle
    change has to move; the composite RATE is set by the interaction, not by
    the compositor.
  * composited px / presented px per frame -- what the frame actually touched.
    A kernel without damage tracking does not report these, and their absence
    is itself the reading: every frame was the whole screen.

Everything here is TCG. The absolute milliseconds mean "on this host, under
emulation, while other things were running on it" -- which is why every workload
runs REPS times and the report carries a median and the spread. A single sample
from a shared host is how a line reports a regression that was its own
neighbour's build.

Usage:
    tests/qmp/qmp_repaint.py [--xres W] [--yres H] [--iso PATH]
                             [--reps N] [--only NAME[,NAME...]] [--json PATH]
"""

import json as _json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, configure, dock_icon, pt   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TEXTEDIT_SLOT = 1
TERMINAL_SLOT = 3

# The focused window's close button, from draw_frame() in c/kernel/gui/wm.c.
# Only the FOCUSED window paints it; every other window's lights are grey. So
# the bounding box of this exact colour is the focused titlebar, wherever the
# cascade happened to put it -- which is the only way to aim at a titlebar
# without duplicating the WM's window-placement arithmetic in the harness.
CLOSE_RGB = (255, 95, 86)


# ---------------------------------------------------------------------------
# the compositor's counters

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


class Meter:
    """Brackets an interaction with two counter lines off the serial console.

    The compositor reports only when a hand is on the machine (wm_perf_report),
    at most once a second, so an interval has to be PROVOKED at both ends --
    otherwise the "before" sample is however many idle seconds old and every
    rate is divided by the wrong interval. One pixel out and back is enough, and
    is two motion samples against a workload's hundreds."""

    def __init__(self, ui, serial):
        self.ui, self.serial = ui, serial

    def _read(self):
        with open(self.serial, errors="replace") as fh:
            return perf_samples(fh.read())

    def mark(self, timeout=4.0):
        n0 = len(self._read())
        t_end = time.time() + timeout
        while time.time() < t_end:
            for v in (1, -1):
                self.ui._input([{"type": "rel", "data": {"axis": "x", "value": v}}])
                self.ui.cur[0] += v
            for _ in range(12):
                time.sleep(0.1)
                s = self._read()
                if len(s) > n0:
                    return s[-1]
        s = self._read()
        return s[-1] if s else None

    def run(self, fn, *a, **kw):
        before = self.mark()
        t0 = time.time()
        fn(*a, **kw)
        wall = time.time() - t0
        after = self.mark()
        if before is None or after is None:
            return None
        d = {k: after.get(k, 0) - before.get(k, 0) for k in after}
        d["secs"] = (after["t"] - before["t"]) / 1000.0
        d["work_secs"] = wall
        return d


def stats(rows, key):
    v = sorted(r[key] for r in rows)
    if not v:
        return (0, 0, 0)
    return (v[len(v) // 2], v[0], v[-1])


def summarize(rows):
    """Median (min..max) of the per-rep derived numbers."""
    per = []
    for d in rows:
        comp = d["composites"]
        per.append({
            "composites": comp,
            "ms": (d["ns"] / comp / 1e6) if comp else 0.0,
            "presms": (d.get("presns", 0) / comp / 1e6) if comp else 0.0,
            "cps": comp / d["secs"] if d["secs"] else 0.0,
            # cpx/fpx exist only on a kernel that tracks damage. Absent means
            # "every frame was the whole screen", which is the baseline reading.
            "cpx": (d.get("cpx", 0) / comp) if comp else 0.0,
            "fpx": (d.get("fpx", 0) / comp) if comp else 0.0,
            "full": d.get("full", -1),
            "rects": d.get("rects", -1),
            "secs": d["secs"],
        })
    out = {}
    for k in ("composites", "ms", "presms", "cps", "cpx", "fpx", "secs"):
        out[k] = stats(per, k)
    out["full"] = stats(per, "full")[0]
    out["rects"] = stats(per, "rects")[0]
    out["reps"] = per
    return out


# ---------------------------------------------------------------------------
# the workloads. Fixed STEP COUNTS, not fixed durations: a before/after pair has
# to perform the identical interaction, and a host under someone else's load
# would otherwise deliver a different number of samples to each side.

def focused_titlebar(ui, tmp):
    """A point on the focused window's titlebar, right of its traffic lights."""
    p = os.path.join(tmp, "title.ppm")
    ui.screendump(p, settle=0.6)
    box = PPM(p).find_color(CLOSE_RGB)
    if box is None:
        return None
    x0, y0, x1, y1 = box
    return (x1 + pt(120), (y0 + y1) // 2)


def w_drag(ui, geo, steps=180):
    """Grab the focused titlebar and walk the window back and forth."""
    tx, ty = geo["title"]
    ui.goto(tx, ty, settle=0.2)
    ui._input([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.05)
    d = pt(6)
    for i in range(steps):
        if (i // 15) % 2:
            d = -abs(d)
        else:
            d = abs(d)
        ui._input([{"type": "rel", "data": {"axis": "x", "value": d}}])
        ui.cur[0] += d
        time.sleep(0.005)
    ui._input([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.2)


def w_dock(ui, geo, steps=200):
    """Sweep the pointer along the dock row, crossing icon boundaries."""
    lo, row = dock_icon(0)
    hi = dock_icon(6)[0]
    ui.goto(lo, row, settle=0.2)
    x, d = lo, pt(6)
    for _ in range(steps):
        x += d
        if x >= hi:
            x, d = hi, -abs(d)
        elif x <= lo:
            x, d = lo, abs(d)
        ui._input([{"type": "rel", "data": {"axis": "x", "value": d}}])
        ui.cur[0] = x
        time.sleep(0.005)
    time.sleep(0.2)


def w_type(ui, geo, steps=48):
    """Keystrokes into TextEdit: the app repaints and flushes each time."""
    for i in range(steps):
        ui.key("abcdefghijklmnopqrstuvwxyz"[i % 26], settle=0.02)
    time.sleep(0.3)


def w_theme(ui, geo, steps=10):
    """The menu-bar dark-mode switch. Every window must repaint: this is the
    workload where a FULL-SCREEN repaint is the correct answer, and it is here
    to prove the damage tracking still produces one."""
    x, y = geo["toggle"]
    ui.goto(x, y, settle=0.15)
    for _ in range(steps):
        ui.click(hold=0.08)
        time.sleep(0.18)
    time.sleep(0.3)


def w_scroll(ui, geo, steps=48):
    """Wheel notches over the Terminal's scrollback."""
    x, y = geo["content"]
    ui.goto(x, y, settle=0.2)
    for i in range(steps):
        btn = "wheel-up" if (i // 8) % 2 else "wheel-down"
        ui._input([{"type": "btn", "data": {"button": btn, "down": True}},
                   {"type": "btn", "data": {"button": btn, "down": False}}])
        time.sleep(0.02)
    time.sleep(0.3)


WORKLOADS = [
    ("drag",   w_drag,   "a window dragged by its titlebar (the 900pt Terminal)"),
    ("dock",   w_dock,   "the pointer swept across the dock (hover magnifies)"),
    ("type",   w_type,   "keystrokes into TextEdit (app repaint + flush)"),
    ("theme",  w_theme,  "the dark-mode switch (every window repaints)"),
    ("scroll", w_scroll, "wheel notches over the Terminal"),
]


# ---------------------------------------------------------------------------

def boot(iso, xres, yres, tmp):
    sock, serial = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "serial.log")
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
    deadline = time.time() + 240
    while time.time() < deadline:
        if os.path.exists(serial) and "desktop live" in open(serial, errors="replace").read():
            return qemu, sock, serial
        if qemu.poll() is not None:
            raise RuntimeError("qemu exited early")
        time.sleep(0.2)
    raise RuntimeError("guest never reported a live desktop")


def main(argv):
    xres, yres, reps = 1920, 1200, 3
    iso, only, jpath = None, None, None
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":    xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres":  yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--reps":  reps = int(argv[i + 1]); i += 2
        elif argv[i] == "--iso":   iso = argv[i + 1]; i += 2
        elif argv[i] == "--only":  only = argv[i + 1].split(","); i += 2
        elif argv[i] == "--json":  jpath = argv[i + 1]; i += 2
        else:
            print("unknown arg %r" % argv[i]); return 2
    if iso is None:
        iso = os.path.join(ROOT, "build", "logit.iso")

    scale = configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-repaint-")
    print("=== %dx%d device px (scale %d%%), %s ===  [all timings are TCG]"
          % (xres, yres, scale, os.path.relpath(iso, ROOT)))

    qemu, sock, serial = boot(iso, xres, yres, tmp)
    result = {}
    try:
        time.sleep(4 * slow)
        log = open(serial, errors="replace").read()
        for tag in ("[virtio-gpu]", "[wm] display", "[wm] pointer:"):
            for l in log.splitlines():
                if l.startswith(tag):
                    print("     " + l.strip())

        ui = Session(sock, serial=serial)
        meter = Meter(ui, serial)

        def measure(name, fn, what, geo):
            rows = []
            for r in range(reps):
                d = meter.run(fn, ui, geo)
                if d is None:
                    print("     %-7s rep %d: no counter line" % (name, r))
                    continue
                rows.append(d)
            if not rows:
                return
            s = summarize(rows)
            result[name] = s
            print("  %-10s %s" % (name, what))
            print("     composites %5d (%d..%d)   %6.1f/s   %8.2f ms each "
                  "(%.2f..%.2f)  -> %5.1f fps back to back"
                  % (s["composites"][0], s["composites"][1], s["composites"][2],
                     s["cps"][0], s["ms"][0], s["ms"][1], s["ms"][2],
                     1000.0 / s["ms"][0] if s["ms"][0] else 0.0))
            if s["cpx"][0] > 0 or s["fpx"][0] > 0:
                px = float(xres * yres)
                print("     composited %9.0f px/frame (%5.1f%% of the screen)   "
                      "presented %9.0f px/frame (%5.1f%%)"
                      % (s["cpx"][0], 100.0 * s["cpx"][0] / px,
                         s["fpx"][0], 100.0 * s["fpx"][0] / px))
                print("     of which present (copy + DMA) %6.2f ms (%4.1f%% of a frame)"
                      "   full-screen frames %d, rects %d"
                      % (s["presms"][0],
                         100.0 * s["presms"][0] / s["ms"][0] if s["ms"][0] else 0.0,
                         s["full"], s["rects"]))
            else:
                print("     (kernel reports no damage counters: every frame is the "
                      "whole %d x %d screen)" % (xres, yres))

        # PHASE 1, before anything big is on screen: the Clock, which asks for a
        # small window. Damage tracking can only ever be as good as the extent an
        # app reports, and SYS_GUI_FLUSH reports "my whole canvas" -- so the size
        # of that canvas IS the result for every app-repaint class, and a table
        # that only measured the biggest window on the desktop would be quoting
        # its worst case as its result.
        if not only or "drag-small" in only:
            t = focused_titlebar(ui, tmp)
            if t:
                measure("drag-small", w_drag,
                        "a SMALL window (the Clock) dragged by its titlebar",
                        {"title": t})

        # TextEdit, then the Terminal: two more windows, the Terminal focused.
        ui.click_at(*dock_icon(TEXTEDIT_SLOT))
        time.sleep(6 * slow)
        ui.click_at(*dock_icon(TERMINAL_SLOT))
        time.sleep(10 * slow)

        title = focused_titlebar(ui, tmp)
        if title is None:
            print("FAIL could not find a focused titlebar (no window?)")
            return 1
        geo = {
            "title": title,
            "content": (title[0], title[1] + pt(120)),
            # menu_tog_* from draw_menubar() in c/kernel/gui/wm.c
            "toggle": (xres - pt(210) + pt(19), (pt(24) - pt(18)) // 2 + pt(9)),
        }
        print("     focused titlebar at %r, toggle at %r" % (geo["title"], geo["toggle"]))

        for name, fn, what in WORKLOADS:
            if only and name not in only:
                continue
            measure(name, fn, what, geo)
    finally:
        qemu.kill()
        qemu.wait()

    if jpath:
        with open(jpath, "w") as fh:
            _json.dump({"xres": xres, "yres": yres, "scale": scale,
                        "iso": iso, "workloads": result}, fh, indent=1)
        print("     wrote %s" % jpath)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
