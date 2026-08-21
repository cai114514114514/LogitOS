#!/usr/bin/env python3
"""Rank WHY css/ files never complete the WPT harness.

The failure list is thousands of lines and is not a work order. `wpt_rank.py`
groups FAILING SUBTESTS by cause; this groups the strictly worse category --
files that died before reporting anything, so none of their subtests are in
the denominator at all.

Input is the `--report` TSV tests/unit/wpt_test.c writes:

    HARNESS <path> [HARNESS] <why> <stack>

Two passes, because the biggest bucket is not self-describing. "the harness
completed with zero subtests" says nothing about the cause, so those files are
opened and classified by what they were WAITING for -- a body onload handler,
a support script's setup(), a window load listener.

    python3 tools/cssom_rank.py build/cssom/before.tsv [--wpt-root build/wpt]
"""
import collections
import os
import re
import sys


def read_rows(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            fields = line.rstrip("\n").split("\t")
            # HARNESS = started and threw. NOTSTARTED = loaded, testharness
            # installed, and no test() was ever registered. Two labels, one
            # category for this table's purpose: the file contributed nothing
            # to the denominator. Reading only HARNESS hides the single
            # largest population in the corpus (<body onload>), which is the
            # mistake this table exists to correct.
            if len(fields) >= 4 and fields[0] in ("HARNESS", "NOTSTARTED"):
                rows.append((fields[1], fields[3]))
    return rows


# An identifier that is not defined is almost never a missing API: it is
# `<div id="container">` and a script saying `container.style...`, i.e. named
# access on the Window object. Distinguishing the two matters, because one is a
# single HTML feature worth ~150 files and the other is 150 separate APIs.
KNOWN_APIS = {
    "CSS", "CSSKeywordValue", "CSSStyleValue", "CSSUnitValue", "CSSMathSum",
    "FontFace", "BroadcastChannel", "Range", "Highlight", "DOMRect", "DOMPoint",
    "DOMMatrix", "getSelection", "HTML5_ELEMENTS", "ResizeObserver",
    "IntersectionObserver", "CSSStyleSheet", "MediaQueryList", "Worker",
    "OffscreenCanvas", "SVGElement", "CustomElementRegistry", "requestIdleCallback",
}


def classify(why):
    if "referenced script" in why:
        return "support script missing from the vendored checkout"
    if "zero subtests" in why or "registered no tests" in why:
        return "ZERO SUBTESTS -- harness completed, no test() ever registered"
    if why.startswith("watchdog"):
        return "watchdog -- script never stopped running"
    if "testharness.js did not install" in why:
        return "testharness.js did not install"
    if "harness never completed" in why:
        return "no exception, but the harness never completed (async never resolved)"

    # `<inline 1>` has a space in it, so the location is non-greedy up to
    # the ": <Kind>Error:" that follows. A \S+ here silently matched
    # nothing and left half the table unaggregated.
    m = re.match(r"uncaught in (.+?): (\w*Error): (.*)", why)
    if not m:
        return why[:90]
    kind, msg = m.group(2), m.group(3)

    r = re.match(r"'([^']+)' is not defined", msg)
    if r:
        name = r.group(1)
        if name in KNOWN_APIS:
            return "ReferenceError: %s is not defined (missing API)" % name
        return ("ReferenceError: an element id used as a global "
                "(named access on the Window object)")
    r = re.match(r"cannot read property '([^']+)' of (\w+)", msg)
    if r:
        return "TypeError: cannot read property '%s' of %s" % (r.group(1), r.group(2))
    if kind == "SyntaxError":
        return "SyntaxError: %s (QuickJS could not parse the script)" % msg[:40]
    return "%s: %s" % (kind, msg[:70])


ZERO_PATTERNS = [
    (re.compile(r"<body[^>]*\bonload\s*=", re.I),
     "ZERO: <body onload=...> -- a WINDOW handler, never reflected"),
    (re.compile(r"\bonload\s*=", re.I),
     "ZERO: some other onload= content attribute"),
    (re.compile(r"(window\.onload|addEventListener\(\s*[\"']load)"),
     "ZERO: a window load listener that registered no test()"),
]


def classify_zero(path, root):
    try:
        with open(os.path.join(root, path), encoding="utf-8", errors="replace") as f:
            src = f.read()
    except OSError:
        return "ZERO: file unreadable"
    support = sorted({os.path.basename(m.group(1))
                      for m in re.finditer(r"<script[^>]*src=[\"']?([^\"'>\s]+)", src, re.I)}
                     - {"testharness.js", "testharnessreport.js"})
    for rx, label in ZERO_PATTERNS:
        if rx.search(src):
            return "%s [%s]" % (label, ",".join(support[:2]) or "no support js")
    return "ZERO: no load hook at all [%s]" % (",".join(support[:2]) or "no support js")


def main():
    args = [a for a in sys.argv[1:]]
    root = "build/wpt"
    if "--wpt-root" in args:
        i = args.index("--wpt-root")
        root = args[i + 1]
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 2
    rows = read_rows(args[0])

    counts = collections.Counter()
    examples = collections.defaultdict(list)
    for path, why in rows:
        key = classify(why)
        if key.startswith("ZERO SUBTESTS"):
            key = classify_zero(path, root)
        counts[key] += 1
        if len(examples[key]) < 2:
            examples[key].append(path)

    print("css/ files that never completed the WPT harness: %d" % len(rows))
    print()
    print("%6s  %s" % ("files", "cause"))
    print("%6s  %s" % ("-" * 6, "-" * 66))
    for key, n in counts.most_common(40):
        print("%6d  %s" % (n, key))
        for e in examples[key][:1]:
            print("%6s  %s" % ("", "e.g. " + e))
    tail = sum(n for _, n in counts.most_common()[40:])
    if tail:
        print("%6d  (%d further causes, one or two files each)"
              % (tail, len(counts) - 40))
    return 0


if __name__ == "__main__":
    sys.exit(main())
