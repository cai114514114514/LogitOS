#!/usr/bin/env python3
"""Window snapping and the close animation ("Fluid" P3), on the machine.

CLOSE. The desktop is photographed, TextEdit is launched and closed with
Cmd+W, and once the fade is over the desktop is photographed again: the two
must be pixel-identical below the menu bar (whose clock ticks). A close
animation that under-reports its last frame leaves a ghost's residue on the
wallpaper, and nothing repaints it -- the round trip is what sees that. The
guest must also report at least three distinct fade positions and the end.

SNAP. TextEdit's titlebar is dragged to the left edge, then the right edge,
then into the menu bar; each time the guest's `[wm] snap` line must give
exactly the frame the zone means (a half, or the zoom). Then a half-snapped
window is dragged back into the middle, and `[wm] unsnap` must give back the
size it had before its first snap.

PLACE. The five apps a desktop tour found opening outside the work area --
TextEdit and Code Studio off the right edge, Settings, Terminal and the Browser
with their bottom rows under the dock -- are opened from the dock one at a
time. For each, the guest's own numbers decide: the `[wm] place` line names the
window and the work area (below the menu bar, above the dock), and the LAST
`[wm] win N frame` line for that window -- printed once its geometry has been
quiet for 150 ms, so after any SYS_GUI_WIN_MIN growth -- must lie inside it.
The frame line is the referee rather than the place line because it is what
the compositor ends up drawing, whatever path moved the window last.

  --only close|snap|place   run one scene
  --negctl            a control kernel: WM_CLOSE_DAMAGE_LIE (with --only close),
                      WM_SNAP_NEGCTL (with --only snap) or
                      WM_PLACE_NEGCTL_NO_FIT (with --only place); the scene must fail

Usage: qmp_snapclose.py --iso ISO [--disk DISK] [--only close|snap|place] [--negctl]
"""
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, configure                      # noqa: E402
from qmp_repaint import boot, CLOSE_RGB                         # noqa: E402

XRES, YRES = 1280, 800            # scale 100%: device px == points


def titlebar_grab(ui, tmp):
    """A point on the focused window's titlebar, right of its traffic lights,
    found from the red close light only the focused window paints."""
    shot = os.path.join(tmp, "tb.ppm")
    ui.screendump(shot)
    box = PPM(shot).find_color(CLOSE_RGB)
    if not box:
        return None
    x0, y0, x1, y1 = box
    return x1 + 120, (y0 + y1) // 2


