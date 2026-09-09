#!/usr/bin/env python3
"""DOES THE MACHINE SAY WHERE ITS APPS ARE, AND DOES THE HELPER BELIEVE IT?

    python3 tests/qmp/qmp_dock_map.py <iso> <disk.img> <disk-alt.img>

THE JAR AND THE TEN DOORS. The Dock's slot order is built at boot by wm.c's
scan_apps() off the disk that booted, and until dock_publish() landed the
machine never published it. Ten places outside the kernel spelled it instead:
qmp_ui.py's NAPPS/BROWSER_SLOT/GALLERY_SLOT/SETTINGS_SLOT, qmp_launch_click.py's
name->index dict, qmp_cursor.py's WIDGETS_SLOT, qmp_monitor.py's MONITOR_SLOT,
and bare integers in qmp_files.py, qmp_fs.py, qmp_freeze.py and qmp_preview.py.
That is CLAUDE.md rule 3 -- one jar, two doors -- at ten doors, and the tree has
already paid for it three times (/dev/log, LOGIT_ARG_MAX, the IME chord).

TASK E (2026-09-02) removed Widgets and Gallery from the Dock entirely --
Widgets dropped from $(APPS), Gallery's pack entry empty by default -- rather
than the halfway measure Gallery used to get (packed, painted, merely
un-clickable). So this gate now also asserts a NEGATIVE fact about the
product disk: scanning its own published line finds neither app, and finds
no `hidden` tile at all (nothing on $(DISK) is tagged AEX_CAT_TEST any more).
The two-disk comparison below no longer removes `widgets` for the ALT machine
(it is not in $(APPS) to remove) -- see tests/dock.mk's DOCK_ALT_APPS, which
now filters out `textedit` instead, chosen for having no other coupling in the
Makefile and not being an app either check below launches on its own.

WHAT MAKES IT EXPENSIVE HERE is that the dock is CENTRED: nreg is in the
geometry, so ONE app more or fewer moves EVERY icon by half a slot. A stale
coordinate does not raise anything. It lands in a gap and nothing happens --
which is indistinguishable from a slow launch -- or it lands on the NEIGHBOUR
and the driver goes on testing the wrong window, green. That second shape is
exactly what tools/check-test-liveness.py exists to find and records verbatim:
"five drivers click the address bar at a coordinate that has been inside the
window-manager titlebar for some time; they pass because the browser happens to
start with that field focused, so the click is decoration."

SO THIS GATE IS NOT "the helper returns a number". Three things are checked, and
only the third is the control:

  1. THE LINE DESCRIBES THE DOCK THAT IS DRAWN, checked by CLICKING it. The
     coordinate is computed from the guest's own line -- slot AND app count --
     and the app the guest then says it launched must be the one asked for. A
     self-consistent parse of a line proves nothing about where draw_dock() put
     the pixels; a launch does.

  2. NEITHER WIDGETS NOR GALLERY IS PRESENT, AND NOTHING IS `hidden`. Both used
     to have Dock tiles -- Gallery's painted but un-clickable (AEX_CAT_TEST),
     Widgets fully ordinary -- and Task E (2026-09-02) removed both as
     applications: dropped from $(APPS) / GALLERY_AEX empty by default, so
     scan_apps() never finds either .aex on the product disk and neither gets
     a slot at all. Checked two ways: the two app names are absent from the
     published line, and the `hidden` field -- which used to be Gallery's
     mechanism for "tile visible, not clickable" -- is false on every entry,
     because nothing on this disk asks for that treatment any more. (The
     mechanism itself still exists in wm.c for anything that packs an
     AEX_CAT_TEST app in the future; this just confirms the product doesn't.)

  3. THE CONTROL, WATCHED FAILING. The second image is the same build with ONE
     APP REMOVED from the pack list (textedit -- not widgets, which Task E
     already dropped from every disk; see tests/dock.mk's DOCK_ALT_APPS
     comment for why textedit). Same ISO, same Python, not one character
     edited between the two halves. The helper must return a DIFFERENT
     slot and a DIFFERENT x for the same app name -- and then the FIRST image's
     coordinate for that app, which is what a hard-coded constant would still be
     handing out, is clicked on the second machine and must NOT launch it. If
     the helper answered the same number on both disks it would be reading its
     own constant and nothing would have been fixed; if the stale coordinate
     still worked, this whole class of bug would not exist and neither would
     this gate.

Env: QEMU (default qemu-system-x86_64), QEMU_CPU (default max).
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui                                                     # noqa: E402
from qmp_ui import Session, dock_icon_of, dock_slot               # noqa: E402

ISO, DISK, DISK_ALT = sys.argv[1], sys.argv[2], sys.argv[3]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
XRES, YRES = 1280, 800

LAUNCH_RE = re.compile(r"\[wm\] launched (.+)")

fails = []


def check(ok, msg):
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails.append(msg)


class Guest:
    """One booted machine, torn down on exit. -snapshot: nothing here writes."""

    def __init__(self, disk):
        self.tmp = tempfile.mkdtemp(prefix="logit-dock-")
        self.sock = os.path.join(self.tmp, "qmp.sock")
        self.serial = os.path.join(self.tmp, "serial.log")
        self.proc = subprocess.Popen(
            [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
             "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
             "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
             "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
             "-rtc", "base=localtime",
             "-vga", "none",
             "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES),
             "-serial", "file:" + self.serial, "-no-reboot",
             "-display", "none", "-qmp", "unix:%s,server,nowait" % self.sock],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 300
        while True:
            if os.path.exists(self.serial) and "desktop live" in self.log():
                time.sleep(2.0)          # let the Finder's open animation settle
                break
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited before the desktop came up (%s)" % disk)
            if time.time() > deadline:
                raise RuntimeError("guest never reported a live desktop (%s)" % disk)
            time.sleep(0.2)
        self.s = Session(self.sock, serial=self.serial)

    def log(self):
        try:
            with open(self.serial, errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""

    def click_and_watch(self, x, y, settle=4.0):
        """Click, then return the app names the GUEST says it launched after it.

        The guest's own report, not a screenshot: the pointer is on the display's
        hardware cursor plane and windows animate open, so "did this click launch
        something" read off pixels is two guesses stacked. `[wm] launched NAME`
        is printed by wm.c's launch path itself."""
        mark = len(self.log())
        self.s.click_at(x, y)
        time.sleep(settle)
        return LAUNCH_RE.findall(self.log()[mark:])

    def close(self):
        try:
            self.s.cmd({"execute": "quit"})
        except Exception:
            pass
        try:
            self.proc.wait(timeout=20)
        except Exception:
            self.proc.kill()


