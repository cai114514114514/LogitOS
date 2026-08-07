#!/usr/bin/env python3
"""Boot LogitOS at a given display mode and prove the desktop SCALED rather than
shrank -- with a picture, and with a click.

Three things get asserted, and the third is the one that matters:

  1. The guest reports the backing scale this mode should produce, and the
     screendump really is that many pixels. (Cheap; catches a mis-plumbed mode.)

  2. A control the app drew at a known LOGICAL size measures scale x that size on
     screen. This is the difference between "scaled" and "the same picture in a
     bigger window": at 1x the Error swatch is 18 px across, at 1.5x it is 27.

  3. A click computed from that same scale toggles the checkbox next to it. This
     is the assertion that a screenshot cannot make. A scaled UI with unscaled
     hit-testing looks perfect and is unusable, and it is exactly what you get if
     the compositor multiplies coordinates on the way down and forgets to divide
     them on the way back up. Run with --control to click where an UNSCALED build
     would have put the checkbox and require nothing to happen -- that is the
     negative control, and it fails against a kernel without this change.

Everything is located from the picture rather than from a table of coordinates:
the Error swatch is an exact, unique colour, so the window's content origin is
derived from where it actually landed. A test that hardcodes where it thinks a
window is tests the hardcoding.

Usage:
    tests/qmp/qmp_scale.py [--xres W] [--yres H] [--out build/scale-WxH.png]
                           [--control] [--keep]
"""

import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import (BROWSER_SLOT, Session, PPM, configure, dock_icon,   # noqa: E402
                    locate_cursor, pick_scale, pt)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

WIDGETS_SLOT = 4          # scan_apps order: clock textedit monitor terminal widgets ...

# Geometry the Widgets app draws, in POINTS -- read straight off c/apps/gui/widgets.c
# and c/apps/gui/aui.c. These are the app's own numbers; the whole point is that
# they did not change and do not have to.
SWATCH_ERR_PT = (252, 250, 18, 18)     # hstack(AUI_PAD=16, 250, gap=28), 3rd item
SWATCH_ERR_RGB = (255, 69, 58)         # AUI_ERROR, light theme
CHECKBOX_PT = (20, 116, 16, 16)        # aui_checkbox("Fancy mode")
ACCENT_RGB = (64, 130, 246)            # AUI_ACCENT: the CHECKED checkbox fill


def count_color(ppm, box, rgb):
    """Exact-match pixels of `rgb` inside `box` -- local, so another window's
    palette cannot answer for this one."""
    x0, y0, x1, y1 = box
    n = 0
    for y in range(max(0, y0), min(ppm.h, y1 + 1)):
        base = y * ppm.w * 3
        for x in range(max(0, x0), min(ppm.w, x1 + 1)):
            o = base + x * 3
            if (ppm.px[o], ppm.px[o + 1], ppm.px[o + 2]) == rgb:
                n += 1
    return n


def changed_bbox(a, b):
    """Bounding box of every pixel that differs between two frames, or None.

    Used to ask "did a window that big appear?" without knowing where the window
    manager decided to put it -- which is a cascade counter, i.e. exactly the
    kind of thing a test should not be asserting."""
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    row = a.w * 3
    for y in range(min(a.h, b.h)):
        ra = a.px[y * row:(y + 1) * row]
        rb = b.px[y * row:(y + 1) * row]
        if ra == rb:
            continue
        for x in range(a.w):
            if ra[x * 3:x * 3 + 3] != rb[x * 3:x * 3 + 3]:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return None if x1 < 0 else (x0, y0, x1, y1)


def ppm_to_png(ppm_path, png_path):
    import struct, zlib
    d = open(ppm_path, "rb").read()
    i = d.index(b"255\n") + 4
    w, h = (int(x) for x in d[:i].split()[1:3])
    px = d[i:]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    open(png_path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b""))
    return w, h


