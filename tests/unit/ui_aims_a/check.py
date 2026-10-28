#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ui-aims-a: the Finder and TextEdit drivers click aims, not constants.

Design P13 (addendum I). For each migrated driver this checks, against
tests/ui-aims-a.serial (the `[files]`, `[textedit]` and `[wm] win` lines of a
real boot of the tour image):

  1. every aim the driver resolves was published by the guest
       FAIL aim-missing <app> <name>
  2. each of them maps to a point inside the 1280x800 screen through the WM's
     window frame line (the driver's own click path: qmp_aim.Aims.screen)
       FAIL aim-offscreen <app> <name> <x> <y>
  3. the app source publishes that name (the fixture cannot go stale)
       FAIL aim-unpublished <app> <name>
  4. no driver spells a click, goto or pos() with a numeric literal; the only
     constant left is the close light (18,-15), pinned WM geometry (addendum D.6)
       FAIL literal-click <driver>:<line>

--negctl drops the `[files] aim work-field` lines from the fixture before
step 1 and so must log `FAIL aim-missing files work-field`.
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

    serial = read("tests/ui-aims-a.serial")
    if a.negctl:
        serial = "\n".join(l for l in serial.splitlines()
                           if not l.startswith("[files] aim work-field "))
    aims = qmp_aim.parse_aims(serial)

    class Serial:                                   # what Aims needs of a session
        @staticmethod
        def serial_text():
            return serial

    # driver -> (regex, group order) of the aim references it resolves.
    # (app, name) tuples come either from the call (app, name) or fixed per helper.
    drivers = {
        "tests/boot/run-textedit-work.py": [(r"\bat\('([\w-]+)'\)", "textedit")],
        "tests/boot/run-textedit-document.py": [(r"\bat\('([\w-]+)'\)", "textedit")],
        "tests/boot/run-project-work.py": [(r"\bfa\('([\w-]+)'\)", "files"),
                                           (r"\bta\('([\w-]+)'\)", "textedit")],
        "tests/boot/run-agent-session.py": [(r"\baim\('(files|textedit)', '([\w-]+)'\)", None)],
        "tests/qmp/qmp_files.py": [(r"\baim_screen\(\"([\w-]+)\"\)", "files")],
    }
    sources = {"files": ["c/apps/gui/files/files.c", "c/apps/gui/files/files_project.inc"],
               "textedit": ["c/apps/gui/textedit/textedit.c", "c/apps/gui/textedit/textedit_work.inc"]}
    src_text = {app: "\n".join(read(p) for p in ps) for app, ps in sources.items()}

    fails = []
    seen = set()
    aimer = qmp_aim.Aims(Serial)
    for path, pats in drivers.items():
        text = read(path)
        for rx, app in pats:
            for m in re.finditer(rx, text):
                if app is None:
                    use_app, name = m.group(1), m.group(2)
                else:
                    use_app, name = app, m.group(1)
                if (use_app, name) in seen:
                    continue
                seen.add((use_app, name))
                got = aims.get((use_app, name))
                if got is None:
                    fails.append("FAIL aim-missing %s %s" % (use_app, name))
                    continue
                print("ok aim %s %s %d %d (%s)" % (use_app, name, got[0], got[1], os.path.basename(path)))
                try:
                    sx, sy = aimer.screen(use_app, name, timeout=0)
                    if not (0 <= sx < 1280 and 0 <= sy < 800):
                        fails.append("FAIL aim-offscreen %s %s %d %d" % (use_app, name, sx, sy))
                except qmp_aim.AimTimeout as exc:
                    fails.append("FAIL aim-offscreen %s %s no-window (%s)" % (use_app, name, exc))
                stem = re.sub(r"-\d+$", "", name)
                body = src_text[use_app]
                emitted = ('"%s"' % name in body or '"%s"' % stem in body or
                           (stem.startswith("sidebar") and '"sidebar-"' in body and '"%s"' % name.split("-", 1)[1] in body))
                if not emitted:
                    fails.append("FAIL aim-unpublished %s %s" % (use_app, name))

    # 4. no numeric click / goto / pos literals left in the drivers.
    bad = re.compile(r"(?<![\w.])(click|goto)\((?!\)|\*|18,-15\)|\"right\"\)|px,py|x,y|fx, fy|tx,ty)|\bpos(?:ition)?\(\s*-?\d")
    for path in drivers:
        for n, line in enumerate(read(path).splitlines(), 1):
            code = line.split("#", 1)[0]
            if re.search(r"\bdef (click|goto)\(", code):
                continue
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
