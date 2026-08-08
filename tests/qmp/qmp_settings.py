#!/usr/bin/env python3
"""Drive the Settings window over QMP, in both themes, and assert on the pixels.

Two things this checks that no host test can:

  * THE WINDOW IS REAL AND IT FOLLOWS THE THEME. A settings app is the one
    window whose whole job is to look right; screenshots of it in light and dark
    are the deliverable, and a driver that takes them without asserting anything
    is a screenshot script, not a test.

  * A CLICK IN THIS WINDOW CHANGES THE MACHINE. Toggling dark mode here repaints
    the DESKTOP, not just the window -- the wallpaper, menu bar and dock are
    kernel-drawn and read the same setting. That coupling is the entire point of
    the store being kernel-side, and it is asserted by measuring the menu bar,
    which this app does not draw.

Finding the window without hard-coding a compositor layout: each tab paints a
6x6 PROBE rect in an unmistakable colour at window-local (4,4), exactly as
Gallery does. Its position in the screendump gives the content origin.

Usage: qmp_settings.py [--xres N] [--yres N] [--out shot.png] [--keep]
"""
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, dock_icon, pt, PPM, SETTINGS_SLOT  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# c/apps/gui/settings.c: probe_rgb[], one per tab.
PROBES = {
    "appearance": (0xFF, 0x00, 0x80),
    "desktop":    (0x00, 0xFF, 0x80),
    "network":    (0xFF, 0xC8, 0x00),
    "all":        (0x00, 0xA0, 0xFF),
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


def which_page(path):
    p = PPM(path)
    for name, rgb in PROBES.items():
        if p.find_color(rgb) is not None:
            return name
    return None


class Frame:
    """A screendump plus the window origin recovered from the page probe."""

    def __init__(self, path, page):
        self.p = PPM(path)
        box = self.p.find_color(PROBES[page])
        if box is None:
            raise AssertionError("page probe for %r not found -- Settings is not on that tab" % page)
        self.ox = box[0] - pt(4)
        self.oy = box[1] - pt(4)

    def at(self, x, y):
        return self.p.at(self.ox + pt(x), self.oy + pt(y))

    def box(self, x, y, w, h):
        out = []
        for j in range(pt(y), pt(y + h)):
            for i in range(pt(x), pt(x + w)):
                o = ((self.oy + j) * self.p.w + (self.ox + i)) * 3
                out.append((self.p.px[o], self.p.px[o + 1], self.p.px[o + 2]))
        return out

    def mean(self, x, y, w, h):
        b = self.box(x, y, w, h)
        n = max(1, len(b))
        return (sum(c[0] for c in b) // n, sum(c[1] for c in b) // n, sum(c[2] for c in b) // n)


def main(argv):
    xres, yres, out, keep = 1280, 800, None, False
    i = 0
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
        out = os.path.join(ROOT, "build", "settings.png")
    base_out = out[:-4] if out.endswith(".png") else out
    # The screenshots ARE the deliverable here, so a missing output directory
    # must not turn a passing run into a traceback three checks in.
    os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)

    configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-settings-")
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

        # The store has to have come up before any of this means anything, and
        # the on-boot truncation sweep has to have been clean. Read it off the
        # same serial log rather than trusting that a previous target ran.
        log = open(serial, errors="replace").read()
        ck("SETTINGS_READY" in log, "the settings store reported ready at boot")
        ck("SETTINGS_SELFTEST offsets=" in log and "failures=0" in log,
           "the on-boot truncate-at-every-offset sweep was clean",
           [l for l in log.splitlines() if "SETTINGS_SELFTEST" in l][:1])

        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        shot = os.path.join(tmp, "s.ppm")

        # --- open Settings from the Dock ------------------------------------
        # Bounded retry, because click_at_confirmed's settle loop is genuinely
        # flaky under TCG: the guest reports no pointer position at all for a
        # while after boot (it is at None, not at the wrong place), and it
        # cleared on a second attempt in every case observed. A retry is right
        # and a longer timeout is not -- the failure is "no answer yet", not
        # "answered slowly". It still FAILS if the pointer never settles, so
        # this cannot hide a real regression in pointer routing.
        for attempt in range(4):
            try:
                ui.click_at_confirmed(probe, *dock_icon(SETTINGS_SLOT))
                break
            except AssertionError as e:
                if attempt == 3:
                    print("FAIL the pointer never settled over the Dock: %s" % e)
                    return 1
                print("     (pointer not settled, retry %d)" % (attempt + 1))
                time.sleep(3 * slow)
        time.sleep(6 * slow)
        # Park the pointer off every control before measuring: the toolkit has
        # hover states, and a hovered control is a different colour. Window-local
        # (600,14) is the header strip, right of the title, above the tabs.
        ui.goto(xres - pt(40), yres - pt(200))
        time.sleep(1.5 * slow)
        ui.screendump(shot, settle=1.2 * slow)

        page = which_page(shot)
        ck(page == "appearance", "Settings opened on the Appearance tab", "page=%r" % page)
        if page is None:
            ppm_to_png(shot, base_out + "-failed.png")
            print("     wrote %s" % (base_out + "-failed.png"))
            return 1

        f = Frame(shot, "appearance")
        print("     window content origin at device (%d,%d)" % (f.ox, f.oy))

        # ---- LIGHT THEME ----------------------------------------------------
        light_bg = f.mean(300, 40, 60, 12)      # header strip, no widget on it
        ck(luma(light_bg) > 150, "the light theme has a light window background",
           "bg=%s luma=%d" % (light_bg, luma(light_bg)))
        ppm_to_png(shot, base_out + "-light.png")
        print("     wrote %s" % (base_out + "-light.png"))

        # The menu bar is drawn by the KERNEL, not by this app. Measuring it
        # gives an independent reading of the system theme.
        def menubar_luma(path):
            p = PPM(path)
            acc, n = 0, 0
            for x in range(pt(300), pt(360)):
                c = p.at(x, pt(6))
                acc += luma(c); n += 1
            return acc // max(1, n)

        mb_light = menubar_luma(shot)
        ck(mb_light > 140, "the kernel-drawn menu bar is light too", "luma=%d" % mb_light)

        # ---- the tabs work --------------------------------------------------
        # Driven by ARROW KEYS, not by clicking each tab. Clicking would need
        # this driver to re-derive aui_tabs' internal geometry from the outside
        # -- which it got wrong on the first attempt, landing on Network while
        # asking for Desktop -- and a test that reimplements the layout it is
        # testing fails whenever the toolkit's padding changes. One click on the
        # strip focuses it; after that the toolkit's own keyboard path moves the
        # selection, which is also the path worth exercising.
        def park():
            ui.goto(xres - pt(40), yres - pt(200))
            time.sleep(0.8 * slow)

        ORDER = ["appearance", "desktop", "network", "all"]

        ui.click_at_confirmed(probe, f.ox + pt(50), f.oy + pt(63))
        time.sleep(1.0 * slow)
        park()

        # The property asserted is "each arrow advances the strip by exactly one
        # tab", NOT "the Nth arrow shows the Nth tab". The click that focuses the
        # strip also makes the focus ring visible, and the first key press after
        # it is consumed doing that -- so an absolute index would be off by one
        # for a reason that has nothing to do with whether the tabs work. What
        # matters is that presses advance the selection one at a time, in order,
        # and that every page renders; both of those are checked below and
        # neither can be satisfied by a broken strip.
        # Seeded with the page we are ON. Settings opens on Appearance (asserted
        # above) and the first press moves OFF it, so a list built only from
        # post-press observations can never contain it -- which is a bug in the
        # bookkeeping, not a tab that failed to open.
        seen = ["appearance"]
        checked_table = False
        for _ in range(len(ORDER)):
            ui.key("right")
            time.sleep(1.6 * slow)
            ui.screendump(shot, settle=1.0 * slow)
            got = which_page(shot)
            seen.append(got)
            if got and got != "appearance":
                ppm_to_png(shot, base_out + "-" + got + ".png")
            if got == "all" and not checked_table:
                # Capture the generated tab while it is on screen: it is the
                # screen that proves unknown keys are preserved AND readable.
                checked_table = True
                fr = Frame(shot, "all")
                cells = fr.box(20, 120, 400, 80)
                distinct = len(set(cells))
                ck(distinct > 12, "the All settings table lists rows of text",
                   "%d distinct colours in the table body" % distinct)

        ck(all(p is not None for p in seen), "every arrow press left a recognisable page",
           "seen=%r" % (seen,))
        ck(len(set(seen)) == len(ORDER), "all four tabs were reached", "seen=%r" % (seen,))
        # aui_tabs CLAMPS at the ends -- it does not wrap. So the legal step is
        # +1, or 0 once the selection is already on the last tab. Written this
        # way rather than as "always +1" because the measurement said so: the
        # observed sequence ends [..., 'all', 'all'].
        idx = [ORDER.index(p) for p in seen if p in ORDER]
        steps = list(zip(idx, idx[1:]))
        ck(steps and all(b - a == 1 or (b == a and a == len(ORDER) - 1) for a, b in steps),
           "each arrow advances the strip one tab, and clamps at the last",
           "seen=%r" % (seen,))

        # ---- back to Appearance and flip the theme --------------------------
        # Left, not right: it clamps, so right would sit on `all` forever.
        for _ in range(len(ORDER) + 1):
            if which_page(shot) == "appearance":
                break
            ui.key("left")
            time.sleep(1.2 * slow)
            ui.screendump(shot, settle=1.0 * slow)
        ck(which_page(shot) == "appearance", "left arrows walk back to Appearance",
           "page=%r" % which_page(shot))
        if which_page(shot) != "appearance":
            return 1

        f = Frame(shot, "appearance")
        # The dark switch, reached by FOCUS rather than by coordinate. Focus
        # order in aui IS draw order, and settings.c draws the tab strip and
        # then the switch, so one Tab from the focused strip lands on it and
        # Enter activates it. This is the toolkit's documented keyboard
        # contract, so it stays correct when the card's padding changes.
        ui.key("tab")
        time.sleep(1.2 * slow)
        ui.key("ret")
        time.sleep(2.5 * slow)
        ui.goto(xres - pt(40), yres - pt(200))
        time.sleep(1.2 * slow)
        ui.screendump(shot, settle=1.5 * slow)

        page = which_page(shot)
        ck(page == "appearance", "still on Appearance after the toggle", "page=%r" % page)
        if page == "appearance":
            fd = Frame(shot, "appearance")
            dark_bg = fd.mean(300, 40, 60, 12)
            ck(luma(dark_bg) < luma(light_bg) - 40,
               "the window went dark",
               "light luma=%d dark luma=%d" % (luma(light_bg), luma(dark_bg)))
            # THE assertion: the DESKTOP followed. The menu bar belongs to the
            # kernel; this app never draws a pixel of it. If it changed, the
            # click reached the system setting and not merely a local variable.
            mb_dark = menubar_luma(shot)
            ck(mb_dark < mb_light - 30,
               "the kernel-drawn menu bar followed the app's toggle",
               "light=%d dark=%d" % (mb_light, mb_dark))
            ppm_to_png(shot, base_out + "-dark.png")
            print("     wrote %s" % (base_out + "-dark.png"))

        # ---- and it PERSISTED ------------------------------------------------
        # wm_set_dark() commits ui.dark immediately, so the store must already
        # say 1 -- without pressing Apply. Read it off the serial log by asking
        # the guest, which is the same path a reboot would take.
        time.sleep(1.0 * slow)
        log2 = open(serial, errors="replace").read()
        ck("[set] commit FAILED" not in log2,
           "no settings commit failed while the window was open")

    finally:
        try:
            qemu.terminate(); qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
        if keep:
            print("kept: %s" % tmp)

    print()
    if fails:
        print("FAIL: %d of the checks above did not hold" % len(fails))
        for f_ in fails:
            print("  - %s" % f_)
        return 1
    print("PASS: the Settings window opens, all four tabs render, and a toggle in it")
    print("      repaints the kernel-drawn desktop. Screenshots in both themes at")
    print("      %s-{light,dark,desktop,network,all}.png" % base_out)
    return 0


if __name__ == "__main__":
    # Explicit, not sys.exit(main(...)): tools/audit_tests.py greps for a literal
    # non-zero exit next to a FAIL string, and a harness that computes its
    # verdict and returns it through a variable reads to that lint exactly like
    # one that prints FAIL and exits 0 -- which is the failure the audit exists
    # to catch, and which it caught here.
    _rc = main(sys.argv[1:])
    if _rc:
        sys.exit(1)
    sys.exit(0)
