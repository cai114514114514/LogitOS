#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ui-aims-b: the Monitor, Clock, Studio and Preview drivers click aims, not constants.

Design P13 and P14 (addendum I). Against tests/ui-aims-b.serial (the `[monitor]`, `[clock]`,
`[studio]`, `[preview]` aim lines and the `[wm] win` lines of real boots of the test images built
from this tree), for every aim a migrated driver resolves:

  1. the guest published it
       FAIL aim-missing <app> <name>
  2. it maps to a point inside the 1280x800 screen through the WM's window frame line (the driver's
     own click path, qmp_aim.Aims.screen)
       FAIL aim-offscreen <app> <name> <x> <y>
  3. the app source (or aui, for the `dialog-*` names aui_dialog_buttons owns) publishes that name,
     so the fixture cannot go stale
       FAIL aim-unpublished <app> <name>
  4. no driver spells a click with a numeric screen literal; what is left is design-pinned (addendum
     D.6): the menu-bar dark toggle, the titlebar points
       FAIL literal-click <driver>:<line>
  5. P14: the Preview viewer's 0xFD0181 anchor is painted only behind qmp_probes_on()
       FAIL probe-ungated preview/view.inc:<line>

--negctl drops the `[monitor] aim tab-memory` lines from the fixture before step 1 and so must log
`FAIL aim-missing monitor tab-memory`. Exit 0 only when no FAIL line was printed.
"""
import argparse
import os
import re
import sys

MON = "tests/qmp/qmp_monitor.py"
CLK = "tests/boot/run-clock-ui.py"
STU = "tests/boot/run-studio-ui.py"
STC = "tests/boot/run-studio-completion.py"
PRV = "tests/qmp/qmp_preview.py"
NAV = "tests/qmp/qmp_preview_nav.py"

# driver -> (app, [regexes whose group 1 is an aim name]); a name with a trailing %d is expanded below.
DRIVERS = {
    MON: ("monitor", [r"\baim\(\"([\w-]+)\"\)", r"click_aim\(\w+, \"([\w-]+)\"\)", r"\"(tab-\w+)\"",
                      r"row_top\((\d+)\)"]),
    CLK: ("clock", [r"aim_point\('([\w-]+)'\)"]),
    STU: ("studio", [r"aclick\('([\w-]+)'"]),
    STC: ("studio", [r"\baim\(\"([\w-]+)\"\)"]),
    PRV: ("preview", [r"click_preview_aim\([^\n]*\"([\w-]+)\"\)"]),
    NAV: ("preview", []),
}
SOURCES = {
    "monitor": ["c/apps/gui/monitor/monitor.c"],
    "clock": ["c/apps/gui/clock/view.c", "c/apps/gui/clock/main.c"],
    "studio": ["c/apps/gui/studio/studio.c", "c/apps/gui/studio/core/studio_view.inc",
               "c/apps/gui/studio/core/studio_project.inc"],
    "preview": ["c/apps/gui/preview/preview.c", "c/apps/gui/preview/view.inc",
                "c/apps/gui/preview/picker.inc"],
}
AUI_SRC = ["c/apps/gui/aui_overlay.inc", "c/apps/gui/aui_lists.inc"]
# Known screen points a driver may still click (design-pinned, addendum D.6).
PINNED = ("DARK_X", "1280 - 221", "f.oy - pt(20)", "pt(15)", "if a.before")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--negctl", action="store_true")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    sys.path.insert(0, os.path.join(root, "tests", "qmp"))
    import qmp_aim

    def read(rel):
        with open(os.path.join(root, rel), encoding="utf-8") as fh:
            return fh.read()

    serial = read("tests/ui-aims-b.serial")
    if a.negctl:
        serial = "\n".join(l for l in serial.splitlines()
                           if not l.startswith("[monitor] aim tab-memory "))
    aims = qmp_aim.parse_aims(serial)

    class Serial:                                   # what Aims needs of a session
        @staticmethod
        def serial_text():
            return serial

    src = {app: "\n".join(read(p) for p in ps) for app, ps in SOURCES.items()}
    aui = "\n".join(read(p) for p in AUI_SRC)
    fails, seen = [], set()
    aimer = qmp_aim.Aims(Serial)

    def names_of(path, app, rxs):
        text = read(path)
        out = []
        for rx in rxs:
            for m in re.finditer(rx, text):
                n = m.group(1)
                out.append("row-" + n if n.isdigit() else n)
        if path in (PRV, NAV):
            out.append("cell-0")                     # click_preview_cell: any library row's aim
        return out

    for path, (app, rxs) in DRIVERS.items():
        for name in names_of(path, app, rxs):
            if (app, name) in seen:
                continue
            seen.add((app, name))
            got = aims.get((app, name))
            if got is None:
                fails.append("FAIL aim-missing %s %s" % (app, name))
                continue
            print("ok aim %s %s %d %d (%s)" % (app, name, got[0], got[1], os.path.basename(path)))
            try:
                sx, sy = aimer.screen(app, name, timeout=0)
                if not (0 <= sx < 1280 and 0 <= sy < 800):
                    fails.append("FAIL aim-offscreen %s %s %d %d" % (app, name, sx, sy))
            except qmp_aim.AimTimeout as exc:
                fails.append("FAIL aim-offscreen %s %s no-window (%s)" % (app, name, exc))
            stem = re.sub(r"-\d+$", "", name)
            if name.startswith("dialog-"):
                ok = '"dialog-confirm"' in aui and '"dialog"' in aui
            elif name.startswith("row-"):
                ok = 'aui_aim_prefix("row")' in src[app]
            else:
                ok = '"%s"' % name in src[app] or '"%s"' % stem in src[app]
            if not ok:
                fails.append("FAIL aim-unpublished %s %s" % (app, name))

    # 4. no numeric click literals left in the drivers.
    clickcall = re.compile(r"\b(click_at_confirmed|click)\(([^\n]*)")
    for path in DRIVERS:
        for n, line in enumerate(read(path).splitlines(), 1):
            code = line.split("#", 1)[0]
            if re.search(r"\bdef (click|click_at_confirmed)\(", code):
                continue
            for m in clickcall.finditer(code):
                args = m.group(2)
                if re.search(r"(?<![\w.])\d+\s*,\s*\d+|pt\(\d", args) and not any(p in code for p in PINNED):
                    fails.append("FAIL literal-click %s:%d" % (path, n))

    # 5. P14: the viewer anchor is gated.
    for n, line in enumerate(read("c/apps/gui/preview/view.inc").splitlines(), 1):
        if "0xFD0181" in line and "qmp_probes_on()" not in line and not line.lstrip().startswith(("/*", "*")):
            fails.append("FAIL probe-ungated preview/view.inc:%d" % n)

    for f in fails:
        print(f)
    if not seen:
        print("FAIL no-aims-checked")
        return 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
