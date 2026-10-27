#!/usr/bin/env python3
"""Static check of the Makefile wiring for the UI overhaul backlog.

Usage: ui_prewire_check.py [--makefile Makefile] --ids ID [ID ...]

Checks (each failure prints `FAIL <check> <detail>`, exit status 1):
  missing-include  every id has a literal `-include tests/<id>.mk` line
  missing-hook     `DISK_PACK += $(UI_DISK_PACK)`, `$(UI_RELEASE_PACK)` after
                   the wallpaper spec in the release mkfs line, and the
                   AUI_INC_SRC wildcard (aui_*.inc aui_*.h ui_aim.h)
  prereq-hook      NO `$(DISK): $(UI_` / `$(RELEASE_DISK): $(UI_` prerequisite
                   line: prerequisites expand before the fragments are
                   included, so such a hook would always be empty.
"""
import argparse
import re
import sys

AUI_INC = ("AUI_INC_SRC := $(sort $(wildcard $(GUIDIR)/aui_*.inc "
           "$(GUIDIR)/aui_*.h $(GUIDIR)/ui_aim.h))")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--makefile", default="Makefile")
    ap.add_argument("--ids", nargs="+", required=True)
    a = ap.parse_args()
    with open(a.makefile, encoding="utf-8") as f:
        lines = f.read().splitlines()
    bad = 0

    def fail(check, detail):
        nonlocal bad
        bad += 1
        print("FAIL %s %s" % (check, detail))

    inc = {l.strip() for l in lines if l.startswith("-include ")}
    for i in a.ids:
        if "-include tests/%s.mk" % i not in inc:
            fail("missing-include", i)
    if not any(l.strip() == "DISK_PACK += $(UI_DISK_PACK)" for l in lines):
        fail("missing-hook", "DISK_PACK += $(UI_DISK_PACK)")
    if not any(re.search(r"\$\(BUILD\)/wallpaper\.png:/wallpaper\.png\s+\$\(UI_RELEASE_PACK\)\s*$", l)
               for l in lines):
        fail("missing-hook", "UI_RELEASE_PACK after wallpaper.png in the release mkfs line")
    if not any(l.strip() == AUI_INC for l in lines):
        fail("missing-hook", "AUI_INC_SRC")
    for n, l in enumerate(lines, 1):
        if re.match(r"^\$\((RELEASE_)?DISK\):.*\$\(UI_", l):
            fail("prereq-hook", "line %d: %s" % (n, l.strip()))
    if bad == 0:
        print("ok ui-prewire: %d ids wired, hooks present, no prerequisite hooks" % len(a.ids))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
