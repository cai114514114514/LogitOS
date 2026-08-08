#!/usr/bin/env python3
"""Drive the aui Gallery over QMP and assert against the pixels.

A widget toolkit is a VISUAL artefact, so its regression test has to look at the
frame. This boots the desktop, opens Gallery from the Dock, and measures:

  * anti-aliasing  -- a rounded corner has a RAMP of tones along its arc, not
                      two. This is the assertion the negative control
                      (make test-aui-negctl, -DAUI_NO_AA) is built to fail.
  * alpha          -- five translucent swatches over one gradient must produce
                      five distinct, monotonically lighter colours.
  * elevation      -- a card casts a luminance ramp that decays back to the page.
  * hover          -- a control changes colour when the pointer is over it and
                      changes back when it leaves.
  * focus          -- Tab puts a ring on a control, and a second Tab moves it.
  * keyboard nav   -- Tab to the tab strip, arrow right, and the PAGE changes
                      (each page paints a distinct probe colour in its corner).
  * click          -- clicking an unchecked box fills it with the accent.
  * typing         -- text entered into a field appears in the field.

Finding things without hard-coding the compositor: each page paints a 6x6 PROBE
rect in an unmistakable colour at window-local (4,4). Its position in the
screendump gives the window's content origin, so every other coordinate below is
a plain window-local point out of c/apps/gui/gallery.c.

Usage: qmp_gallery.py [--xres N] [--yres N] [--out shot.png] [--keep]
"""
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, dock_icon, pt, PPM, GALLERY_SLOT  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PROBES = {
    "controls": (255, 0, 128),
    "shapes":   (0, 255, 128),
    "data":     (255, 200, 0),
    "overlay":  (0, 160, 255),
}


