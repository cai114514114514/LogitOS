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
    # The four below appeared the moment find_dead started COMPUTING
    # reachability instead of trusting a list. They were being excused as dead,
    # which is why nobody ever had to argue for them; reachable, they have to.
    # One did not survive the argument -- see the gate now in tests/sweep.mk.
    "tests/boot/sweep-classify.py":
        "classifier: it partitions targets into host / device / needs-arguments "
        "and writes three files. It reaches no verdict -- FAIL appears as one "
        "of the statuses its comments explain, not one it decides",
    "tests/boot/sweep-confirm.sh":
        "reporter: it re-runs each non-PASS target alone and records the second "
        "answer per target. The GATE over its result file is the test-sweep / "
        "test-sweep-confirm make target -- which exists because this audit "
        "found that there was none and the sweep could not fail",
    "tests/boot/bootwait.sh":
        "waiter: it blocks until a serial line appears and says which outcome "
        "it saw. Every caller tests for the line itself, because 'the boot "
        "printed X' is the caller's assertion, not this file's",
    "tests/qmp/qmp_browser.py":
        "shared driver imported by qmp_site.py, qmp_browser_https.py, "
        "qmp_ps2_only.py and run-usb-absent-test.sh -- it drives a browser and "
        "prints 'done'; each importer owns its own verdict",
    # And two more, surfaced the same way one layer later: giving the lock
    # instruments make targets made them reachable, which brought their verdicts
    # into scope. Third time this has happened in one pass -- deadness hides
    # muteness, so every reachability gain is also a muteness disclosure.
    "tests/boot/run-smp-lockprobe.sh":
        "probe: it prints every lock's ticket counter across a workload. There "
        "is no threshold to assert -- the numbers are the input to a decision, "
        "and the one that mattered (kheap_lock 30.7 M against the BKL's 36,836) "
        "was a ratio nobody had predicted, which is why it is measured and not "
        "bounded",
    "tests/boot/sweep-resume.sh":
        "driver: it runs the targets a sweep has no result for and chains into "
        "sweep-confirm.sh. The verdict over the finished result file belongs to "
        "test-sweep-confirm, which is where the gate lives",
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
        # JOIN BACKSLASH CONTINUATIONS FIRST. A prerequisite list that wraps --
        #
        #     test-fs-boot: test-fsmount test-durability test-fscrash \
        #                   test-fsreplay test-hugefile test-barrier
        #
        # loses everything after the backslash, because the split below takes
        # one physical line. test-fs-boot is one of this file's own SUITES
        # roots, so its wrapped members were counted UNWIRED while being wired,
        # and the UNWIRED number this audit exists to report was inflated by it.
        #
        # Fourth instance in one day of a make construct that spans physical
        # lines being read one line at a time: it also produced a wrong md5
        # that let a real Makefile breakage through, a false MISSING in a shell
        # check, and a variant binary that tools/negctl_drift.py could not see.
        text = re.sub(r"\\\n[ \t]*", " ", text)
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
    """Harness files nothing can reach.

    A harness is alive if a Makefile names it, OR if a harness that is itself
    alive names it -- COMPUTED, not declared.

    ALLOW_DEAD says in as many words that the only acceptable reason for a
    harness to have no target is "another harness drives it". That is a
    checkable property, so it is checked. Trusting a hand-written list for it
    cost real coverage: qmp_lockdump.py, sweep-classify.py,
    run-smp-lockprobe.sh and run-smp-freeze-probe.sh were all reported dead
    while being driven every run, and the only remedy on offer was to add four
    more lines to a list -- which is how the list becomes the place a genuinely
    dead test hides.

    TRANSITIVE FROM A MAKEFILE ROOT, and that word is the whole design. "Some
    other harness mentions it" would let two dead scripts that reference each
    other excuse one another forever; a walk outward from the targets cannot,
    because nothing outside the reachable set can add to it.
    """
    blob = "\n".join(texts.values())
    hs = harnesses()

    alive = set(h for h in hs if os.path.basename(h) in blob or h in blob)
    body = {}
    for h in hs:
        try:
            body[h] = read(os.path.join(ROOT, h))
        except OSError:
            body[h] = ""

    grew = True
    while grew:                       # transitive closure, roots outward
        grew = False
        for h in hs:
            if h in alive:
                continue
            for a_ in alive:
                if os.path.basename(h) in body[a_]:
                    alive.add(h)
                    grew = True
                    break

    return [h for h in hs if h not in alive and h not in ALLOW_DEAD]


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


