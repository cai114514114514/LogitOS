#!/usr/bin/env python3
"""Spotlight ("Fluid" P4), on the machine: Cmd+Space, type, Enter.

  1. Cmd+Space starts /sbin/spotlight.aex as a PANEL (the guest's `[wm] panel`
     line), "term" + Enter launches Terminal, and the panel puts itself away.
  2. Cmd+Space again SHOWS the same process (no second launch); "shaping" +
     Enter opens /docs/shaping.txt -- a file found by the index, opened in
     TextEdit.
  3. A query nothing matches leaves only "Ask Assistant"; Enter starts the
     assistant (AEX name "Tasks") with the query as its launch argument.
  4. Escape hides it (`[wm] panel hide`).

Every verdict is a guest serial line: the window manager's own launch/panel
lines and Spotlight's `[spotlight] open` line. --negctl expects a Spotlight
built without its file source (-DSPOTLIGHT_NEGCTL_NO_FILES): scene 2 must fail.

Usage: qmp_spotlight.py --iso ISO [--disk DISK] [--negctl]
"""
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, configure                      # noqa: E402
from qmp_repaint import boot                                    # noqa: E402

XRES, YRES = 1280, 800


def wait_for(ui, mark, pattern, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(pattern, ui.serial_text()[mark:])
        if m:
            return m
        time.sleep(0.25)
    return None


def main(argv):
    iso = argv[argv.index("--iso") + 1]
    disk = argv[argv.index("--disk") + 1] if "--disk" in argv else None
    negctl = "--negctl" in argv
    configure(XRES, YRES)
    tmp = tempfile.mkdtemp(prefix="logit-spotlight-")
    qemu, sock, serial = boot(iso, XRES, YRES, tmp, disk)
    fails = []

    def ck(cond, what):
        print("  %s  %s" % ("ok  " if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    try:
        time.sleep(6)
        ui = Session(sock, serial=serial)

        print("1. Cmd+Space, term, Enter")
        mark = len(ui.serial_text())
        ui.key_mods(("meta_l",), "spc", settle=0.5)
        ck(wait_for(ui, mark, r"\[wm\] panel win=\d+ 'Spotlight'", 60) is not None,
           "Cmd+Space started Spotlight as a panel")
        time.sleep(1.5)
        mark = len(ui.serial_text())
        ui.typ("term")
        time.sleep(0.8)
        a = os.path.join(tmp, "spotlight.ppm")               # also kept for a human to look at
        ui.screendump(a)
        # PARTIAL == FULL for the panel: what typing left on screen (each key a
        # partial composite of the damage set aui reported) must equal a full
        # composite of the same canvas. Cmd+Space twice hides and re-shows the
        # panel -- the query survives, and showing it is a full-screen frame.
        # Found a real under-report: 22 px descenders of the replaced
        # placeholder stayed on screen while aui padded text boxes by px/4.
        ui.key_mods(("meta_l",), "spc", settle=0.8)
        ui.key_mods(("meta_l",), "spc", settle=1.2)
        b = os.path.join(tmp, "spotlight-full.ppm")
        ui.screendump(b)
        pa, pb = PPM(a), PPM(b)
        cx, cy = XRES // 2, YRES // 4
        bad = sum(1 for y in range(cy - 60, cy + 200) for x in range(cx - 300, cx + 300)
                  if pa.at(x, y) != pb.at(x, y))
        ck(bad == 0, "the typed-into panel is pixel-identical to a full composite of it (%d px differ)" % bad)
        ui.key("ret", settle=0.5)
        ck(wait_for(ui, mark, r"\[wm\] launched Terminal", 60) is not None,
           "\"term\" + Enter launched Terminal")
        ck(wait_for(ui, mark, r"\[wm\] panel hide", 10) is not None, "...and the panel put itself away")
        time.sleep(2.0)

        print("2. Cmd+Space again, a file")
        mark = len(ui.serial_text())
        ui.key_mods(("meta_l",), "spc", settle=0.8)
        ck(wait_for(ui, mark, r"\[wm\] panel show", 10) is not None, "the second Cmd+Space showed it")
        ck(re.search(r"\[wm\] launched Spotlight", ui.serial_text()[mark:]) is None,
           "...the SAME process, not a second launch")
        ui.typ("shaping")
        time.sleep(2.5)                          # the first file query builds the index
        mark2 = len(ui.serial_text())
        ui.key("ret", settle=0.5)
        ck(wait_for(ui, mark2, r"\[spotlight\] open kind=1 /docs/shaping\.txt", 20) is not None,
           "\"shaping\" + Enter opened /docs/shaping.txt, a file from the index")
        ck(wait_for(ui, mark2, r"\[wm\] launched TextEdit", 60) is not None, "...in TextEdit")
        if negctl:
            return 1 if fails else 0
        time.sleep(2.0)

        print("3. a query nothing matches -> Ask Assistant")
        mark = len(ui.serial_text())
        ui.key_mods(("meta_l",), "spc", settle=0.8)
        ui.typ("zqxwvj")
        time.sleep(1.0)
        ui.key("ret", settle=0.5)
        ck(wait_for(ui, mark, r"\[spotlight\] open kind=4 zqxwvj", 20) is not None,
           "Enter on the only row asked the assistant")
        ck(wait_for(ui, mark, r"\[wm\] launched Tasks", 60) is not None, "...which started")
        time.sleep(2.0)

        print("4. Escape")
        mark = len(ui.serial_text())
        ui.key_mods(("meta_l",), "spc", settle=0.8)
        ui.key("esc", settle=0.8)
        ck(wait_for(ui, mark, r"\[wm\] panel hide", 10) is not None, "Escape hid the panel")
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
