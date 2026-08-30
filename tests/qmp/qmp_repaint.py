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
    anim    a widget animation: the Settings toggle, flipped six times

The last one is a different question from the other five and arrived later. They
measure a repaint somebody else provoked; `anim` measures the toolkit's own
motion core (c/apps/gui/aui.c section 5c) and asks not "what does a frame cost"
but "how many frames did an interaction ask for, and DID THE ASKING STOP". It
runs twice -- with the window clear of the dock and overlapping it -- because
the compositor grows any damage touching a glass panel to the whole panel, so
the same widget costs ~1.8x depending only on where the user left the window,
and that is invisible from inside the toolkit.

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
                             [--assert] [--expect-off N]

--assert turns the `anim` class into a gate (see assert_anim at the bottom).
--expect-off carries the composite count from a -DAUI_ANIM_OFF build; without
it the positive assertion is a thermometer rather than a control, and the gate
says so out loud rather than passing quietly. `make test-anim` runs both sides.
"""

import json as _json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import (PPM, Session, configure, dock_icon,     # noqa: E402
                    dock_icon_of, pt)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# TEXTEDIT_SLOT = 1 / TERMINAL_SLOT = 3 used to live here, and SETTINGS_SLOT
# was imported from qmp_ui. Every tile this driver clicks is looked up on the
# guest's [wm] dock line now; the dock-band geometry below uses the band the
# guest names (first icon through last icon), not icons 0..6 of a guessed
# count.
# settings.c's page probe: a 6x6 pt swatch at window-local (4,4), one colour per
# tab, painted for exactly this purpose. It answers two questions no amount of
# coordinate arithmetic in this file could: where the Settings window's content
# origin actually is, and which page is on screen. (The tile that opens the
# window comes from the guest's dock line; the probe answers the rest.)
SETTINGS_PROBE = {(0xFF, 0x00, 0x80): 0,    # Appearance  (the default tab)
                  (0x00, 0xFF, 0x80): 1,    # Desktop     (the animated toggle)
                  (0xFF, 0xC8, 0x00): 2,    # Network
                  (0x00, 0xA0, 0xFF): 3}    # All settings
PROBE_PT = (4, 4)
SETTINGS_TAB_Y_PT = 52 + 17          # aui_tabs strip: cut at AUI_SP(13), h=34
SETTINGS_WINH_PT = 480               # settings.c:37 WINH -- see anim_push_to_dock
# How many times the `anim` class flips the toggle. ONE number: w_anim's
# default and the floor assert_anim() multiplies. Spelled twice it would be a
# gate whose expectation and whose workload disagree, which passes.
ANIM_FLIPS = 6

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
    got = ui.settle_pointer(geo["ppm"], tx, ty)      # confirmed: see w_theme
    if got != (tx, ty):
        print("     warning: pointer would not settle on the titlebar (%r, wanted "
              "%r) -- this row is not a measurement" % (got, (tx, ty)))
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
    # The band is the guest's dock: first tile through last tile. "Icon 6" was
    # a third of the row once Settings was packed and nobody had said so.
    n = len(ui.dock())
    lo, row = dock_icon(0, n)
    hi = dock_icon(n - 1, n)[0]
    got = ui.settle_pointer(geo["ppm"], lo, row)      # confirmed: see w_theme
    if got != (lo, row):
        print("     warning: pointer would not settle on the dock (%r, wanted "
              "%r) -- this row is not a measurement" % (got, (lo, row)))
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
    to prove the damage tracking still produces one.

    CONFIRMED against the guest's own pointer report, not dead-reckoned. The
    switch is 38 x 18 points in the top-right corner and the pointer arrives
    from wherever the previous workload left it, which on a large desktop is a
    move long enough to lose PS/2 packets. A click two pixels off does nothing,
    silently, and the row then reads as "the theme flip composites almost
    nothing" -- which is a true statement about a click that never happened and
    would have been reported as a result."""
    x, y = geo["toggle"]
    got = ui.settle_pointer(geo["ppm"], x, y)
    if got != (x, y):
        print("     warning: pointer would not settle on the dark-mode switch "
              "(%r, wanted %r) -- this row is not a measurement" % (got, (x, y)))
    for _ in range(steps):
        ui.click(hold=0.08)
        time.sleep(0.18)
    time.sleep(0.3)