def find_stranded_controls(targets):
    """A *-negctl that NOT_CI drops from CI and that NOTHING invokes.

    NOT_CI excludes every `test-*-negctl` from the list tools/ci.sh runs, and
    the reason it gives is an assumption about the Makefile: a control is "RUN
    BY its positive counterpart".  Nothing checked it.  A control that is a
    separate target and is named by nobody is excluded by the regex AND
    invoked from nowhere -- run never, while looking exactly like a control
    that is covered.  Six were in that state when this check was written, four
    of them added the same day, and it is invisible from either end: from
    ci.sh it is "deliberately not in CI", from the Makefile it is "a target
    somebody surely runs".

    This is the same shape as the MUTE category one level up -- a thing that
    reads like a gate and cannot fail -- and it belongs beside it for the same
    reason: the value of the category is being empty.

    Invoked = named as a prerequisite of any target, or mentioned in any
    recipe (a `$(MAKE) test-x-negctl` line counts).  Deliberately generous:
    the finding is "nobody could possibly reach this", not "it is not reached
    the way I would have written it".
    """
    named = set()
    for name, (recipe, prereqs) in targets.items():
        for pr in prereqs:
            named.add(pr)
        blob = "\n".join(recipe)
        for other in targets:
            if other != name and other in blob:
                named.add(other)
    return [n for n in sorted(targets)
            if n.endswith("-negctl") and NOT_CI.match(n) and n not in named]


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
    # STRANDED IS ONE FACT TOO, for exactly the reason UNWIRED is (see below):
    # there are 55, and 55 findings would bury DEAD and MUTE, whose whole value
    # is being zero. So the same shape -- recorded by name, gating on GROWTH --
    # and the same warning: zeroing it would be worse than carrying it, because
    # "55 negative controls in this tree are run by nobody" is TRUE.
    stranded = find_stranded_controls(targets)
    st_path = os.path.join(ROOT, "tests", "audit-stranded.baseline")
    if "--bless-stranded" in sys.argv:
        with open(st_path, "w", encoding="utf-8") as fh:
            fh.write("# Negative controls that NOT_CI drops from CI and that no\n"
                     "# target invokes -- run NEVER. See STRANDED CONTROLS in\n"
                     "# tools/audit_tests.py: recorded debt, gated on GROWTH.\n"
                     "# The fix for one entry is a single line: make it a\n"
                     "# prerequisite of its positive counterpart.\n")
            fh.write("\n".join(sorted(stranded)) + "\n")
        print("blessed %d stranded control(s) into %s" % (len(stranded), st_path))
        return 0
    st_known = set()
    try:
        with open(st_path, encoding="utf-8") as fh:
            st_known = set(ln.strip() for ln in fh
                           if ln.strip() and not ln.startswith("#"))
    except OSError:
        pass
    st_fresh = sorted(set(stranded) - st_known)
    if stranded:
        print("\nSTRANDED CONTROLS -- %d dropped from CI by NOT_CI and invoked by"
              % len(stranded))
        print("nobody, so run NEVER; %d recorded as debt. The fix is one line:"
              % (len(stranded) - len(st_fresh)))
        print("make it a PREREQUISITE of its positive. Naming it on a")
        print("ci-host:/ci-boot: line satisfies UNWIRED and still runs it never,")
        print("which is worse, because it looks fixed.")
    if st_fresh:
        print("STRANDED (NEW -- one line, or bless it deliberately) (%d):"
              % len(st_fresh))
        for t in st_fresh:
            print("  %s" % t)
        bad += len(st_fresh)
    # UNWIRED IS ONE FACT, NOT 354 FINDINGS, AND IT GATES ON GROWTH.
    #
    # Reported as 354 individual findings it made this audit permanently red,
    # which costs the other two categories their gate: DEAD and MUTE are exactly
    # the checks whose value is being zero, and they are unreadable underneath a
    # list nobody can act on in one sitting. But zeroing UNWIRED would be worse
    # -- "ci runs 22 of 588 targets" is TRUE, and the tree should not stop
    # saying it just because saying it is inconvenient.
    #
    # So the debt is recorded, by NAME, in tests/audit-unwired.baseline, and the
    # gate is that the set does not grow. A new target that no suite reaches is
    # caught the day it is written, which is the only day it is cheap to wire.
    # Recording the SET and not the count is deliberate: a count lets one target
    # get wired while another is unwired, netting zero and hiding both.
    #
    # Refresh with `python3 tools/audit_tests.py --bless-unwired`, which is a
    # separate deliberate act, and read the diff before committing it.
    base_path = os.path.join(ROOT, "tests", "audit-unwired.baseline")
    if "--bless-unwired" in sys.argv:
        with open(base_path, "w", encoding="utf-8") as fh:
            fh.write("# Targets no aggregate suite reaches. See UNWIRED in\n"
                     "# tools/audit_tests.py: this is recorded debt, and the\n"
                     "# gate is that it does not GROW.\n")
            fh.write("\n".join(sorted(orphan)) + "\n")
        print("blessed %d unwired target(s) into %s" % (len(orphan), base_path))
        return 0
    known = set()
    try:
        with open(base_path, encoding="utf-8") as fh:
            known = set(ln.strip() for ln in fh
                        if ln.strip() and not ln.startswith("#"))
    except OSError:
        pass
    fresh = sorted(set(orphan) - known)
    if orphan:
        print("\nUNWIRED -- %d target(s) no suite reaches; %d recorded as debt"
              % (len(orphan), len(orphan) - len(fresh)))
    if fresh:
        print("UNWIRED (NEW -- wire it, or bless it deliberately) (%d):"
              % len(fresh))
        for t in fresh:
            print("  %s" % t)
        bad += len(fresh)
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
