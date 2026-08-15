#!/usr/bin/env python3
"""THE MENU BAR GATE: click "LogitOS", hover to "File", Esc -- and check the
three pictures that produces, not just that the guest claims to have opened
something.

Three checks, matching the unit brief's GATE line exactly:

  1. Click "LogitOS" -> a dropdown is actually ON SCREEN where the guest says
     it put one (glass panel, not the bare wallpaper/menu-bar colour that was
     there before).
  2. Move the pointer to "File" WITHOUT clicking -> the guest reports it
     switched menus, and the picture at "LogitOS"'s old dropdown rect has
     reverted while "File"'s new rect now shows a panel.
  3. Escape -> the guest reports closed, and the LAST dropdown's rect is
     BACK to exactly what it was before any menu ever opened -- byte for
     byte, not merely "looks close". A partial-redraw bug here would leave
     the panel's shadow or glass tint as a ghost, and a byte-for-byte check
     against the true baseline is the only thing that catches that.

All three geometries come from the guest's own serial report ("[wm] menu
title ..." / "[wm] menu open ..."), never re-derived from AA font metrics in
Python -- see the comments on menu_bar_layout()/menu_report_open() in wm.c.

    python3 tests/qmp/qmp_menu.py [--iso build/logit.iso] [--disk build/disk.img]
                                  [--out DIR]
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui                                                   # noqa: E402

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
XRES, YRES = 1280, 800


def boot(iso, disk):
    tmp = tempfile.mkdtemp(prefix="logit-menu-")
    sock = os.path.join(tmp, "qmp.sock")
    serial = os.path.join(tmp, "serial.log")
    proc = subprocess.Popen(
        [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
         "-rtc", "base=localtime",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES),
         "-serial", "file:" + serial, "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 300
    while time.time() < deadline:
        if os.path.exists(serial) and "desktop live" in read(serial):
            time.sleep(2.0)             # let the Finder's open pop finish
            return proc, sock, serial, tmp
        if proc.poll() is not None:
            raise RuntimeError("qemu exited before the desktop came up")
        time.sleep(0.2)
    raise RuntimeError("guest never reported a live desktop")


def read(path):
    try:
        with open(path, errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


TITLE_RE = re.compile(r"\[wm\] menu title (\d+) (\S+) x0 (-?\d+) y0 (-?\d+) x1 (-?\d+) y1 (-?\d+)")
OPEN_RE = re.compile(r"\[wm\] menu open (\d+) x0 (-?\d+) y0 (-?\d+) x1 (-?\d+) y1 (-?\d+)")
CLOSED_RE = re.compile(r"\[wm\] menu closed")


def titles(text):
    """{index: (name, x0, y0, x1, y1)} from the guest's one-time layout report."""
    out = {}
    for m in TITLE_RE.finditer(text):
        g = m.groups()
        out[int(g[0])] = (g[1], int(g[2]), int(g[3]), int(g[4]), int(g[5]))
    return out


def last_open(text):
    """The most recently reported (menu index, x0, y0, x1, y1), or None."""
    got = None
    for m in OPEN_RE.finditer(text):
        g = m.groups()
        got = (int(g[0]), int(g[1]), int(g[2]), int(g[3]), int(g[4]))
    return got


