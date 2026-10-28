#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ui-aims-c: Browser chrome, Terminal, Assistant and Chat publish aims, and
their drivers click / look for them instead of keeping a copy of the layout.

Design P13 (addendum I). Against tests/ui-aims-c.serial (the `[browser]`,
`[terminal]`, `[assistant]`, `[ch]` aim lines and `[wm] win` lines of real boots
of the test image: a Browser with two tabs, a Terminal, an Assistant, and the
Chat window of qmp_ch.py):

  1. every aim a migrated driver resolves was published by the guest
       FAIL aim-missing <app> <name>
  2. each of them maps to a point inside the 1280x800 screen through the WM's
     window frame line (the drivers' own click path: qmp_aim.Aims.screen)
       FAIL aim-offscreen <app> <name> <x> <y>
  3. the app source publishes that name, so the fixture cannot go stale; the
     names of the spec that no boot reaches (task-<i>, the Browser's caret)
     are checked in the source only
       FAIL aim-unpublished <app> <name>
  4. qmp_tabs.py and qmp_ch.py click no numeric screen coordinate
       FAIL literal-click <driver>:<line>

--negctl drops the `[browser] aim caret` lines from the fixture before step 1
and so must log `FAIL aim-missing browser caret` and nothing else.
Exit 0 only when no FAIL line was printed.
"""
import argparse
import os
import re
import sys


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

    serial = read("tests/ui-aims-c.serial")
    if a.negctl:
        serial = "\n".join(l for l in serial.splitlines()
                           if not l.startswith("[browser] aim caret "))
    aims = qmp_aim.parse_aims(serial)

    class Serial:                                   # what Aims needs of a session
        @staticmethod
        def serial_text():
            return serial

    # driver -> [(regex whose group 1 is the aim name, app)]
    drivers = {
        "tests/qmp/qmp_addrbar.py": [(r'screen\("browser", "([\w-]+)"', "browser")],
        "tests/qmp/qmp_tabs.py": [(r'(?:screen|wait)\("browser", "([\w-]+)"', "browser")],
        "tests/qmp/qmp_term.py": [(r'aim (input) ', "terminal")],
        "tests/qmp/qmp_cjk_term.py": [(r'wait\("terminal", "([\w-]+)"', "terminal")],
        "tests/qmp/qmp_audio_term.py": [(r'wait\("terminal", "([\w-]+)"', "terminal")],
        "tests/qmp/qmp_ch.py": [(r'\baim\(g, "([\w-]+)"', "ch")],
    }
    sources = {"browser": ["c/apps/gui/browser/app/main.c", "c/apps/gui/browser/app/address.inc"],
               "terminal": ["c/apps/gui/terminal/terminal.c"],
               "assistant": ["c/apps/gui/assistant/assistant.c"],
               "ch": ["c/apps/gui/ch/ch.c"]}
    src_text = {app: "\n".join(read(p) for p in ps) for app, ps in sources.items()}
    # names the spec lists that no driver clicks: the source must still emit them
    spec_only = [("assistant", "field"), ("assistant", "send"), ("assistant", "suggestion-0"),
                 ("assistant", "store"), ("assistant", "task-0"), ("browser", "caret"), ("ch", "send")]

    def emitted(app, name):
        body = src_text[app]
        stem = re.sub(r"-\d+$", "", name)
        return '"%s"' % name in body or '"%s"' % stem in body

    fails = []
    seen = set()
    aimer = qmp_aim.Aims(Serial)
    for path, pats in drivers.items():
        text = read(path)
        for rx, app in pats:
            for m in re.finditer(rx, text):
                name = m.group(1)
                if (app, name) in seen:
                    continue
                seen.add((app, name))
                got = aims.get((app, name))
                if got is None:
                    fails.append("FAIL aim-missing %s %s" % (app, name))
                else:
                    print("ok aim %s %s %d %d (%s)" % (app, name, got[0], got[1], os.path.basename(path)))
                    try:
                        sx, sy = aimer.screen(app, name, timeout=0)
                        if not (0 <= sx < 1280 and 0 <= sy < 800):
                            fails.append("FAIL aim-offscreen %s %s %d %d" % (app, name, sx, sy))
                    except qmp_aim.AimTimeout as exc:
                        fails.append("FAIL aim-offscreen %s %s no-window (%s)" % (app, name, exc))
                if not emitted(app, name):
                    fails.append("FAIL aim-unpublished %s %s" % (app, name))
    for app, name in spec_only:
        if (app, name) not in seen and not emitted(app, name):
            fails.append("FAIL aim-unpublished %s %s" % (app, name))
        seen.add((app, name))
    # The assistant is in the fixture too (a boot of the real window).
    for app, name in (("assistant", "field"), ("assistant", "send"), ("assistant", "suggestion-0"),
                      ("assistant", "store"), ("ch", "send")):
        if (app, name) not in aims:
            fails.append("FAIL aim-missing %s %s" % (app, name))

    bad = re.compile(r"(?<![\w.])(click_at|goto)\(\s*-?\d")
    for path in ("tests/qmp/qmp_tabs.py", "tests/qmp/qmp_ch.py"):
        for n, line in enumerate(read(path).splitlines(), 1):
            code = line.split("#", 1)[0]
            if bad.search(code):
                fails.append("FAIL literal-click %s:%d" % (path, n))

    for f in fails:
        print(f)
    if not seen:
        print("FAIL no-aims-checked")
        return 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
