#!/usr/bin/env python3
"""Does partial compositing draw the same desktop a full one would?

The compositor now recomposites only the rectangles something reported as
damaged. That is a performance change on the surface and a CORRECTNESS change
underneath, because `dirty_rect` used to throw all four of its arguments away:
no caller in wm.c was ever held to reporting the true extent of what it changed,
and now every one of them is. A caller that under-reports leaves pixels on
screen that no longer belong there. Nothing repaints them -- there is no
periodic full repaint left to cover for it -- so they stay.

That failure mode killed the previous attempt at this, so it gets three
independent instruments here rather than one:

  1. THE PRIMITIVES, host-side (tests/unit/fb_clip_test.c). Drawing with a clip
     must produce exactly the pixels drawing without one would have produced
     inside it, and must not touch a pixel outside it. Sixteen primitives, one
     property, no emulator.

  2. PARTIAL == FULL, on the machine. Perform an interaction, photograph the
     result, then force whole-screen recomposites of the IDENTICAL state and
     photograph again. Every pixel must match. This does not need the
     interaction to be reversible and it does not care what the damage
     rectangles were: it compares what the partial path drew against what a full
     composite of the same desktop draws.

  3. ROUND TRIP, on the machine. Photograph, interact, return to exactly the
     starting state, photograph. Zero differing pixels. This is the check the
     cursor work used and it catches the thing (2) cannot: damage that was
     missed for a state the machine has since left.

Plus the two controls, because an assertion that never fires proves nothing:

  * MEASUREMENT CONTROL. The dark-mode switch must still produce FULL-SCREEN
    frames -- a repaint of everything is the right answer there -- while a
    window drag must produce none. If damage tracking never fell back to a full
    frame it would be broken in the opposite direction, and a test that only
    ever asserts "smaller" cannot see that.

  * NEGATIVE CONTROL (--negative). Builds a kernel that deliberately reports
    only the top-left quarter of a window as damaged (WM_DAMAGE_LIE in wm.c),
    in a throwaway copy of the tree, and requires the checks above to FAIL. "No
    stale pixels" is otherwise a claim about a test that has never failed.

Every timing here is TCG. This test asserts pixels, not milliseconds.

Usage:
    tests/qmp/qmp_damage.py [--xres W] [--yres H] [--iso PATH]
                            [--skip-unit] [--negative]
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, configure, dock_icon, pt   # noqa: E402
from qmp_repaint import CLOSE_RGB, boot, perf_samples       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TEXTEDIT_SLOT = 1
TERMINAL_SLOT = 3


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


def run_unit_test():
    """The host-side primitive test. No make target: it is one cc line."""
    out = os.path.join(tempfile.mkdtemp(prefix="logit-fbclip-"), "fb_clip_test")
    cc = subprocess.run(
        ["cc", "-O1", "-g", "-Wall", "-Wextra", "-o", out,
         os.path.join(ROOT, "tests/unit/fb_clip_test.c"),
         os.path.join(ROOT, "c/kernel/gui/fb.c"),
         "-I" + os.path.join(ROOT, "c/kernel/gui"),
         "-I" + os.path.join(ROOT, "c/drivers/virtio"),
         "-I" + os.path.join(ROOT, "c/kernel/mm"),
         "-I" + os.path.join(ROOT, "c/lib/text")],
        capture_output=True, text=True)
    if cc.returncode != 0:
        print(cc.stderr[-2000:])
        return False
    r = subprocess.run([out], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        print("     " + line)
    return r.returncode == 0


def build_negative():
    """A kernel that under-reports window damage, built in a throwaway copy.

    The lie is one line in wm.c, flipped by substitution rather than by a patch
    file, so it cannot silently stop applying when the surrounding code moves."""
    tmp = tempfile.mkdtemp(prefix="logit-negctl-")
    dst = os.path.join(tmp, "tree")
    print("     copying the tree to %s ..." % dst)
    shutil.copytree(ROOT, dst, ignore=shutil.ignore_patterns(
        ".git", "build", "__pycache__", "*.pyc", "rust"))
    # rust/ is excluded from the copy but the kernel links its staticlib; bring
    # the prebuilt artifact across rather than rebuilding the world.
    src_rust = os.path.join(ROOT, "rust")
    if os.path.isdir(src_rust):
        shutil.copytree(src_rust, os.path.join(dst, "rust"))
    wm = os.path.join(dst, "c/kernel/gui/wm.c")
    text = open(wm).read()
    needle = "#define WM_DAMAGE_LIE 0"
    if needle not in text:
        print("FAIL wm.c no longer has %r -- the negative control cannot be built"
              % needle)
        return None
    open(wm, "w").write(text.replace(needle, "#define WM_DAMAGE_LIE 1"))
    print("     building the lying kernel ...")
    r = subprocess.run(["make", "-j", str(os.cpu_count() or 4)],
                       cwd=dst, capture_output=True, text=True)
    iso = os.path.join(dst, "build", "logit.iso")
    if r.returncode != 0 or not os.path.exists(iso):
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        return None
    return iso


def main(argv):
    xres, yres = 1280, 800
    iso, skip_unit, negative = None, False, False
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":       xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres":     yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--iso":      iso = argv[i + 1]; i += 2
        elif argv[i] == "--skip-unit": skip_unit = True; i += 1
        elif argv[i] == "--negative": negative = True; i += 1
        else:
            print("unknown arg %r" % argv[i]); return 2

    fails, notes = [], []

    def ck(cond, what):
        print("%-4s %s" % ("ok" if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    if not skip_unit:
        print("=== 1. the primitives, host-side ===")
        ck(run_unit_test(), "every fb primitive draws the same pixels clipped as "
                            "unclipped, and none outside the clip")

    if negative:
        print("\n=== negative control: a kernel that under-reports damage ===")
        iso = build_negative()
        if iso is None:
            print("\nqmp_damage: could not build the negative control")
            return 1
    if iso is None:
        iso = os.path.join(ROOT, "build", "logit.iso")

    scale = configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-damage-")
    print("\n=== %dx%d device px (scale %d%%), %s ==="
          % (xres, yres, scale, iso))

    # The menu-bar clock ticks, and two frames seconds apart are entitled to
    # differ there. Nothing below it is.
    SKIP = pt(24) + 2
    pixel_checks = []          # (name, ok) -- the ones the negative control inverts

    qemu, sock, serial = boot(iso, xres, yres, tmp)
    try:
        time.sleep(5 * slow)
        ui = Session(sock, serial=serial)

        ui.click_at(*dock_icon(TEXTEDIT_SLOT))
        time.sleep(6 * slow)
        ui.click_at(*dock_icon(TERMINAL_SLOT))
        time.sleep(12 * slow)

        p = os.path.join(tmp, "p.ppm")
        REST = (pt(30), pt(120))               # wallpaper, far from everything

        ui.screendump(os.path.join(tmp, "t.ppm"), settle=0.8 * slow)
        box = PPM(os.path.join(tmp, "t.ppm")).find_color(CLOSE_RGB)
        if box is None:
            print("FAIL no focused window on screen")
            return 1
        title = (box[2] + pt(120), (box[1] + box[3]) // 2)
        print("     focused titlebar at %r" % (title,))

        def shot(tag, settle=1.2):
            path = os.path.join(tmp, tag + ".ppm")
            ui.screendump(path, settle=settle * slow)
            return PPM(path)

        def toggle_theme(times=2):
            """Force whole-screen recomposites of the CURRENT state.

            The dark-mode switch is the one interaction that legitimately
            repaints everything (wm_set_dark -> dirty_full), so an even number
            of them leaves the desktop exactly as it was, drawn by the full
            path."""
            tx = xres - pt(210) + pt(19)
            ty = (pt(24) - pt(18)) // 2 + pt(9)
            ui.goto(tx, ty, settle=0.2)
            for _ in range(times):
                ui.click(hold=0.1)
                time.sleep(1.2 * slow)
            ui.settle_pointer(p, *REST)
            time.sleep(1.0 * slow)

        def partial_equals_full(name, interact):
            interact()
            ui.settle_pointer(p, *REST)
            a = shot(name + "-partial")
            toggle_theme()
            b = shot(name + "-full")
            n, bb = diff_pixels(a, b, SKIP)
            ok = (n == 0)
            print("     %-18s partial vs full: %d differing px%s"
                  % (name, n, "" if bb is None else "  bbox %r" % (bb,)))
            ck(ok, "%s: the partially-composited desktop is pixel-identical to a "
                   "full composite of the same state" % name)
            pixel_checks.append((name, ok))

        # ---- the interactions ------------------------------------------
        def i_drag():
            """Grab the titlebar, walk the window around, put it back exactly.

            Confirmed against the guest's own pointer report at both ends: dead
            reckoning off `rel` deltas loses samples, and a window left two
            pixels from where it started is a differing pixel that is the
            harness's fault, not the compositor's."""
            got = ui.settle_pointer(p, *title)
            if got != tuple(title):
                notes.append("drag: pointer would not settle on the titlebar (%r)" % (got,))
            ui._input([{"type": "btn", "data": {"button": "left", "down": True}}])
            time.sleep(0.1)
            for (dx, dy) in [(pt(160), pt(90)), (-pt(90), pt(40)), (-pt(70), -pt(130))]:
                ui.goto(ui.cur[0] + dx, ui.cur[1] + dy, settle=0.25)
            ui.settle_pointer(p, *title)        # back to the exact grab point
            ui._input([{"type": "btn", "data": {"button": "left", "down": False}}])
            time.sleep(0.6 * slow)

        def i_dock():
            for k in (0, 4, 8, 3, 6, 0):
                ui.goto(*dock_icon(k), settle=0.12)
            time.sleep(0.4 * slow)

        def i_scroll():
            ui.goto(title[0], title[1] + pt(150), settle=0.2)
            for btn, n in (("wheel-up", 6), ("wheel-down", 12)):
                for _ in range(n):
                    ui._input([{"type": "btn", "data": {"button": btn, "down": True}},
                               {"type": "btn", "data": {"button": btn, "down": False}}])
                    time.sleep(0.03)
            time.sleep(0.5 * slow)

        def i_type():
            for ch in "hello":
                ui.key(ch, settle=0.06)
            for _ in range(5):
                ui.key("backspace", settle=0.06)
            time.sleep(0.6 * slow)

        print("\n=== 2. partial composite == full composite ===")
        for name, fn in (("drag", i_drag), ("dock-hover", i_dock),
                         ("scroll", i_scroll), ("keystrokes", i_type)):
            partial_equals_full(name, fn)

        print("\n=== 3. round trip: back to the starting state, no stale pixels ===")
        ui.settle_pointer(p, *REST)
        a = shot("rt0")
        i_drag()
        i_dock()
        i_scroll()
        ui.settle_pointer(p, *REST)
        b = shot("rt1")
        n, bb = diff_pixels(a, b, SKIP)
        print("     round trip: %d differing px%s"
              % (n, "" if bb is None else "  bbox %r" % (bb,)))
        ok = (n == 0)
        ck(ok, "a drag out and back, a dock sweep and a scroll round trip leave "
               "the desktop pixel-identical")
        pixel_checks.append(("round-trip", ok))

        # ---- 4. the measurement control --------------------------------
        print("\n=== 4. the measurement control ===")
        log = open(serial, errors="replace").read()
        s = perf_samples(log)
        ck(len(s) >= 2, "the compositor reported its counters at all")
        if s:
            last = s[-1]
            has = "cpx" in last and "full" in last
            ck(has, "the kernel reports damage counters (cpx/fpx/full/rects)")
            if has:
                # A theme flip is a full-screen repaint and MUST show as one.
                before = perf_samples(log)[-1]
                toggle_theme(2)
                after = perf_samples(open(serial, errors="replace").read())[-1]
                dfull = after["full"] - before["full"]
                ck(dfull >= 2, "the dark-mode switch still produces FULL-SCREEN "
                               "frames (%d of them for 2 clicks) -- damage "
                               "tracking falls back when a full repaint is right"
                               % dfull)

                # A drag must produce none, and must composite well under a screen.
                before = perf_samples(open(serial, errors="replace").read())[-1]
                i_drag()
                time.sleep(1.0)
                for v in (1, -1):
                    ui._input([{"type": "rel", "data": {"axis": "x", "value": v}}])
                time.sleep(1.5)
                after = perf_samples(open(serial, errors="replace").read())[-1]
                dfull = after["full"] - before["full"]
                dc = after["composites"] - before["composites"]
                dpx = after["cpx"] - before["cpx"]
                frac = (dpx / float(dc) / (xres * yres)) if dc else 1.0
                print("     drag: %d composites, %d full-screen, %.1f%% of the "
                      "screen per frame" % (dc, dfull, 100.0 * frac))
                ck(dc > 5, "the drag really composited (%d frames)" % dc)
                ck(dfull == 0, "a window drag produces NO full-screen frames")
                ck(frac < 0.90, "a window drag composites less than the whole "
                                "screen (%.1f%%)" % (100.0 * frac))

        # ---- 5. idle ----------------------------------------------------
        print("\n=== 5. idle ===")
        before = perf_samples(open(serial, errors="replace").read())[-1]
        time.sleep(6.0)
        for v in (1, -1):
            ui._input([{"type": "rel", "data": {"axis": "x", "value": v}}])
        time.sleep(1.5)
        after = perf_samples(open(serial, errors="replace").read())[-1]
        dc = after["composites"] - before["composites"]
        dpx = after["cpx"] - before["cpx"]
        if dc:
            frac = dpx / float(dc) / (xres * yres)
            print("     idle: %d composites over ~7.5 s, %.1f%% of the screen "
                  "per frame" % (dc, 100.0 * frac))
            ck(frac < 0.35, "an idle desktop repaints a strip, not a screen "
                            "(%.1f%% per frame)" % (100.0 * frac))
    finally:
        qemu.kill()
        qemu.wait()

    for n in notes:
        print("note: " + n)

    if negative:
        broke = [n for (n, ok) in pixel_checks if not ok]
        print("\n=== negative control verdict ===")
        print("     pixel checks that FAILED under the lying kernel: %s"
              % (", ".join(broke) if broke else "none"))
        if broke:
            print("\nqmp_damage: NEGATIVE CONTROL OK -- under-reported damage is "
                  "detected (%d of %d pixel checks failed)"
                  % (len(broke), len(pixel_checks)))
            return 0
        print("\nqmp_damage: NEGATIVE CONTROL FAILED -- a kernel that reports a "
              "quarter of each window as damaged passed every pixel check, so "
              "the pixel checks prove nothing")
        return 1

    if fails:
        print("\nqmp_damage: %d FAILED" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("\nqmp_damage: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