def wait_for(serial, pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        text = read(serial)
        r = pred(text)
        if r is not None:
            return r
        time.sleep(0.05)
    return None


def pixels_in_rect(ppm_path, rect):
    """Raw RGB bytes of `rect` (x0,y0,x1,y1) in a PPM screendump."""
    d = open(ppm_path, "rb").read()
    i = d.index(b"255\n") + 4
    hdr = d[:i].split()
    w = int(hdr[1])
    px = d[i:]
    x0, y0, x1, y1 = rect
    out = bytearray()
    for y in range(y0, y1):
        row_off = y * w * 3
        out += px[row_off + x0 * 3: row_off + x1 * 3]
    return bytes(out)


class Gate:
    def __init__(self):
        self.rows = []

    def check(self, ok, name, detail=""):
        self.rows.append((bool(ok), name, detail))
        return bool(ok)

    def report(self):
        print("\n=== results ===")
        bad = 0
        for ok, name, detail in self.rows:
            print("  %-4s %-58s %s" % ("ok" if ok else "FAIL", name, detail))
            if not ok:
                bad += 1
        print("")
        if bad:
            print("qmp_menu: %d FAILED of %d" % (bad, len(self.rows)))
        else:
            print("PASS: all %d checks" % len(self.rows))
        return bad


def main(argv):
    iso, disk = "build/logit.iso", "build/disk.img"
    outdir = "/tmp/mo/N"
    i = 1
    while i < len(argv):
        if argv[i] == "--iso":    iso = argv[i + 1]; i += 2
        elif argv[i] == "--disk": disk = argv[i + 1]; i += 2
        elif argv[i] == "--out":  outdir = argv[i + 1]; i += 2
        else:
            print("unknown arg %r" % argv[i]); return 2
    os.makedirs(outdir, exist_ok=True)
    g = Gate()

    proc, sock, serial, tmp = boot(iso, disk)
    try:
        ses = qmp_ui.Session(sock, serial=serial)

        # The layout report is printed the first time draw_menubar() runs,
        # which is already true by the time the desktop is "live" -- but wait
        # for it explicitly rather than assume, same discipline wait_windows()
        # uses elsewhere in this test family.
        t = wait_for(serial, lambda s: titles(s) if len(titles(s)) >= 3 else None)
        g.check(t is not None, "guest reported all 3 menu titles' hit rects")
        if not t:
            print("no title layout ever arrived -- nothing else here is checkable")
            return g.report()
        print("[setup] titles: %s" % t)

        baseline = os.path.join(outdir, "0_closed_before.ppm")
        ses.screendump(baseline, settle=0.4)

        # -------------------------------------------------------- 1. LogitOS
        lx0, ly0, lx1, ly1 = t[0][1:]
        cx, cy = (lx0 + lx1) // 2, (ly0 + ly1) // 2
        mark = len(read(serial))
        ses.click_at(cx, cy, settle=0.3)
        opened = wait_for(serial, lambda s: last_open(s[mark:]))
        g.check(opened is not None and opened[0] == 0,
                "clicking LogitOS opened menu 0", "got=%r" % (opened,))
        shot1 = os.path.join(outdir, "1_logitos_open.ppm")
        ses.screendump(shot1, settle=0.3)
        if opened:
            box0 = opened[1:]
            before = pixels_in_rect(baseline, box0)
            after = pixels_in_rect(shot1, box0)
            g.check(before != after,
                    "the LogitOS dropdown's rect actually changed on screen",
                    "box=%s" % (box0,))

        # -------------------------------------------------- 2. hover to File
        fx0, fy0, fx1, fy1 = t[1][1:]
        fcx, fcy = (fx0 + fx1) // 2, (fy0 + fy1) // 2
        mark2 = len(read(serial))
        ses.goto(fcx, fcy, settle=0.3)          # NO click -- hover-switch only
        switched = wait_for(serial, lambda s: last_open(s[mark2:]))
        g.check(switched is not None and switched[0] == 1,
                "hovering File switched to menu 1 with no click", "got=%r" % (switched,))
        shot2 = os.path.join(outdir, "2_file_switched.ppm")
        ses.screendump(shot2, settle=0.3)
        if switched and opened:
            box1 = switched[1:]
            # File's panel now shows something where LogitOS's old panel used
            # to be the only thing open -- and LogitOS's own rect (from shot1)
            # is no longer showing that dropdown.
            g.check(pixels_in_rect(shot2, box1) != pixels_in_rect(baseline, box1),
                    "the File dropdown's rect now differs from the closed baseline",
                    "box=%s" % (box1,))
            g.check(pixels_in_rect(shot2, box0) != pixels_in_rect(shot1, box0),
                    "the LogitOS panel's old rect is no longer what shot1 showed",
                    "box=%s" % (box0,))

        # ------------------------------------------------------------ 3. Esc
        mark3 = len(read(serial))
        ses.key("esc")
        closed = wait_for(serial, lambda s: True if CLOSED_RE.search(s[mark3:]) else None)
        g.check(closed is not None, "Escape closed the menu")
        shot3 = os.path.join(outdir, "3_closed_after.ppm")
        ses.screendump(shot3, settle=0.4)
        if switched:
            box1 = switched[1:]
            same = pixels_in_rect(shot3, box1) == pixels_in_rect(baseline, box1)
            g.check(same, "the covered region is repainted byte-for-byte (no ghost)",
                    "box=%s" % (box1,))

        print("\nscreendumps: %s" % outdir)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()

    return g.report()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
