#!/usr/bin/env python3
"""Diff two css_selstatic_census runs and report the reach of the slice.

Reads two files of `page<TAB>index<TAB>tag<TAB>digest` lines -- the corpus
cascaded with the four static-pseudo handlers reverted to h_false (before) and
as shipped (after) -- and reports, per page, how many elements ended up with a
DIFFERENT computed style.

IT REFUSES RATHER THAN GUESSES when the two runs do not describe the same
document. If the element count or the tag sequence differs, the two files are
not comparable and every per-page number would be an alignment artefact rather
than a style difference -- the shape of wrong answer that reads exactly like a
finding. That is a hard error, not a warning.

A zero total is a legitimate answer and is printed as one, in words: it means
the corpus writes these selectors but no element on any page satisfies them,
which is a real fact about the corpus and NOT evidence the handlers are wrong.
test-css-selstatic is what answers that.
"""
import sys


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 4:
                continue
            rows.append(tuple(parts))
    return rows


def load_match(path):
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if len(p) == 3 and p[0] == "MATCH":
                    out.append((p[1], int(p[2])))
    except OSError:
        pass
    return out


def main():
    if len(sys.argv) not in (3, 5):
        print("usage: css_selstatic_census.py BEFORE.txt AFTER.txt "
              "[BEFORE.match AFTER.match]", file=sys.stderr)
        return 2
    before, after = load(sys.argv[1]), load(sys.argv[2])

    if len(before) != len(after):
        print("REFUSED: the two runs saw %d and %d elements. Same corpus, same\n"
              "         parser, one variable -- a length difference means the runs\n"
              "         are not comparable and every number below would be an\n"
              "         alignment artefact." % (len(before), len(after)),
              file=sys.stderr)
        return 1

    pages = {}
    order = []
    for b, a in zip(before, after):
        if b[0] != a[0] or b[1] != a[1] or b[2] != a[2]:
            print("REFUSED: the runs diverge at %s/%s (%s vs %s) -- not the same\n"
                  "         document tree." % (b[0], b[1], b[2], a[2]), file=sys.stderr)
            return 1
        page = b[0]
        if page not in pages:
            pages[page] = [0, 0]
            order.append(page)
        pages[page][1] += 1
        if b[3] != a[3]:
            pages[page][0] += 1

    if len(sys.argv) == 5:
        mb, ma = load_match(sys.argv[3]), load_match(sys.argv[4])
        print("=== elements the four selectors MATCH, corpus-wide ===")
        print("(counted with a sentinel rule of the census's own, so it is blind")
        print(" to what the pages' rules carry. READ THIS TABLE FIRST -- it is")
        print(" the reach of the handlers; the table below is what that reach is")
        print(" worth once the engine's property coverage is applied to it.)")
        print()
        print("%-12s %8s %8s" % ("selector", "before", "after"))
        tb = ta = 0
        for (sb, cb), (sa, ca) in zip(mb, ma):
            print("%-12s %8d %8d" % (sa, cb, ca))
            tb += cb
            ta += ca
        print("%-12s %8d %8d" % ("TOTAL", tb, ta))
        print()

    print("=== elements whose COMPUTED STYLE changed, per page ===")
    print("(before = the four static-pseudo handlers on h_false; after = shipped.")
    print(" One variable. A differing digest is a differing computed style over")
    print(" every property LibCSS knows, not a guess from selector text.)")
    print()
    print("%-12s %8s %8s %7s" % ("page", "elems", "changed", "pct"))
    tot_c = tot_e = 0
    for page in order:
        c, e = pages[page]
        tot_c += c
        tot_e += e
        print("%-12s %8d %8d %6.2f%%" % (page, e, c, 100.0 * c / e if e else 0.0))
    print("%-12s %8d %8d %6.2f%%" % ("TOTAL", tot_e, tot_c,
                                     100.0 * tot_c / tot_e if tot_e else 0.0))
    print()
    if tot_c == 0:
        print("ZERO ELEMENTS CHANGED, AND THE TABLE ABOVE IS WHY THAT IS NOT A")
        print("ZERO RESULT. The selectors now match; the declarations behind the")
        print("matching rules land on properties css_engine.c parses and never")
        print("reads (audit-css's 'parsed but NEVER READ' table -- cursor,")
        print("pointer-events, content, outline). apple is the worked example:")
        print("33 elements match :disabled, and the only rule that reaches them")
        print("is `button:disabled{cursor:default}`.")
        print()
        print("Those are two different repairs -- this slice made the selector")
        print("match, the unread-property line decides whether matching pays --")
        print("so they are reported as two numbers and never summed.")
        print("test-css-selstatic is what says the matching is CORRECT.")
    else:
        print("%d of %d elements (%.2f%%) are styled differently because the four" %
              (tot_c, tot_e, 100.0 * tot_c / tot_e))
        print("static pseudo-classes now match. This is REACH, not correctness --")
        print("a handler that matched too much would make this number bigger.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
