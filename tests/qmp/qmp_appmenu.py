#!/usr/bin/env python3
"""An app's own menus on the menu bar ("Fluid" P5), on the machine.

Until SYS_GUI_MENU_SET the bar was three kernel constants. Now it follows the
top window. This drives the whole path a person uses:

  1. The Finder (auto-launched) published File/Edit/View/Go: the guest's
     `[wm] menus` line shows them between LogitOS and the kernel's Window.
  2. TextEdit comes up and the bar follows it: LogitOS File Edit Assistant
     Window.
  3. Type "hello world", open Edit BY CLICKING ITS TITLE (geometry from the
     guest's own `[wm] menu title` report), click the "Select All" row (from
     `[wm] menu row`), and type "Z" -- which replaces the selection only if
     Select All actually reached the app.
  4. Cmd+S -- a letter the window manager does not claim -- must be routed to
     TextEdit's "Save" item (`[wm] menu key ... 'Save'`).
  5. THE VERDICT IS THE FILE. QEMU runs WITHOUT -snapshot on a private copy
     of the disk, and /untitled.txt is read back off it on the host: it must
     be exactly "Z". A serial line saying an item fired is the kernel agreeing
     with itself; the bytes on the disk are the app agreeing with the person.

The control is a kernel built with -DWM_MENU_NEGCTL_DROP_EVENT: it still logs
every choice but never delivers EV_MENU, so the log checks pass and the file
check must FAIL (make test-appmenu-negctl).

Usage: qmp_appmenu.py --iso ISO [--disk DISK]
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure                           # noqa: E402
from qmp_repaint import boot                                    # noqa: E402

XRES, YRES = 1280, 800
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def wait_for(ui, mark, pattern, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(pattern, ui.serial_text()[mark:])
        if m:
            return m
        time.sleep(0.25)
    return None


def last_title(log, name):
    """The newest `[wm] menu title` report for `name` -- titles move with focus."""
    ms = re.findall(r"\[wm\] menu title (\d+) (\S+) x0 (\d+) y0 (\d+) x1 (\d+) y1 (\d+)", log)
    for m in reversed(ms):
        if m[1] == name:
            return tuple(int(v) for v in (m[2], m[3], m[4], m[5]))
    return None


def main(argv):
    iso = argv[argv.index("--iso") + 1]
    disk = argv[argv.index("--disk") + 1] if "--disk" in argv else \
        os.path.join(os.path.dirname(iso), "disk.img")
    configure(XRES, YRES)
    tmp = tempfile.mkdtemp(prefix="logit-appmenu-")
    work = os.path.join(tmp, "disk.img")
    # A clone, not the build's image: this run WRITES the disk (no -snapshot).
    if subprocess.call(["cp", "-c", disk, work], stderr=subprocess.DEVNULL) != 0:
        shutil.copyfile(disk, work)
    qemu, sock, serial = boot(iso, XRES, YRES, tmp, work, snapshot=False)
    fails = []

    def ck(cond, what):
        print("  %s  %s" % ("ok  " if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    try:
        time.sleep(6)
        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")

        print("1. the Finder's menus")
        ck(wait_for(ui, 0, r"\[wm\] menus win=\d+ n=6 LogitOS File Edit View Go Window", 30) is not None,
           "the Finder published File/Edit/View/Go, framed by LogitOS and Window")

        print("2. TextEdit's menus follow it to the top")
        mark = len(ui.serial_text())
        ui.launch_app("textedit", probe=probe)
        m = wait_for(ui, mark, r"\[wm\] menus win=(\d+) n=5 LogitOS File Edit Assistant Window", 30)
        ck(m is not None, "the bar now reads LogitOS File Edit Assistant Window")
        win = m.group(1) if m else r"\d+"
        time.sleep(2.0)

        print("3. type, then Edit > Select All by clicking")
        ui.typ("hello world")
        time.sleep(1.0)
        t = last_title(ui.serial_text(), "Edit")
        ck(t is not None, "the guest reported where \"Edit\" is")
        if t:
            mark = len(ui.serial_text())
            ui.click_at_confirmed(probe, (t[0] + t[2]) // 2, (t[1] + t[3]) // 2)
            o = wait_for(ui, mark, r"\[wm\] menu open \d+ x0 (\d+) y0 \d+ x1 (\d+)", 10)
            r = wait_for(ui, mark, r"\[wm\] menu row \d+ 'Select All' y0 (\d+) y1 (\d+)", 10)
            ck(o is not None and r is not None, "Edit opened and listed a \"Select All\" row")
            if o and r:
                x = (int(o.group(1)) + int(o.group(2))) // 2
                y = (int(r.group(1)) + int(r.group(2))) // 2
                mark = len(ui.serial_text())
                ui.click_at_confirmed(probe, x, y)
                ck(wait_for(ui, mark, r"\[wm\] menu fire win=%s id=1 'Select All'" % win, 10) is not None,
                   "clicking the row chose Select All (id 1, TextEdit's Ctrl+A code)")
        time.sleep(0.8)
        ui.typ("Z")
        time.sleep(0.8)

        print("4. Cmd+S reaches the Save item")
        mark = len(ui.serial_text())
        ui.key_mods(("meta_l",), "s", settle=0.5)
        ck(wait_for(ui, mark, r"\[wm\] menu key win=%s id=19 'Save'" % win, 10) is not None,
           "Cmd+S was routed to TextEdit's Save (id 19)")
        time.sleep(4.0)                           # the write and its journal commit
    finally:
        qemu.kill()
        qemu.wait()

    print("5. the file on the disk")
    out = os.path.join(tmp, "untitled.txt")
    rc = subprocess.call([sys.executable, os.path.join(ROOT, "tests/boot/lfs_extract.py"),
                          work, "/untitled.txt", out],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    got = open(out, "rb").read() if rc == 0 and os.path.exists(out) else None
    ck(got == b"Z", "/untitled.txt is exactly \"Z\" -- Select All then Z, saved by Cmd+S (got %r)" % (got,))

    if fails:
        print("FAIL (%d): %s   (kept: %s)" % (len(fails), "; ".join(fails), tmp))
        return 1
    shutil.rmtree(tmp, ignore_errors=True)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