def w_scroll(ui, geo, steps=48):
    """Wheel notches over the Terminal's scrollback."""
    x, y = geo["content"]
    got = ui.settle_pointer(geo["ppm"], x, y)         # confirmed: see w_theme
    if got != (x, y):
        print("     warning: pointer would not settle in the window (%r, wanted "
              "%r) -- this row is not a measurement" % (got, (x, y)))
    for i in range(steps):
        btn = "wheel-up" if (i // 8) % 2 else "wheel-down"
        ui._input([{"type": "btn", "data": {"button": btn, "down": True}},
                   {"type": "btn", "data": {"button": btn, "down": False}}])
        time.sleep(0.02)
    time.sleep(0.3)


# ---------------------------------------------------------------------------
# the `anim` class: what a WIDGET animation costs, and whether it stops.
#
# The five classes above all measure a repaint somebody else provoked. This one
# measures the toolkit's own motion core (c/apps/gui/aui.c section 5c), and it
# is a different shape of question: not "what does a frame cost" -- the classes
# above already answer that -- but "how many frames did an interaction ask for,
# and did the asking stop".
#
# NOTHING HERE IS DEAD-RECKONED, and that is the whole reason it is this long.
# tools/check-test-liveness.py names five drivers in this tree that click a
# coordinate which has quietly become part of the window-manager titlebar and
# pass anyway, because the thing they wanted was focused already. Both hops
# below are confirmed against the guest's own output: the window origin and the
# page come from settings.c's page probe, and the toggle's position comes from
# a serial line settings.c emits from the same rect it draws the toggle at.

def settings_probe(ui, tmp, name="probe.ppm"):
    """(origin_x, origin_y, tab_index) from the page probe, or None."""
    p = ui.screendump(os.path.join(tmp, name), settle=0.5)
    img = PPM(p)
    for rgb, tab in SETTINGS_PROBE.items():
        box = img.find_color(rgb)
        if box is not None:
            x0, y0, _, _ = box
            # The swatch's top-left IS window-local PROBE_PT.
            return (x0 - pt(PROBE_PT[0]), y0 - pt(PROBE_PT[1]), tab)
    return None


def anim_setup(ui, tmp, serial):
    """Open Settings, land on the Desktop tab, and return the geometry the
    `anim` workload needs. Returns None (loudly) if any hop is unconfirmed --
    a row that could not aim is not a row that measured zero."""
    ui.launch_app("settings")
    # POLL FOR THE PROBE, do not sleep a guess at it. The first version waited a
    # flat 6 s, which was enough when five other workloads had already warmed
    # the machine and NOT enough on a cold `--only anim` run -- so the class
    # reported "the window never opened" about a window that opened two seconds
    # later. A fixed settle is a guess whose failure looks like a result.
    got = None
    deadline = time.time() + 40
    while time.time() < deadline:
        got = settings_probe(ui, tmp)
        if got is not None:
            break
        time.sleep(1.0)
    if got is None:
        print("     anim: no Settings page probe on screen -- the window never "
              "opened, or its probe moved. NOT a measurement.")
        return None
    ox, oy, tab = got

    # Hop 1: the Desktop tab. aui_tabs sizes each tab by its MEASURED TEXT
    # WIDTH, so there is no arithmetic that gives its centre without
    # reimplementing text measurement in Python. Walk the strip instead and let
    # the probe say when we have arrived -- eight bounded clicks, and the exit
    # condition is the guest's own report rather than our model of it.
    for wx in range(20, 600, 40):
        if tab == 1:
            break
        ui.click_at(ox + pt(wx), oy + pt(SETTINGS_TAB_Y_PT))
        time.sleep(0.4)
        got = settings_probe(ui, tmp)
        if got is None:
            continue
        ox, oy, tab = got
    if tab != 1:
        print("     anim: could not reach the Settings Desktop tab (probe says "
              "tab %d). NOT a measurement." % tab)
        return None

    # Hop 2: the toggle. settings.c prints its centre in window-local points
    # from the call site that draws it -- see the anim_aim() note there for why
    # this is a serial line and not a constant in this file.
    aim = None
    with open(serial, errors="replace") as fh:
        for line in fh:
            i = line.find("[settings] anim-toggle ")
            if i >= 0:
                try:
                    a, b = line[i + 23:].split()[:2]
                    aim = (int(a), int(b))
                except ValueError:
                    pass
    if aim is None:
        print("     anim: settings.aex never printed its anim-toggle aim. "
              "Either the build is old or page_desktop never drew. NOT a "
              "measurement.")
        return None
    return {"toggle": (ox + pt(aim[0]), oy + pt(aim[1])), "aim": aim,
            "origin": (ox, oy), "ppm": os.path.join(tmp, "aim.ppm")}


def w_anim(ui, geo, steps=ANIM_FLIPS):
    """Flip the Settings toggle and let each animation run to completion.

    The gap is deliberately longer than AUI_T_BASE (180 ms): overlapping the
    flips would measure one continuous animation rather than `steps` of them,
    and the STOP assertion below would have nothing to be true about."""
    x, y = geo["toggle"]
    got = ui.settle_pointer(geo["ppm"], x, y)
    if got != (x, y):
        print("     warning: pointer would not settle on the toggle (%r, wanted "
              "%r) -- this row is not a measurement" % (got, (x, y)))
    for _ in range(steps):
        ui.click(hold=0.05)
        time.sleep(0.55)


def anim_push_to_dock(ui, tmp, geo, aim_pt):
    """Drag the focused Settings window down until it overlaps the dock slab,
    and re-derive the toggle's screen position from the probe afterwards.

    THIS IS THE MEASUREMENT ONLY THE HARNESS CAN MAKE. The compositor grows any
    damage rectangle that touches a glass panel until it contains the WHOLE
    panel, because blur reads a neighbourhood and cannot be clipped
    (c/kernel/gui/wm.c, dmg_expand). So the identical widget, running the
    identical code, costs ~16 ms more PER FRAME when the user has left the
    window low on the screen -- roughly 1.8x for a Settings toggle. Nothing
    inside aui can see that, and no amount of reading the source will produce
    the number. Both placements are published; the delta is the finding."""
    t = focused_titlebar(ui, tmp)
    if t is None:
        return None
    tx, ty = t
    got = ui.settle_pointer(geo["ppm"], tx, ty)
    if got != (tx, ty):
        return None
    # HOW FAR DOWN, and the first version of this got it wrong in a way worth
    # keeping: it dragged a fixed 14 x 20 pt and the window went PAST the dock
    # and off the bottom of the screen. Composited pixels then FELL (493,879
    # against 768,405 clear of the dock) and the row read as "overlapping the
    # dock is cheaper", which is the opposite of the property being measured.
    # Aim instead: put the window's bottom edge on the dock's icon row, which is
    # inside the panel by construction and leaves the whole window on screen.
    _, row = dock_icon(0, len(ui.dock()))
    dy_total = row - (geo["origin"][1] + pt(SETTINGS_WINH_PT))
    ui._input([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.05)
    step = pt(6)
    moved = 0
    while moved < dy_total:
        d = min(step, dy_total - moved)
        ui._input([{"type": "rel", "data": {"axis": "y", "value": d}}])
        ui.cur[1] += d
        moved += d
        time.sleep(0.01)
    ui._input([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.6)
    got = settings_probe(ui, tmp, "probe2.ppm")
    if got is None or got[2] != 1:
        return None
    ox, oy, _ = got
    return {"toggle": (ox + pt(aim_pt[0]), oy + pt(aim_pt[1])),
            "origin": (ox, oy), "ppm": geo["ppm"]}


def w_idle(ui, geo, steps=0):
    """Do nothing for a second, well after the last animation must have ended.

    THIS IS THE STOP ASSERTION and it is the one that catches the failure this
    whole design is built to avoid: an animation that never marks itself
    arrived keeps registering deadlines forever, which is a poll loop with
    extra steps. It is invisible to the positive assertion -- more composites
    look like more animation -- and it is exactly what the machine deleted 3.3
    million syscalls to be rid of."""
    time.sleep(1.2)


WORKLOADS = [
    ("drag",   w_drag,   "a LARGE window (the 900pt Terminal) dragged by its titlebar"),
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
    do_assert, expect_off = False, None
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":    xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres":  yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--reps":  reps = int(argv[i + 1]); i += 2
        elif argv[i] == "--iso":   iso = argv[i + 1]; i += 2
        elif argv[i] == "--only":  only = argv[i + 1].split(","); i += 2
        elif argv[i] == "--json":  jpath = argv[i + 1]; i += 2
        elif argv[i] == "--assert":     do_assert = True; i += 1
        elif argv[i] == "--expect-off": expect_off = int(argv[i + 1]); i += 2
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
                        "a SMALLER window (the 640pt Finder) dragged by its titlebar",
                        {"title": t, "ppm": os.path.join(tmp, "aim.ppm")})

        # TextEdit, then the Terminal: two more windows, the Terminal focused.
        # Verified launches: the rows below measure repaints of "the TextEdit
        # window" and "the Terminal window", claims that are only true if those
        # are the windows that came up.
        ui.launch_app("textedit")
        time.sleep(6 * slow)
        ui.launch_app("terminal")
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
            "ppm": os.path.join(tmp, "aim.ppm"),
        }
        print("     focused titlebar at %r, toggle at %r" % (geo["title"], geo["toggle"]))

        for name, fn, what in WORKLOADS:
            if only and name not in only:
                continue
            measure(name, fn, what, geo)

        # PHASE 3: the toolkit's own motion. Last, because it opens another
        # window and drags it, which would change every row above it.
        if not only or "anim" in only:
            ageo = anim_setup(ui, tmp, serial)
            if ageo is not None:
                print("     anim toggle at %r (window-local %r)"
                      % (ageo["toggle"], ageo["aim"]))
                measure("anim", w_anim,
                        "6 flips of a Settings toggle, window CLEAR of the dock",
                        ageo)
                measure("anim-idle", w_idle,
                        "1.2 s of nothing, 650 ms after the last flip landed",
                        ageo)
                dgeo = anim_push_to_dock(ui, tmp, ageo, ageo["aim"])
                if dgeo is not None:
                    measure("anim-dock", w_anim,
                            "the same 6 flips, window OVERLAPPING the dock slab",
                            dgeo)
                    a, d = result.get("anim"), result.get("anim-dock")
                    if a and d and d["cpx"][0] <= a["cpx"][0]:
                        print("     anim-dock: composited px did NOT go up "
                              "(%.0f vs %.0f clear of the dock), so the window "
                              "is not overlapping the slab and this row is NOT "
                              "the glass penalty -- it is a differently-placed "
                              "window. Do not quote it."
                              % (d["cpx"][0], a["cpx"][0]))
                    elif a and d:
                        print("     anim-dock: +%.0f px and %+.2f ms per frame "
                              "over the same widget clear of the dock -- that "
                              "is dmg_expand growing every animated frame to "
                              "contain the whole dock panel."
                              % (d["cpx"][0] - a["cpx"][0],
                                 d["ms"][0] - a["ms"][0]))
                else:
                    print("     anim-dock: could not reposition the window over "
                          "the dock -- the glass penalty is UNMEASURED here, "
                          "not zero.")
    finally:
        qemu.kill()
        qemu.wait()

    if jpath:
        with open(jpath, "w") as fh:
            _json.dump({"xres": xres, "yres": yres, "scale": scale,
                        "iso": iso, "workloads": result}, fh, indent=1)
        print("     wrote %s" % jpath)
    if do_assert:
        return assert_anim(result, ANIM_FLIPS, expect_off)
    return 0


# ---------------------------------------------------------------------------
# THE GATE. Three assertions, and the second and third are what make this a
# control rather than a thermometer.

def assert_anim(result, flips, expect_off):
    """0 if the `anim` class says what it must, 1 otherwise."""
    if "anim" not in result:
        print("FAIL anim: the class never produced a measurement (see the "
              "reason printed above). A gate that cannot aim must say so, not "
              "report zero.")
        return 1
    comp = result["anim"]["composites"][0]
    idle = result.get("anim-idle", {}).get("composites", (99,))[0]
    ok = True

    # (1) POSITIVE. AUI_T_BASE is 180 ms and a 640x480 pt canvas at 150% is
    # ~691,200 device px at ~30 ns each, so the toolkit asks for a frame every
    # ~21 ms: about 9 per flip. Four is the floor at which motion is still
    # motion rather than a jump cut, and it is what the vocabulary is designed
    # around -- so it is what is asserted.
    lo = 4 * flips
    if comp < lo:
        print("FAIL anim positive: %d composites over %d flips, wanted >= %d. "
              "One per flip means the animation did nothing." % (comp, flips, lo))
        ok = False
    else:
        print("ok   anim positive: %d composites over %d flips (>= %d)"
              % (comp, flips, lo))

    # (2) NEGATIVE. Handed in by the caller from a -DAUI_ANIM_OFF run of this
    # same script. Without it the row above is not evidence: a compositor that
    # repainted several times after any click would satisfy it whether or not a
    # single animation ran.
    if expect_off is not None:
        # THE THRESHOLD HERE WAS WRONG IN ITS FIRST VERSION, and running the
        # control is what found that -- which is the entire argument for
        # running controls rather than reasoning about them. It asserted the
        # OFF build produce at most 2 composites per flip. The OFF build
        # produced FIVE, and correctly: a click is a press event AND a release
        # event, both of which change what aui would draw, so the interaction
        # repaints several times whether or not anything animates. An absolute
        # ceiling would have failed a correct harness and been "fixed" by
        # loosening it until it passed, which is how a control becomes decor.
        #
        # What the control actually has to establish is SEPARATION: that the
        # animated build asks for materially more frames than the same
        # interaction asks for on its own. Two conditions, because either alone
        # is gameable -- a ratio alone passes on tiny numbers, a delta alone
        # passes when the baseline is already huge.
        ratio_ok = comp >= expect_off * 3 // 2
        delta_ok = (comp - expect_off) >= 2 * flips
        if not ratio_ok or not delta_ok:
            print("FAIL anim CONTROL: animated %d composites vs %d with "
                  "-DAUI_ANIM_OFF (%.2fx, +%d over %d flips). Wanted >= 1.5x "
                  "AND at least 2 extra frames per flip. The two builds are "
                  "not distinguishable, so the positive row above is measuring "
                  "the interaction's own repaints, not the motion core."
                  % (comp, expect_off, float(comp) / max(1, expect_off),
                     comp - expect_off, flips))
            ok = False
        else:
            print("ok   anim control: OFF %d vs animated %d (%.2fx, +%.1f frames "
                  "per flip that exist only because aui_anim() scheduled them)"
                  % (expect_off, comp, float(comp) / max(1, expect_off),
                     (comp - expect_off) / float(flips)))
    else:
        print("     anim control: NOT RUN. Pass --expect-off N from a "
              "-DAUI_ANIM_OFF build; without it the positive row is a "
              "thermometer, not a control.")

    # (3) STOP. The one that catches the failure the whole design is built to
    # avoid, and the only place it is visible: a slot that never latches keeps
    # registering deadlines forever. MORE COMPOSITES LOOK LIKE MORE ANIMATION,
    # so assertion (1) would pass while the machine burned a core.
    #
    # THE FIRST VERSION OF THIS ASSERTION WAS A COUNT, AND IT WAS WRONG. An
    # idle desktop is not at zero composites: the menu-bar clock repaints its
    # 24-point strip twice a second forever, so 1.2 s of "nothing" legitimately
    # produced 4 frames and a threshold of 3 would have failed a correct build
    # -- and, far worse, a threshold of 6 would have PASSED a build whose
    # animation was still running, because six app frames and six clock frames
    # are the same number.
    #
    # The pixels tell them apart and the count cannot. A clock frame composites
    # 69,120 px (the menu bar, 1920x36 at this mode); an animated Settings frame
    # composites ~768,000. So the assertion is on WHAT WAS PAINTED: if anything
    # is still animating, idle frames are the size of the app's canvas.
    idle_px = result.get("anim-idle", {}).get("cpx", (0,))[0]
    anim_px = result["anim"]["cpx"][0]
    if anim_px > 0 and idle_px > anim_px / 4.0:
        print("FAIL anim STOP: idle frames composite %.0f px, a quarter or more "
              "of the %.0f px an ANIMATED frame costs. Something is still asking "
              "for app repaints 650 ms after the last flip landed -- that is a "
              "poll loop with extra steps." % (idle_px, anim_px))
        ok = False
    else:
        print("ok   anim stop: %d idle composites of %.0f px each (an animated "
              "frame is %.0f px; the menu-bar clock alone is 69,120)"
              % (idle, idle_px, anim_px))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
