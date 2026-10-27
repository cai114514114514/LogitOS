#!/usr/bin/env python3
"""The Cmd+Tab switcher, driven the way a hand drives it: Cmd HELD while Tab is
pressed, then Cmd released.

The whole feature rests on one event -- Cmd coming UP -- that this machine did
not have until key-up existed (keyboard.c modk -> wm_mods_changed). QMP sends
real make/break scancodes for each key, so holding meta_l across two Tabs and
letting go is exactly the keyboard sequence a person produces.

Three scenes, with Finder (auto-launched), TextEdit and Terminal open:

  1. BROWSE: hold Cmd, Tab, Tab, wait past SW_SHOW_MS, look, release.
     The panel must be on screen while held (pixels at the screen centre
     change), the guest must report open n=3 sel=1, step sel=2, and the
     release must COMMIT to the third most recently used window: Finder.
  2. TAP: Cmd+Tab and let go at once -> the previous window, no panel.
  3. CANCEL: hold Cmd, Tab, Escape, release -> "cancel", focus unchanged.

Every verdict comes from the guest's own [wm] switcher lines or from its
scanout; nothing is inferred from timing. With --negctl the kernel was built
with -DKEYUP_NEGCTL (the driver drops releases, as it did before key-up), and
scene 1 must FAIL at the commit.

Usage: qmp_switcher.py --iso ISO [--disk DISK] [--negctl]
"""
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, configure, title_matches       # noqa: E402
from qmp_repaint import boot                                    # noqa: E402

XRES, YRES = 1280, 800


def press(ui, qcode, down):
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": down}}])
    time.sleep(0.08)


def tap(ui, qcode):
    press(ui, qcode, True)
    press(ui, qcode, False)


def changed_centre(a, b, half=160):
    n = 0
    cx, cy = a.w // 2, a.h // 2
    for y in range(cy - half // 2, cy + half // 2):
        for x in range(cx - half, cx + half):
            if a.at(x, y) != b.at(x, y):
                n += 1
    return n


def main(argv):
    iso = argv[argv.index("--iso") + 1]
    disk = argv[argv.index("--disk") + 1] if "--disk" in argv else None
    negctl = "--negctl" in argv
    configure(XRES, YRES)
    tmp = tempfile.mkdtemp(prefix="logit-switcher-")
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
        # Confirmed clicks (the guest's own [wm] ptr line says where the
        # pointer is): relative PS/2 motion across a large desktop can drop
        # packets, and a click on the gap between icons launches nothing.
        ui.launch_app("textedit", probe=probe)
        time.sleep(2.0)
        ui.launch_app("terminal", probe=probe)
        time.sleep(3.0)

        print("1. browse: hold Cmd, Tab, Tab, look, release")
        before = os.path.join(tmp, "before.ppm")
        held = os.path.join(tmp, "held.ppm")
        ui.screendump(before)
        mark = len(ui.serial_text())
        press(ui, "meta_l", True)
        tap(ui, "tab")
        tap(ui, "tab")
        time.sleep(0.8)                                  # well past SW_SHOW_MS
        ui.screendump(held)
        press(ui, "meta_l", False)
        time.sleep(1.0)
        log = ui.serial_text()[mark:]
        ck(re.search(r"\[wm\] switcher open n=3 sel=1", log) is not None,
           "Cmd+Tab opened the switcher over 3 windows, on the previous one")
        ck(re.search(r"\[wm\] switcher step sel=2", log) is not None,
           "a second Tab with Cmd still held stepped to the third")
        px = changed_centre(PPM(before), PPM(held))
        ck(px > 3000, "the panel was on screen while Cmd was held (%d px changed at the centre)" % px)
        m = re.search(r"\[wm\] switcher commit win=\d+ '([^']*)'", log)
        ck(m is not None, "releasing Cmd committed the choice")
        if m:
            ck(not title_matches(m.group(1), "terminal")
               and m.group(1) not in ("/untitled.txt", "未命名"),
               "...to the third most recent window, not Terminal or TextEdit (got %r)" % m.group(1))
        if negctl:
            return 1 if fails else 0

        print("2. tap: Cmd+Tab and let go at once")
        mark = len(ui.serial_text())
        ui.key_mods(("meta_l",), "tab", settle=0.8)
        log = ui.serial_text()[mark:]
        m = re.search(r"\[wm\] switcher commit win=\d+ '([^']*)'", log)
        ck(m is not None and title_matches(m.group(1), "terminal"),
           "a quick Cmd+Tab flips back to the previous window, Terminal (got %r)" % (m.group(1) if m else None))

        print("3. cancel: hold Cmd, Tab, Escape, release")
        mark = len(ui.serial_text())
        press(ui, "meta_l", True)
        tap(ui, "tab")
        tap(ui, "esc")
        press(ui, "meta_l", False)
        time.sleep(0.8)
        log = ui.serial_text()[mark:]
        ck("[wm] switcher cancel" in log, "Escape cancelled the switcher")
        ck("[wm] switcher commit" not in log, "...and releasing Cmd afterwards switched nothing")
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
