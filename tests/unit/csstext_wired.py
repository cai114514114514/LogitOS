#!/usr/bin/env python3
"""tests/unit/csstext_wired.py -- every source list that names layout.c must
also name layout_text.c.

WHY THIS EXISTS.  c/apps/browser/layout.c calls ltx_break_utf8(), which lives
in c/apps/browser/layout_text.c.  layout_flex.c and layout_grid.c dodged this
problem by being textually #included; layout_text.c cannot, because it is
measured on its own against the Unicode conformance corpus and folding it into
layout.c's translation unit would put the file under test inside the engine it
is tested independently of.  So the dependency is a LINK dependency, and there
are 31 places in this tree that spell a link line by hand.

CLAUDE.md names that shape as one of the five reasons a gate in this tree goes
red for a reason unrelated to the code under test: "a source file grew a
dependency and the link line did not follow", credited with taking down
test-canvas, test-frameworks, test-platform-*, test-mse*, test-demux-expect and
test-tcp-host.  The answer to a hand-copied list is a check, not care.

AND IT JOINS CONTINUATIONS FIRST, which is CLAUDE.md's rule 2 and the reason
six other make-parsing tools in this tree carry the same re.sub on their first
line: `LBOX_SRC := a \\\\\\n b` is one logical line, and a checker that reads it
as two sees a list that does not contain what it does contain.

THE SELF-CHECK.  A parser that matched nothing would report success forever, so
this refuses to pass on zero hits: if it cannot find layout.c in a single link
line it says so and exits non-zero, because that is a broken checker and not a
clean tree.
"""
import re
import sys
import glob
import os

NEEDS = "c/apps/browser/layout.c"
WANTS = "c/apps/browser/layout_text.c"


def logical_lines(path):
    """The file's lines with make's backslash continuations joined, comments
    stripped, each tagged with the line number it STARTED on."""
    raw = open(path, encoding="utf-8").read().split("\n")
    out, i = [], 0
    while i < len(raw):
        start = i + 1
        buf = raw[i]
        while buf.endswith("\\") and i + 1 < len(raw):
            i += 1
            buf = buf[:-1] + " " + raw[i].lstrip()
        h = buf.find("#")
        out.append((start, buf if h < 0 else buf[:h]))
        i += 1
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    files = sorted(glob.glob(os.path.join(root, "tests", "*.mk")))
    files.append(os.path.join(root, "Makefile"))
    word = re.compile(re.escape(NEEDS) + r"(?=[\s\\]|$)")

    hits, bad = 0, []
    for f in files:
        if not os.path.exists(f):
            continue
        for lineno, code in logical_lines(f):
            if not word.search(code):
                continue
            hits += 1
            if WANTS not in code:
                bad.append((os.path.relpath(f, root), lineno, code.strip()))

    print("csstext_wired: %d link line(s) name %s" % (hits, NEEDS))
    if hits == 0:
        print("  FAIL: none found at all -- this checker is broken, not the tree")
        return 1
    if bad:
        print("  FAIL: %d of them do not also name %s:" % (len(bad), WANTS))
        for f, n, code in bad:
            print("      %s:%d  %s" % (f, n, code[:140]))
        print("  layout.c calls ltx_break_utf8(); a link without layout_text.c is")
        print("  an undefined symbol, which reads as a broken gate.")
        return 1
    print("  ok: all %d also name %s" % (hits, WANTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
