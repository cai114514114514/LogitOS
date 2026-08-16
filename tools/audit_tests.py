#!/usr/bin/env python3
"""audit_tests.py -- find tests that CANNOT FAIL.

A test that cannot fail is worse than no test, because it is counted.  This
audit looks for the three shapes of that, all three of which were found in this
tree on 2026-08-08:

  DEAD      a harness under tests/boot or tests/qmp that no Makefile target
            names, so nothing ever runs it.  (`test-durability` was a real
            five-boot durability proof that belonged to no suite.)

  MUTE      a harness that computes a verdict, prints PASS or FAIL, and then
            exits 0 either way -- so `make` sees success on a failing run.
            (`qmp_term.py` did exactly this.)

  ORPHAN    a Makefile target that exists but is reachable from no aggregate
            suite, so `make ci` would never call it even though someone wrote
            it and it works.

It is a lint, not a runner: it reads the tree, it does not boot anything, and
it is therefore cheap enough to be a prerequisite of `make ci`.

Exit status is 1 if anything is found, so it can gate.  `--list` prints the
inventory and always exits 0.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Aggregate suites: a target reachable from one of these is "wired in".
SUITES = ["ci", "ci-host", "ci-boot", "test-fs", "test-fs-host", "test-fs-boot"]

# Harnesses that legitimately have no Makefile target.  Each needs a REASON,
# and the reason has to be that another harness drives it -- not "it is slow"
# and not "it is manual", because both of those describe a dead test too.
ALLOW_DEAD = {
    # library modules imported by the harnesses, not harnesses themselves
    "tests/boot/netwire.py": "imported by run-net-*.sh harnesses, not a test",
    "tests/boot/mkreplay.py": "image builder called by run-fsreplay-test.sh",
    "tests/boot/run-ahci-mkdisk.py": "disk builder called by run-ahci-test.sh",
    "tests/boot/audio_check.py": "checker invoked by run-audio-*.sh",
    "tests/boot/secprobe.c": "guest program, not a harness",
}

# NOT GATES, and each has to say why. A file under tests/ that prints the word
# FAIL is not automatically something that can pass wrongly: a library prints it
# for its caller, a reporter prints it as DATA about the thing it measured, and
# a builder prints it about its own inputs. The point of naming them here is
# that the MUTE list is then EMPTY when the tree is healthy, so the next
# harness that computes a verdict and swallows it stands out instead of being
# the 29th line of a list nobody reads.
ALLOW_MUTE = {
    "tests/qmp/qmp_ui.py":
        "shared library (dock geometry, pointer, screendump) imported by the "
        "qmp_* harnesses -- it has no verdict of its own",
    "tests/qmp/qmp_site.py":
        "scoreboard REPORTER: per-site PAINTED/FAILED is the measurement it "
        "exists to produce, not a pass/fail for the run. The scoreboard's own "
        "header says PAINTED does not mean correct",
    "tests/qmp/sites_run.py":
        "the same, one level up: it drives qmp_site.py over a site list and "
        "writes the scoreboard files",
    "tests/boot/mkreplay.py":
        "fixture builder for test-fsreplay -- it seals an uninstalled "
        "transaction into an image; the gate that judges it is the harness "
        "that boots the result",
}


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def makefile_texts():
    """Every file that can define a target: the Makefile and tests/*.mk."""
    out = {}
    mk = os.path.join(ROOT, "Makefile")
    if os.path.exists(mk):
        out["Makefile"] = read(mk)
    d = os.path.join(ROOT, "tests")
    for n in sorted(os.listdir(d)):
        if n.endswith(".mk"):
            out["tests/" + n] = read(os.path.join(d, n))
    return out


TARGET_RE = re.compile(r"^([A-Za-z0-9_][A-Za-z0-9_.\-]*)\s*:(?!=)", re.M)


def collect_targets(texts):
    """target -> (recipe text, prerequisite list). Later definitions append."""
    targets = {}
    for _, text in texts.items():
        lines = text.split("\n")
        cur = None
        for line in lines:
            if line.startswith("\t"):
                if cur:
                    targets[cur][0].append(line)
                continue
            m = TARGET_RE.match(line)
            if m:
                name = m.group(1)
                if name in ("PHONY", ".PHONY"):
                    cur = None
                    continue
                prereq = line.split(":", 1)[1].strip()
                # a static-pattern / double-colon tail is not a prereq list
                prereq = prereq.split("#", 1)[0]
                targets.setdefault(name, ([], []))
                targets[name][1].extend(prereq.split())
                cur = name
            elif line.strip() == "":
                cur = None
    return targets


def reachable(targets, roots):
    """Targets reachable as prerequisites from `roots`."""
    seen = set()
    stack = [r for r in roots if r in targets]
    while stack:
        t = stack.pop()
        if t in seen:
            continue
        seen.add(t)
        for p in targets.get(t, ([], []))[1]:
            if p in targets and p not in seen:
                stack.append(p)
    return seen


def harnesses():
    out = []
    for sub in ("tests/boot", "tests/qmp"):
        d = os.path.join(ROOT, sub)
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            if n.startswith("__"):
                continue
            if n.endswith((".sh", ".py", ".c")):
                out.append(sub + "/" + n)
    return out


def find_dead(texts):
    """Harness files no Makefile / .mk text mentions, and harnesses that only
    other harnesses mention (which is fine only if declared in ALLOW_DEAD)."""
    blob = "\n".join(texts.values())
    dead = []
    for h in harnesses():
        base = os.path.basename(h)
        if base in blob or h in blob:
            continue
        if h in ALLOW_DEAD:
            continue
        # driven by another harness?  that is a helper, not a test -- but it
        # must say so in ALLOW_DEAD, or a genuinely dead test hides here.
        dead.append(h)
    return dead


VERDICT = re.compile(r'"?FAIL', re.I)


def find_mute(dead_files):
    """Harnesses that can print a failure and still exit 0.

    Shell: prints/echoes FAIL but has no `exit 1` and no `set -e` guard.
    Python: has a FAIL string but no sys.exit(non-zero) anywhere.
    """
    mute = []
    # A HARNESS NO TARGET RUNS CANNOT MAKE A GATE PASS WRONGLY. The dead ones
    # are already reported as dead; listing them again as mute buries the few
    # that are actually wired into something, which are the only ones that can
    # mislead anybody. What remained after this and the two exit-status fixes
    # were shared libraries (qmp_ui.py), screenshot drivers (qmp_browser.py)
    # and bug hunters whose success IS exit 0 (qmp_blackframe.py) -- none of
    # them gates, and telling those apart from a gate needs a declaration this
    # tree does not have.
    # Shell harnesses that end in `exit $something` propagate a status too; the
    # literal `exit 1` search misses them for the same reason as above.
    SH_EXIT_VAR = re.compile(r"^\s*exit\s+\"?\$", re.M)
    for h in harnesses():
        p = os.path.join(ROOT, h)
        if h.endswith(".c") or h in dead_files or h in ALLOW_MUTE:
            continue
        text = read(p)
        if not VERDICT.search(text):
            continue                       # no verdict: not a test, or silent
        if h.endswith(".py"):
            # `sys.exit(main())` and `sys.exit(rc)` propagate a status the
            # literal-integer search cannot see, and BOTH are the normal shape
            # in this tree -- qmp_desktop_look.py ends in sys.exit(main()) and
            # its main() returns 1 on a moved check, so calling it mute was a
            # false positive on a gate half of today's commits cite as evidence.
            # An audit that cries wolf is an audit people stop reading, so the
            # test is now "does a status leave this program by ANY route",
            # which is the property that was meant.
            exits_nonzero = (
                re.search(r"sys\.exit\(\s*[1-9]", text) or
                re.search(r"raise\s+SystemExit\(\s*[1-9]", text) or
                re.search(r"sys\.exit\(\s*(main|run|rc|status|ret|code)\b", text) or
                re.search(r"raise\s+SystemExit\(\s*(main|run|rc|status|ret|code)\b", text) or
                re.search(r"^\s*assert\s", text, re.M))
            if not exits_nonzero:
                mute.append((h, "prints FAIL, never sys.exit(nonzero)"))
        else:
            # A SHELL SCRIPT'S EXIT STATUS IS ITS LAST COMMAND'S, so looking
            # only for a literal `exit N` misses the two shapes this tree
            # actually uses: ending in `exec python3 ...` (the child's status
            # becomes the script's) and ending in a bare test like
            # `[ "$ok" -gt 0 ]`. Both were reported as mute, and both fail
            # correctly -- run-usb-absent-test.sh and run-vidbench-test.sh.
            # `set -e` counts too: any failing command takes the script down.
            last = ""
            for ln in reversed(text.split("\n")):
                t = ln.strip()
                if t and not t.startswith("#"):
                    last = t
                    break
            propagates = (re.search(r"exit\s+[1-9]", text) or
                          SH_EXIT_VAR.search(text) or
                          re.search(r"^\s*exec\s", text, re.M) or
                          re.search(r"^\s*set\s+-[a-z]*e", text, re.M) or
                          last.startswith("[") or last.startswith("test "))
            if not propagates:
                mute.append((h, "prints FAIL, never exits nonzero"))
    return mute


# Targets that are deliberately not part of `ci`: benchmarks (they print
# numbers, they do not assert), negative controls that are RUN BY their
# positive counterpart, and interactive/manual drivers.
NOT_CI = re.compile(
    r"^(run|shot|debug|clean|all|probe-|.*-bench$|bench-|.*-diff$|.*-profile$|"
    r"perf-|.*-manual$|test-.*-negctl$|test-.*-control$|"
    r"test-kbench.*|test-perf$|test-net-ab$|test-tcp-throughput$|"
    r"test-preview-timing$|test-.*-deep$)")


def find_orphan_targets(targets, wired):
    orphans = []
    for name in sorted(targets):
        if not name.startswith("test-"):
            continue
        if name in wired:
            continue
        if NOT_CI.match(name):
            continue
        orphans.append(name)
    return orphans


def classify(targets, name):
    """host | boot | skip -- derived from the recipe, never hand-listed.

    A hand-written list of "the suites CI runs" is the thing that rotted here:
    217 test- targets existed and no aggregate named any of them.  So the CI
    asks the Makefile instead, and a target added tomorrow is picked up without
    anyone remembering to add it.
    """
    if not name.startswith("test-"):
        return "skip"
    if NOT_CI.match(name):
        return "skip"
    recipe = "\n".join(targets[name][0])
    prereq = " ".join(targets[name][1])
    if not recipe:
        # No recipe: either an aggregate of other targets (skip -- its members
        # are already in the list, and running both doubles the wall clock) or
        # a bare declaration (skip -- there is nothing to run).
        return "skip"
    blob = recipe + " " + prereq
    if ("tests/boot/" in blob or "tests/qmp/" in blob
            or "$(ISO)" in prereq or "$(DISK)" in prereq
            or "qemu" in recipe):
        return "boot"
    return "host"


def find_duplicate_includes(texts):
    """The same fragment pulled in twice.

    Make does not warn about the include itself -- it warns about every target
    inside it ("overriding recipe for target ..."), dozens of lines of noise
    that scroll past every build, and it silently keeps the LAST definition.
    Two lines each added `-include tests/cssweb.mk` without seeing the other's,
    which is the predictable cost of the fragment convention and worth a lint
    rather than a habit."""
    dup = []
    for name, text in texts.items():
        seen = {}
        for i, line in enumerate(text.split("\n"), 1):
            m = re.match(r"^-?include\s+(\S+)\s*$", line.strip())
            if not m:
                continue
            f = m.group(1)
            if "$(" in f or "*" in f:
                continue                   # computed/globbed: not a duplicate
            if f in seen:
                dup.append((name, f, seen[f], i))
            else:
                seen[f] = i
    return dup


def main():
    texts = makefile_texts()
    targets = collect_targets(texts)
    wired = reachable(targets, SUITES)

    for want in ("host", "boot"):
        if "--suites=" + want in sys.argv:
            for n in sorted(targets):
                if classify(targets, n) == want:
                    print(n)
            return 0

    listing = "--list" in sys.argv

    dead = find_dead(texts)
    mute = find_mute(set(dead))
    orphan = find_orphan_targets(targets, wired)

    print("test audit: %d harnesses, %d make targets, %d wired into a suite"
          % (len(harnesses()), len(targets), len(wired)))

    if listing:
        for h in harnesses():
            print("  harness %s" % h)
        for t in sorted(t for t in targets if t.startswith("test-")):
            print("  target  %s%s" % (t, "" if t in wired else "   [unwired]"))
        return 0

    bad = 0
    if dead:
        print("\nDEAD -- no Makefile target names these (%d):" % len(dead))
        for h in dead:
            print("  %s" % h)
        bad += len(dead)
    if mute:
        print("\nMUTE -- computes a verdict and exits 0 anyway (%d):" % len(mute))
        for h, why in mute:
            print("  %s: %s" % (h, why))
        bad += len(mute)
    if orphan:
        print("\nUNWIRED -- a test- target no suite reaches (%d):" % len(orphan))
        for t in orphan:
            print("  %s" % t)
        bad += len(orphan)
    dup = find_duplicate_includes(texts)
    if dup:
        print("\nDUPLICATE INCLUDE -- make keeps the last one and warns about "
              "every target in it (%d):" % len(dup))
        for name, f, first, second in dup:
            print("  %s includes %s at line %d and again at line %d"
                  % (name, f, first, second))
        bad += len(dup)

    if bad:
        print("\naudit: %d finding(s)" % bad)
        return 1
    print("audit: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