def main():
    qmp_ui.configure(XRES, YRES)

    # ---- machine A: the product disk ------------------------------------
    print("A: the shipped disk (%s)" % DISK)
    a = Guest(DISK)
    try:
        dock_a = a.s.dock()
        print("   guest says: " + " ".join(
            "%d=%s%s" % (e.slot, e.file, ",hidden" if e.hidden else "") for e in dock_a))
        check(len(dock_a) > 1, "the guest published a dock of %d apps" % len(dock_a))

        # 1. the published line describes the dock that is DRAWN.
        x, y = dock_icon_of("terminal", dock_a)
        got = a.click_and_watch(x, y)
        check(got == ["Terminal"],
              "slot %d at (%d,%d) launches Terminal -- guest said %r"
              % (dock_slot("terminal", dock_a), x, y, got))

        # 2. Widgets and Gallery are gone as APPLICATIONS, not merely hidden --
        # Task E's whole point. No tile, no slot, and (since nothing else on
        # this disk asks for the AEX_CAT_TEST treatment) no `hidden` entry.
        present = {e.file for e in dock_a}
        check("widgets.aex" not in present, "widgets.aex has no Dock tile")
        check("gallery.aex" not in present, "gallery.aex has no Dock tile")
        hidden = [e for e in dock_a if e.hidden]
        check(hidden == [], "no tile is marked hidden (got %r)"
              % [e.file for e in hidden])

        stale = dock_icon_of("settings", dock_a)
        slot_a = dock_slot("settings", dock_a)
    finally:
        a.close()

    # ---- machine B: the same build, one app fewer on the disk ------------
    print("B: one app removed from the pack list (%s)" % DISK_ALT)
    b = Guest(DISK_ALT)
    try:
        dock_b = b.s.dock()
        print("   guest says: " + " ".join(
            "%d=%s%s" % (e.slot, e.file, ",hidden" if e.hidden else "") for e in dock_b))
        check(len(dock_b) == len(dock_a) - 1,
              "the dock composition changed: %d apps -> %d" % (len(dock_a), len(dock_b)))
        check(not any(e.file == "textedit.aex" for e in dock_b),
              "textedit.aex is the app that is gone")

        # THE CONTROL. No Python was edited between these two lines.
        slot_b = dock_slot("settings", dock_b)
        fresh = dock_icon_of("settings", dock_b)
        check(slot_b != slot_a,
              "the helper answers a DIFFERENT slot for 'settings': %d -> %d"
              % (slot_a, slot_b))
        check(fresh != stale,
              "and a different tile centre: %r -> %r" % (stale, fresh))

        # The stale coordinate FIRST: it must do nothing, and a launched
        # Settings window would sit over the dock for the next click.
        got = b.click_and_watch(*stale)
        check(got == [],
              "machine A's Settings coordinate %r launches nothing here -- this is "
              "what a hard-coded slot still hands out; guest said %r" % (stale, got))

        got = b.click_and_watch(*fresh)
        check(got == ["Settings"],
              "the coordinate read off THIS machine launches Settings -- guest said %r"
              % (got,))
    finally:
        b.close()

    if fails:
        print("\ndock-map: %d FAILED" % len(fails))
        for m in fails:
            print("  - " + m)
        return 1
    print("\ndock-map: ok -- the dock's order is read from the guest and follows it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
