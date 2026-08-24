#!/usr/bin/env python3
"""Every tests/*.mk must be reachable from the Makefile, transitively.

WHY THIS EXISTS.  Each feature or porting line owns its own fragment and
deliberately does not touch the shared Makefile, so nobody ever adds the
include.  The fragment builds, its gates are run by hand with
"make -f tests/X.mk", and the line ships claiming them -- while
"make test-X" answers "No rule to make target", which reads as a typo in
the documentation rather than as a missing line in the Makefile.  Found
2026-08-24 with nine fragments and 27 gates in that state, two of them
advertised in commit messages that had already landed.

TWO TRAPS, BOTH HIT WHILE WRITING THIS, both of the shape CLAUDE.md warns
about -- the instrument, not the system:

  1. A fragment can be included by another FRAGMENT, not only by the
     Makefile (tests/exec.mk pulls in procfs, coredump, fdstream, fsgeom,
     poll; tests/thread.mk pulls in sched).  A checker that reads only the
     Makefile reports six reachable fragments as dead.
  2. The character class must contain a HYPHEN.  [a-z0-9_] silently drops
     tests/as-m28.mk and reports it unreachable.

And the rule this tree already writes down for anything that parses make:
join the continuations FIRST.  A '#' line is not an include.
"""
import glob, os, re, sys

# Fragments that are deliberately NOT included, with the reason.  Declared
# rather than silently skipped: a list of one with a reason is readable, and
# the tenth line to skip the include has to argue for itself here.
ALLOW_UNWIRED = {
    "schedneg": "a `make -f` wrapper that includes the Makefile itself; "
                "including it back would recurse (see its own header)",
}

def includes_of(path):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return set()
    text = re.sub(r"\\r?\n[ \t]*", " ", text)          # join continuations
    found = set()
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#"):
            continue
        m = re.match(r"-?include\s+tests/([A-Za-z0-9_-]+)\.mk", s)
        if m:
            found.add(m.group(1))
    return found

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    every = {os.path.basename(p)[:-3] for p in glob.glob("tests/*.mk")}
    seen, stack = set(), list(includes_of("Makefile"))
    while stack:
        n = stack.pop()
        if n in seen:
            continue
        seen.add(n)
        stack += [x for x in includes_of(f"tests/{n}.mk") if x not in seen]
    dead = sorted(every - seen - set(ALLOW_UNWIRED))
    if not dead:
        print(f"mk-wired: ok ({len(every)} fragments, "
              f"{len(every) - len(ALLOW_UNWIRED)} reachable from the Makefile, "
              f"{len(ALLOW_UNWIRED)} declared)")
        for k, why in sorted(ALLOW_UNWIRED.items()):
            print(f"  declared: tests/{k}.mk -- {why}")
        return 0
    gates = 0
    print(f"mk-wired: {len(dead)} of {len(every)} fragments are UNREACHABLE.")
    print("Every target below is defined and cannot be run by name:")
    for m in dead:
        body = open(f"tests/{m}.mk", encoding="utf-8", errors="replace").read()
        body = re.sub(r"\\r?\n[ \t]*", " ", body)
        tg = sorted(set(re.findall(r"^(test-[A-Za-z0-9_-]+)\s*:", body, re.M)))
        gates += len(tg)
        print(f"  tests/{m}.mk  {len(tg)} gate(s): {' '.join(tg) or '(none)'}")
    print(f"{gates} gate(s) unreachable.  Fix: add '-include tests/<name>.mk'")
    print("to the Makefile -- one line, in the file that owns the fragment's line.")
    return 1

if __name__ == "__main__":
    sys.exit(main())