def main(argv):
    xres, yres = 1920, 1200
    out = None
    control = False
    tour = False
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":   xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres": yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--out":  out = argv[i + 1]; i += 2
        elif argv[i] == "--control": control = True; i += 1
        elif argv[i] == "--tour": tour = True; i += 1
        else: print("unknown arg %r" % argv[i]); return 2
    if out is None:
        out = os.path.join(ROOT, "build", "scale-%dx%d.png" % (xres, yres))

    scale = configure(xres, yres)
    # Everything the compositor does is per-pixel and QEMU is emulating it, so a
    # 2x display is genuinely ~4x the work per frame. Waits scale with the pixel
    # count rather than being one number tuned at one resolution: a sleep that is
    # generous at 1280x800 is a flake at 2560x1600, and the failure it produces
    # ("the click did nothing") reads as a hit-testing bug.
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-scale-")
    sock, serial = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "serial.log")
    fails = []

    def ck(cond, what):
        print("%-4s %s" % ("ok" if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    print("=== %dx%d device px, expecting scale %d%% (desktop %dx%d pt) ==="
          % (xres, yres, scale, xres * 100 // scale, yres * 100 // scale))

    qemu = subprocess.Popen(
        ["qemu-system-x86_64",
         "-cdrom", os.path.join(ROOT, "build", "logit.iso"),
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
        deadline = time.time() + 120
        while time.time() < deadline:
            if os.path.exists(serial) and "desktop live" in open(serial, errors="replace").read():
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early"); return 1
            time.sleep(0.2)
        time.sleep(3 * slow)                # let the first frames land

        log = open(serial, errors="replace").read()

        # 1. What the guest itself thinks it is drawing onto.
        line = [l for l in log.splitlines() if l.startswith("[wm] display")]
        ck(bool(line), "kernel logged its display geometry")
        if line:
            print("     %s" % line[0].strip())
            ck("scale %d%%" % scale in line[0],
               "guest scale is %d%% (host model agrees)" % scale)
            ck("%dx%d px" % (xres, yres) in line[0],
               "guest framebuffer is %dx%d device px" % (xres, yres))

        ui = Session(sock)
        raw = os.path.join(tmp, "a.ppm")
        ui.screendump(raw, settle=1.0 * slow)
        w, h = ppm_to_png(raw, out)
        ck((w, h) == (xres, yres),
           "screendump is %dx%d (got %dx%d)" % (xres, yres, w, h))
        print("     wrote %s (%dx%d)" % (out, w, h))

        # The pointer is confirmed against the picture before every click, so a
        # miss is reported as a harness failure rather than misread as the guest
        # ignoring the click. (It also proves the cursor itself is being drawn at
        # the scaled size: the arrow is composited, so we can see it.)
        probe = os.path.join(tmp, "probe.ppm")

        # 2. Open Widgets and measure something it drew at a known logical size.
        ui.click_at_confirmed(probe, *dock_icon(WIDGETS_SLOT))
        time.sleep(6 * slow)
        ui.goto(xres // 2, yres - pt(200))        # pointer off the dock + off the window
        before_p = os.path.join(tmp, "b.ppm")
        ui.screendump(before_p, settle=1.0 * slow)
        ppm_to_png(before_p, out.replace(".png", "-widgets.png"))
        before = PPM(before_p)

        box = before.find_color(SWATCH_ERR_RGB)
        ck(box is not None, "Widgets window is open (found its AUI_ERROR swatch)")
        if box is None:
            # The guest's own account of the launch, which distinguishes "the
            # click missed the dock" from "the app started and did not paint".
            print("--- guest serial (tail) ---")
            for l in open(serial, errors="replace").read().splitlines()[-20:]:
                print("     " + l)
            return 1
        sx0, sy0, sx1, sy1 = box
        sw, sh = sx1 - sx0 + 1, sy1 - sy0 + 1
        px, py, pw, ph = SWATCH_ERR_PT
        want_w, want_h = pt(px + pw) - pt(px), pt(py + ph) - pt(py)
        print("     swatch at device (%d,%d) %dx%d; %d pt at %d%% -> %d px"
              % (sx0, sy0, sw, sh, pw, scale, want_w))
        ck(abs(sw - want_w) <= 1 and abs(sh - want_h) <= 1,
           "an 18-point swatch measures %d device px (scaled, not stretched)" % want_w)

        # The window's content origin, derived from where the swatch really is.
        ox, oy = sx0 - pt(px), sy0 - pt(py)

        # 3. Hit-test. Aim at the checkbox centre; --control aims where an
        #    unscaled build would have put it.
        cx_pt = CHECKBOX_PT[0] + CHECKBOX_PT[2] // 2
        cy_pt = CHECKBOX_PT[1] + CHECKBOX_PT[3] // 2
        if control:
            tx, ty = ox + cx_pt, oy + cy_pt          # points read as device px
            print("     CONTROL: clicking the UNSCALED offset (+%d,+%d)" % (cx_pt, cy_pt))
        else:
            tx, ty = ox + pt(cx_pt), oy + pt(cy_pt)
        cbox = (ox + pt(CHECKBOX_PT[0]), oy + pt(CHECKBOX_PT[1]),
                ox + pt(CHECKBOX_PT[0] + CHECKBOX_PT[2]) - 1,
                oy + pt(CHECKBOX_PT[1] + CHECKBOX_PT[3]) - 1)
        n_before = count_color(before, cbox, ACCENT_RGB)
        ck(n_before == 0, "checkbox starts unchecked (0 accent px in its rect)")

        ui.click_at_confirmed(probe, tx, ty)
        time.sleep(3 * slow)
        ui.goto(xres // 2, yres - pt(200))
        after_p = os.path.join(tmp, "c.ppm")
        ui.screendump(after_p, settle=1.0 * slow)
        ppm_to_png(after_p, out.replace(".png", "-clicked.png"))
        after = PPM(after_p)
        n_after = count_color(after, cbox, ACCENT_RGB)
        area = (cbox[2] - cbox[0] + 1) * (cbox[3] - cbox[1] + 1)
        print("     clicked device (%d,%d); accent px in the checkbox rect: %d/%d"
              % (tx, ty, n_after, area))

        if control:
            ck(n_after == 0,
               "NEGATIVE CONTROL: an unscaled click does NOT reach the checkbox")
        else:
            ck(n_after > area * 3 // 4,
               "a click computed at %d%% toggles the checkbox it was aimed at" % scale)

            # 4. The BROWSER still gets a window. It is the largest window in the
            #    system (1180x620 points, hardcoded in c/apps/browser/browser.c,
            #    which this work deliberately did not touch) and therefore the
            #    one that SYS_GUI_CREATE would refuse first if points were being
            #    scaled past the framebuffer. Nothing else in the tree would
            #    notice; the browser would simply come up with no window.
            ui.click_at_confirmed(probe, *dock_icon(BROWSER_SLOT))
            time.sleep(12 * slow)
            ui.goto(pt(30), yres // 2)          # out of the way, over the wallpaper
            br_p = os.path.join(tmp, "d.ppm")
            ui.screendump(br_p, settle=1.0 * slow)
            ppm_to_png(br_p, out.replace(".png", "-browser.png"))
            grew = changed_bbox(PPM(after_p), PPM(br_p))
            want_w, want_h = pt(1180), pt(30) + pt(620)     # TITLEBAR_H is 30 pt
            if grew is None:
                ck(False, "Browser opened a window at %d%%" % scale)
            else:
                gw, gh = grew[2] - grew[0] + 1, grew[3] - grew[1] + 1
                print("     new content bbox %dx%d; a 1180x620 pt window is %dx%d px"
                      % (gw, gh, want_w, want_h))
                ck(gw >= want_w * 9 // 10 and gh >= want_h * 9 // 10,
                   "Browser's 1180x620-point window fits and paints at %d%%" % scale)
            ck("launch: " not in open(serial, errors="replace").read(),
               "no window/launch failures on the guest's serial log")

            if tour:
                # Not an assertion -- a picture. The Terminal is the app whose
                # scaling is easiest to get half-right (its monospace CELL and
                # its glyph size are two different numbers, and scaling only the
                # cell spaces the same small glyphs further apart), so it is
                # worth looking at rather than only measuring.
                for slot in (3, 6):        # terminal, preview
                    ui.click_at_confirmed(probe, *dock_icon(slot))
                    time.sleep(8 * slow)
                ui.goto(pt(30), yres // 2)
                ui.screendump(probe, settle=1.0 * slow)
                ppm_to_png(probe, out.replace(".png", "-tour.png"))
                print("     wrote %s" % out.replace(".png", "-tour.png"))
    finally:
        qemu.kill()
        qemu.wait()

    if fails:
        print("\nqmp_scale: %d FAILED" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("\nqmp_scale: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
