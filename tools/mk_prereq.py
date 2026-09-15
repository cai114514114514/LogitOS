#!/usr/bin/env python3
"""Every object list a recipe LINKS must also be a PREREQUISITE of that rule.

WHY THIS EXISTS, measured 2026-08-25. A prerequisite is expanded when the rule
is READ; a recipe when it is RUN. So a rule whose recipe names $(LIBM_OBJ) and
whose prerequisites do not gets handed object files make was never told to
build:

    make -pRrq | grep '^build/audiocheck.elf:' | tr ' ' '\\n' | grep -c libmobj
        -> 0
    make -n build/audiocheck.elf              | tr ' ' '\\n' | grep -c libmobj
        -> 83

It links whenever something else happened to build them first, and otherwise
fails with

    ld.lld: error: cannot open build/libmobj/third_party/libm/__cos.o

with third_party/libm sitting right there on disk -- which reads as "the build
cannot find the third-party sources" and sends the reader to the one place the
fault is not. It is order-dependent, so `make -k -j6 <everything>` does NOT see
it: across 171 targets something always builds libm first. What sees it is one
target, -j1, in a tree where nothing has run.

TWO TRAPS IN WRITING THIS CHECK, both hit, both worth stating because they are
the same shape as the bug:

  1. `make -pRrq` prints prerequisites EXPANDED and recipes UNEXPANDED. A
     checker that looks for `build/libmobj/` in both finds it in neither the
     recipe (which says `$(LIBM_OBJ)`) nor, correctly, the prerequisites --
     and reports a clean tree. So this reads the SOURCE and compares variable
     NAMES.
  2. In `make -p` output the recipe does not follow the target line directly;
     `#  Implicit rule search has not been done.` comes between. A first
     version required the recipe to be adjacent, matched an empty recipe for
     every rule, skipped all 192 of them and printed "none bad" -- a checker
     that could not fail, which is the category CLAUDE.md names MUTE.

So: this file is run against a KNOWN-BAD case before it is believed. `--selftest`
does exactly that.

Run: python3 tools/mk_prereq.py
"""
import glob
import os
import re
import sys

# Variables that name a list of object files or an archive to link. A recipe
# that mentions one and a prerequisite list that does not is the bug.
LINKY = re.compile(r"^(?:[A-Z0-9_]*_OBJ|RUST_LIB|LIBC_OBJS|LIBM_OBJ)$")

TARGET = re.compile(r"^(\$\(BUILD\)/[A-Za-z0-9_./-]+\.(?:elf|aex))\s*:\s?(.*)$")
VAR = re.compile(r"\$\(([A-Za-z0-9_]+)\)")

# Rules whose recipe legitimately names an object list it does not depend on.
# Declared with a reason, never skipped silently.
ALLOW = {
    # (target, variable): reason
}


def read_joined(path):
    """The file with continuations joined -- this tree's rule for parsing make."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        return re.sub(r"\\\r?\n[ \t]*", " ", fh.read())


def rules_of(path):
    lines = read_joined(path).split("\n")
    out = []
    for i, ln in enumerate(lines):
        m = TARGET.match(ln.rstrip("\r"))
        if not m:
            continue
        recipe = []
        for j in range(i + 1, len(lines)):
            s = lines[j].rstrip("\r")
            if s.startswith("\t"):
                recipe.append(s)
            elif s.strip() == "" or s.lstrip().startswith("#"):
                continue
            else:
                break
        out.append((path, m.group(1), m.group(2), " ".join(recipe)))
    return out


def expand(text, defs, depth=4):
    """Resolve $(NAME) one level at a time against simple := definitions.

    A prerequisite list may name an aggregate -- tests/preview.mk's
    $(PREVIEW_CLI_DEPS) holds seven object lists -- so comparing variable NAMES
    without expanding reports every one of them missing. The first version of
    this checker did exactly that and printed 17 findings of which 14 were
    false; only a rule whose prerequisites genuinely lack the list is a
    finding. Reported before it was believed, corrected here.
    """
    for _ in range(depth):
        nxt = VAR.sub(lambda m: defs.get(m.group(1), m.group(0)), text)
        if nxt == text:
            break
        text = nxt
    return text


def defs_of(paths):
    """Every `NAME := value` / `NAME = value` in the make files, joined."""
    d = {}
    NL = chr(10)
    CR = chr(13)
    pat = re.compile(r"^([A-Za-z0-9_]+)[ 	]*[:?+]?=[ 	]?(.*)$")
    for p in paths:
        for ln in read_joined(p).split(NL):
            m = pat.match(ln.rstrip(CR))
            if m and m.group(1) not in d:
                d[m.group(1)] = m.group(2)
    return d


def check(rules, defs=None):
    bad = []
    for path, tgt, prereq, recipe in rules:
        if not recipe.strip():
            continue
        rv = {v for v in VAR.findall(recipe) if LINKY.match(v)}
        pre = expand(prereq, defs or {})
        pv = {v for v in VAR.findall(prereq) if LINKY.match(v)}
        pv |= {v for v in VAR.findall(pre) if LINKY.match(v)}
        for v in sorted(rv - pv):
            if (tgt, v) in ALLOW:
                continue
            bad.append((path, tgt, v))
    return bad


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    files = ["Makefile"] + sorted(glob.glob("tests/*.mk"))
    rules = []
    for f in files:
        rules += rules_of(f)

    if "--selftest" in sys.argv:
        # The checker must flag a rule that is known to be wrong. A check that
        # has never been watched failing is indistinguishable from one that
        # cannot fail.
        fake = [("<selftest>", "$(BUILD)/x.elf", "$(AUD_OBJ)",
                 "\t$(LD) -o $@ $(AUD_OBJ) $(LIBM_OBJ)")]
        got = check(fake, {})
        ok = len(got) == 1 and got[0][2] == "LIBM_OBJ"
        print("selftest: %s (flagged %r)" % ("ok" if ok else "BROKEN", got))
        return 0 if ok else 1

    bad = check(rules, defs_of(files))
    print("mk-prereq: %d link rule(s) across %d make file(s)" % (len(rules), len(files)))
    if not bad:
        print("mk-prereq: ok -- every object list a recipe links is also a prerequisite")
        return 0
    print("mk-prereq: %d rule(s) link an object list they do not depend on." % len(bad))
    print("Each one hands ld files make was never told to build; it works only")
    print("when another target happens to build them first.")
    for path, tgt, v in bad:
        print("  %-18s %-34s recipe links $(%s), prerequisites do not" % (path, tgt, v))
    return 1


if __name__ == "__main__":
    sys.exit(main())