def drag(ui, tmp, frm, to):
    probe = os.path.join(tmp, "probe.ppm")
    got = ui.settle_pointer(probe, frm[0], frm[1])
    if got != tuple(frm):
        print("     warning: pointer settled at %r, wanted %r" % (got, frm))
    ui._input([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.15)
    ui.goto(to[0], to[1], settle=0.6)
    ui._input([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.8)


def diff_below(a, b, top):
    n, box = 0, None
    for y in range(top, a.h):
        o = y * a.w * 3
        if a.px[o:o + a.w * 3] != b.px[o:o + a.w * 3]:
            for x in range(a.w):
                if a.at(x, y) != b.at(x, y):
                    n += 1
                    box = (x, y, x, y) if box is None else \
                          (min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y))
    return n, box


def park(ui, tmp):
    """Rest the pointer on bare wallpaper at the right edge. Both photographs
    of the round trip must be taken with the pointer in the same place: a
    pointer left on the dock after a launch magnifies an icon and shows its
    name, which is a difference the close animation did not make."""
    ui.settle_pointer(os.path.join(tmp, "park.ppm"), XRES - 15, YRES // 2)
    time.sleep(0.8)


def main(argv):
    iso = argv[argv.index("--iso") + 1]
    disk = argv[argv.index("--disk") + 1] if "--disk" in argv else None
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    configure(XRES, YRES)
    tmp = tempfile.mkdtemp(prefix="logit-snapclose-")
    qemu, sock, serial = boot(iso, XRES, YRES, tmp, disk)
    fails = []

    def ck(cond, what):
        print("  %s  %s" % ("ok  " if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    try:
        time.sleep(6)
        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        log0 = ui.serial_text()
        mbh = 40                                    # above this: the menu bar + clock

        if only in (None, "close"):
            print("close: launch TextEdit, Cmd+W, and the desktop must come back exactly")
            time.sleep(2.0)
            park(ui, tmp)
            a = os.path.join(tmp, "a.ppm")
            ui.screendump(a)
            ui.launch_app("textedit", probe=probe)
            time.sleep(2.5)
            mark = len(ui.serial_text())
            ui.key_mods(("meta_l",), "w", settle=2.0)
            park(ui, tmp)
            b = os.path.join(tmp, "b.ppm")
            ui.screendump(b)
            log = ui.serial_text()[mark:]
            ps = set(int(m) for m in re.findall(r"\[wm\] anim close ghost \d+ p (\d+)", log))
            ck(len(ps) >= 3, "the close fade drew %d distinct positions (>= 3): %s" % (len(ps), sorted(ps)))
            ck("[wm] anim close ghost" in log and " done" in log, "the fade reached its end")
            n, box = diff_below(PPM(a), PPM(b), mbh)
            ck(n == 0, "the desktop is pixel-identical after the close (%d px differ below the menu bar%s)"
               % (n, ", in %r" % (box,) if box else ""))

        if only in (None, "snap"):
            print("snap: left edge, right edge, menu bar, and back")
            ui.launch_app("textedit", probe=probe)
            time.sleep(2.5)
            half = XRES // 2
            g = titlebar_grab(ui, tmp)
            ck(g is not None, "found TextEdit's focused titlebar")
            if g:
                mark = len(ui.serial_text())
                drag(ui, tmp, g, (-60, g[1]))
                log = ui.serial_text()[mark:]
                m = re.search(r"\[wm\] snap win=(\d+) zone=left frame (-?\d+) (\d+) (\d+) (\d+) was (\d+) (\d+)", log)
                ck(m is not None, "dragging to the left edge snapped it (zone=left)")
                orig = None
                if m:
                    x, y, w, h = map(int, m.group(2, 3, 4, 5))
                    orig = (int(m.group(6)), int(m.group(7)))
                    ck(x == 0 and w == half and y >= 20, "...into the left half: frame %d %d %d %d" % (x, y, w, h))
                g = titlebar_grab(ui, tmp)
                if g:
                    mark = len(ui.serial_text())
                    drag(ui, tmp, g, (XRES + 60, g[1]))
                    log = ui.serial_text()[mark:]
                    m = re.search(r"\[wm\] snap win=\d+ zone=right frame (-?\d+) (\d+) (\d+) (\d+)", log)
                    ck(m is not None and int(m.group(1)) == XRES - half and int(m.group(3)) == half,
                       "dragging to the right edge gives the right half (%s)" % (m.group(0) if m else None))
                    ck("[wm] unsnap" in log and orig is not None and
                       ("size %d %d" % orig) in log,
                       "leaving the left half restored the size it had (%r)" % (orig,))
                g = titlebar_grab(ui, tmp)
                if g:
                    mark = len(ui.serial_text())
                    drag(ui, tmp, g, (g[0], 2))
                    log = ui.serial_text()[mark:]
                    m = re.search(r"\[wm\] snap win=\d+ zone=top frame (-?\d+) (\d+) (\d+) (\d+)", log)
                    ck(m is not None and int(m.group(1)) == 0 and int(m.group(3)) == XRES,
                       "dragging into the menu bar zooms it (%s)" % (m.group(0) if m else None))

        if only in (None, "place"):
            print("place: every window the WM opens lies inside the work area")
            place_re = re.compile(r"\[wm\] place win=(\d+) frame -?\d+ -?\d+ \d+ \d+ "
                                  r"work (\d+) (\d+) (\d+) (\d+)")
            for name in ("textedit", "studio", "settings", "terminal", "browser"):
                mark = len(ui.serial_text())
                ui.launch_app(name, probe=probe, timeout=120.0)
                time.sleep(5.0)              # SYS_GUI_WIN_MIN growth, then 150 ms of quiet
                log = ui.serial_text()[mark:]
                places = place_re.findall(log)
                ck(bool(places), "%s: the WM reported placing its window" % name)
                if places:
                    win = places[-1][0]
                    ax, ay, aw, ah = (int(v) for v in places[-1][1:])
                    frames = re.findall(r"\[wm\] win %s frame (-?\d+) (-?\d+) (\d+) (\d+) " % win, log)
                    ck(bool(frames), "%s: window %s reported a settled frame" % (name, win))
                    if frames:
                        x, y, w, h = (int(v) for v in frames[-1])
                        ck(x >= ax and y >= ay and x + w <= ax + aw and y + h <= ay + ah,
                           "%s: frame %d %d %d %d lies inside the work area %d %d %d %d"
                           % (name, x, y, w, h, ax, ay, aw, ah))
                ui.key_mods(("meta_l",), "w", settle=2.0)
        _ = log0
    finally:
        qemu.kill()
        qemu.wait()

    if fails:
        print("FAIL (%d): %s" % (len(fails), "; ".join(fails)))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