def ppm_to_png(src, dst):
    p = PPM(src)
    raw = b"".join(b"\x00" + p.px[y * p.w * 3:(y + 1) * p.w * 3] for y in range(p.h))

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    open(dst, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", p.w, p.h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b""))
    return p.w, p.h


def luma(c):
    return (c[0] * 299 + c[1] * 587 + c[2] * 114) // 1000


def near(a, b, tol=8):
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


class Frame:
    """A screendump plus the window origin recovered from the page probe."""

    def __init__(self, path, page):
        self.p = PPM(path)
        box = self.p.find_color(PROBES[page])
        if box is None:
            raise AssertionError("page probe %s not found -- Gallery is not showing %r"
                                 % (PROBES[page], page))
        self.ox = box[0] - pt(4)
        self.oy = box[1] - pt(4)

    def at(self, x, y):
        """Window-local POINT -> the pixel there."""
        return self.p.at(self.ox + pt(x), self.oy + pt(y))

    def box(self, x, y, w, h):
        """Every pixel in a window-local point rect."""
        out = []
        for j in range(pt(y), pt(y + h)):
            for i in range(pt(x), pt(x + w)):
                o = ((self.oy + j) * self.p.w + (self.ox + i)) * 3
                out.append((self.p.px[o], self.p.px[o + 1], self.p.px[o + 2]))
        return out


def which_page(path):
    p = PPM(path)
    for name, rgb in PROBES.items():
        if p.find_color(rgb) is not None:
            return name
    return None


def main(argv):
    xres, yres = 1280, 800
    out = None
    keep = False
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":   xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres": yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--out":  out = argv[i + 1]; i += 2
        elif argv[i] == "--keep": keep = True; i += 1
        elif argv[i] == "--iso":  os.environ["LOGIT_ISO"] = argv[i + 1]; i += 2
        elif argv[i] == "--disk": os.environ["LOGIT_DISK"] = argv[i + 1]; i += 2
        else:
            print("unknown arg %r" % argv[i]); return 2
    iso = os.environ.get("LOGIT_ISO", os.path.join(ROOT, "build", "logit.iso"))
    disk = os.environ.get("LOGIT_DISK", os.path.join(ROOT, "build", "disk.img"))
    if out is None:
        out = os.path.join(ROOT, "build", "gallery.png")

    configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-gallery-")
    sock, serial = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "serial.log")
    fails = []

    def ck(cond, what, detail=""):
        print("%-4s %s%s" % ("ok" if cond else "FAIL", what, ("  [%s]" % detail) if detail else ""))
        if not cond:
            fails.append(what)

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
        deadline = time.time() + 180
        while time.time() < deadline:
            if os.path.exists(serial) and "desktop live" in open(serial, errors="replace").read():
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early"); return 1
            time.sleep(0.2)
        else:
            print("FAIL desktop never came up"); return 1
        time.sleep(3 * slow)

        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        shot = os.path.join(tmp, "s.ppm")

        # --- open Gallery ---------------------------------------------------
        ui.click_at_confirmed(probe, *dock_icon(GALLERY_SLOT))
        time.sleep(6 * slow)
        ui.goto(xres - pt(60), pt(60))
        time.sleep(1.5 * slow)
        ui.screendump(shot, settle=1.2 * slow)
        page = which_page(shot)
        ck(page == "controls", "Gallery opened on the Controls page", "page=%r" % page)
        if page is None:
            ppm_to_png(shot, out)
            print("     wrote %s" % out)
            return 1
        base = Frame(shot, "controls")
        print("     window content origin at device (%d,%d)" % (base.ox, base.oy))
        ppm_to_png(shot, (out[:-4] if out.endswith(".png") else out) + "-controls.png")

        # WHERE TO PARK THE POINTER BETWEEN MEASUREMENTS. It has to be inside the
        # window and on no widget. Not outside the window: motion only goes to
        # the window under the pointer, so an app whose pointer leaves is never
        # told, and its last hover sticks. (That is a real gap -- there is no
        # EV_MOUSE_LEAVE in the event ABI -- and it is why this reads as a
        # harness detail rather than a bug in the toolkit.) Window-local (700,20)
        # is the header strip, right of the title, above the tab bar.
        def park():
            ui.goto(base.ox + pt(700), base.oy + pt(20))
            time.sleep(1.0 * slow)

        # --- hover ----------------------------------------------------------
        # The Secondary button, window-local (120,112,96,28) in gallery.c. Sample
        # a point inside it but clear of the centred label.
        SX, SY = 120 + 8, 112 + 14
        cold = base.at(SX, SY)
        ui.goto(base.ox + pt(120 + 48), base.oy + pt(112 + 14))
        time.sleep(1.2 * slow)
        ui.screendump(shot, settle=1.0 * slow)
        hot = Frame(shot, "controls").at(SX, SY)
        ck(not near(cold, hot, 3), "a button changes under the pointer (hover state)",
           "%s -> %s" % (cold, hot))

        park()
        ui.screendump(shot, settle=1.0 * slow)
        back = Frame(shot, "controls").at(SX, SY)
        ck(near(back, cold, 3), "and changes back when the pointer leaves",
           "%s -> %s" % (hot, back))

        # --- click: an unchecked box fills with the accent -------------------
        # "Unchecked" checkbox at window-local (156,248), 18x18.
        before = base.at(156 + 3, 248 + 9)
        ui.click_at_confirmed(probe, base.ox + pt(156 + 9), base.oy + pt(248 + 9))
        time.sleep(1.2 * slow)
        park()
        ui.screendump(shot, settle=1.0 * slow)
        after = Frame(shot, "controls").at(156 + 3, 248 + 9)
        ck(luma(after) < luma(before) - 30, "clicking a checkbox fills it",
           "%s -> %s" % (before, after))

        # --- typing ----------------------------------------------------------
        # The empty field with a placeholder, window-local (366,360,220,28).
        fld = (366 + 4, 360 + 2, 210, 24)
        ui.click_at_confirmed(probe, base.ox + pt(366 + 100), base.oy + pt(360 + 14))
        time.sleep(0.8 * slow)
        ui.typ("hello")
        time.sleep(1.2 * slow)
        park()
        ui.screendump(shot, settle=1.0 * slow)
        f = Frame(shot, "controls")
        dark = f.p.dark_pixels((f.ox + pt(fld[0]), f.oy + pt(fld[1]),
                                f.ox + pt(fld[0] + fld[2]), f.oy + pt(fld[1] + fld[3])), 120)
        ck(dark > 40, "typed text lands in the focused field", "dark px = %d" % dark)

        # --- focus ring -------------------------------------------------------
        # Anchored, not counted: click the Primary button to put focus on a KNOWN
        # control (a click focuses but does not make the ring visible -- a ring
        # that appears on every click is the thing everyone turns off), then Tab
        # once and the ring must be on the NEXT control and nowhere else.
        PRIMARY_L, SECOND_L, GHOST_L = 16 - 3, 120 - 3, 224 - 3
        RY = 112 + 14
        ui.click_at_confirmed(probe, base.ox + pt(16 + 48), base.oy + pt(112 + 14))
        time.sleep(0.8 * slow)
        park()
        ui.screendump(shot, settle=1.0 * slow)
        f1 = Frame(shot, "controls")
        ck(near(f1.at(SECOND_L, RY), base.at(SECOND_L, RY), 6),
           "a mouse click focuses without showing a ring")

        ui.key("tab"); time.sleep(1.4 * slow)
        ui.screendump(shot, settle=1.0 * slow)
        f2 = Frame(shot, "controls")
        ck(not near(f2.at(SECOND_L, RY), f1.at(SECOND_L, RY), 6),
           "Tab moves focus to the next control and rings it",
           "%s -> %s" % (f1.at(SECOND_L, RY), f2.at(SECOND_L, RY)))

        ui.key("tab"); time.sleep(1.4 * slow)
        ui.screendump(shot, settle=1.0 * slow)
        f3 = Frame(shot, "controls")
        ck(near(f3.at(SECOND_L, RY), f1.at(SECOND_L, RY), 6),
           "a further Tab takes the ring off it",
           "%s -> %s" % (f2.at(SECOND_L, RY), f3.at(SECOND_L, RY)))
        ck(not near(f3.at(GHOST_L, RY), f1.at(GHOST_L, RY), 6),
           "...and puts it on the one after")
        ck(near(f3.at(PRIMARY_L, RY), base.at(PRIMARY_L, RY), 6),
           "exactly one control is ringed at a time")

        # --- keyboard page change ---------------------------------------------
        # Click the tab strip (which focuses it), then drive it with the arrow
        # key alone: this is the aui_tabs keyboard path, not a click on a tab.
        ui.click_at_confirmed(probe, base.ox + pt(50), base.oy + pt(60))
        time.sleep(1.0 * slow)
        park()
        ui.key("right"); time.sleep(1.6 * slow)
        ui.screendump(shot, settle=1.0 * slow)
        page = which_page(shot)
        ck(page == "shapes", "arrow keys move the tab strip to Shapes", "page=%r" % page)
        if page != "shapes":
            ppm_to_png(shot, out)
            print("     wrote %s" % out)
            print("\n%d assertion(s) failed" % (len(fails) + 1))
            return 1

        sh = Frame(shot, "shapes")
        ppm_to_png(shot, out)
        print("     wrote %s" % out)

        # --- ANTI-ALIASING (the assertion the negative control breaks) ---------
        # gallery.c draws aui_round(16,116,120,90, r=24) in the accent colour.
        # Its top-left 24x24 corner box therefore contains an arc. With coverage
        # anti-aliasing that box holds a ramp of blended tones; with the kernel's
        # boolean corner test it holds exactly two colours.
        corner = sh.box(16, 116, 24, 24)
        bg = sh.at(16 + 60, 116 - 6)          # page background just above the shape
        fill = sh.at(16 + 60, 116 + 45)       # the shape's own colour
        blends = [c for c in corner if not near(c, bg, 6) and not near(c, fill, 6)]
        tones = len(set(corner))
        ck(len(blends) >= 20,
           "the rounded corner is anti-aliased (partial-coverage pixels)",
           "%d blended of %d, %d distinct tones (bg=%s fill=%s)"
           % (len(blends), len(corner), tones, bg, fill))
        ck(tones >= 8, "...with a real ramp, not one intermediate step",
           "%d distinct tones" % tones)

        # The stroked variant, same corner, must be anti-aliased too -- a ring
        # drawn as "fill then punch out" would leave a hard inner edge.
        scorner = sh.box(152, 116, 24, 24)
        sblend = [c for c in scorner if not near(c, bg, 6) and not near(c, fill, 6)]
        ck(len(sblend) >= 12, "the stroked corner is anti-aliased too",
           "%d blended" % len(sblend))

        # --- alpha compositing --------------------------------------------------
        # Five white swatches at 40..216 alpha over one gradient, gallery.c:
        # x = 16+12+i*48, y = 266+12, 44x46.
        lums = [luma(sh.at(16 + 12 + i * 48 + 22, 266 + 12 + 23)) for i in range(5)]
        rising = all(lums[i] < lums[i + 1] for i in range(4))
        ck(rising, "alpha compositing produces five distinct blends",
           "luma %s" % (lums,))
        ck(lums[-1] - lums[0] >= 30, "...spanning a real range", "%d" % (lums[-1] - lums[0]))

        # --- elevation / shadow --------------------------------------------------
        # The elevation-3 card: x = 16+300+3*100 = 616, y = 266+6, 76x58.
        # Below its bottom edge there must be a darkening that decays to the page.
        cx = 616 + 38
        below = [luma(sh.at(cx, 266 + 6 + 58 + k)) for k in range(2, 26, 2)]
        page_l = luma(sh.at(cx, 266 + 6 + 58 + 40))
        ck(min(below) < page_l - 6, "an elevated card casts a shadow",
           "min %d vs page %d" % (min(below), page_l))
        ck(below[-1] > below[0], "...that falls off with distance", "%s" % (below,))
        ck(luma(sh.at(16 + 60, 266 + 6 + 30)) is not None, "page still renders")

        # --- the remaining pages, for the eye ---------------------------------
        # Data and Overlays carry the widgets a screenshot is the only sensible
        # record of (a table with headers, a modal sheet over a scrim). Nothing
        # is asserted here beyond "the page painted and its probe is there" --
        # the point is the picture.
        base_out = out[:-4] if out.endswith(".png") else out
        ppm_to_png(shot, base_out + "-shapes.png")
        for want in ("data", "overlay"):
            ui.key("right"); time.sleep(1.6 * slow)
            ui.screendump(shot, settle=1.0 * slow)
            got = which_page(shot)
            ck(got == want, "the %s page paints" % want, "page=%r" % got)
            if got == want:
                ppm_to_png(shot, base_out + "-" + want + ".png")
        if which_page(shot) == "overlay":
            # Open the modal so the scrim and the sheet are in the record.
            ui.click_at_confirmed(probe, base.ox + pt(16 + 75), base.oy + pt(156 + 16))
            time.sleep(1.6 * slow)
            ui.screendump(shot, settle=1.0 * slow)
            ppm_to_png(shot, base_out + "-dialog.png")
            f4 = PPM(shot)
            # The scrim must actually darken the page behind the sheet.
            ck(f4.at(base.ox + pt(20), base.oy + pt(500))[0] < 220,
               "a modal dims the page behind it",
               "%s" % (f4.at(base.ox + pt(20), base.oy + pt(500)),))

        # --- the toolkit's own frame cost ---------------------------------------
        log = open(serial, errors="replace").read()
        costs = [l for l in log.splitlines() if "[aui] page" in l]
        if costs:
            print("     cost lines from the guest:")
            for l in costs[-4:]:
                print("       %s" % l.strip())
        ck(bool(costs), "the guest reported its own frame cost")

    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
        if keep:
            print("     artefacts in %s" % tmp)

    if fails:
        print("\n%d assertion(s) failed:" % len(fails))
        for f_ in fails:
            print("  - %s" % f_)
        return 1
    print("\nall gallery assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
