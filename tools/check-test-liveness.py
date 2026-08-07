#!/usr/bin/env python3
"""Find tests that cannot fail.

    python3 tools/check-test-liveness.py [--list]

The deepest item on the day's damage report was not that tests failed. It was
that tests PASSED while testing nothing:

  * tests/qmp/qmp_freeze.py printed two numbers and exited 0 whatever they were.
  * a fixture wrote the wrong size for weeks, so make test-fscrash only ever
    exercised its "the file was absent" branch -- the branch it exists to check
    had never once run.
  * five drivers "click the address bar" at a coordinate that has been inside
    the window-manager titlebar for some time; they pass because the browser
    happens to start with that field focused, so the click is decoration.

A test with no failing path is the machine-checkable end of that family, and it
is the one this tool enforces. Three rules, in descending confidence:

  1  NO-FAIL   the script has no path that exits non-zero (and no raise/assert).
               Nothing it observes can change the outcome. This FAILS the build.
  2  UNREFERENCED  nothing in the Makefile, tests/*.mk or any other script runs
               it. It is not a test, it is a file. Reported as a warning.
  3  RAW-COORD  a QMP driver that aims the pointer at a hardcoded pixel pair
               instead of going through tests/qmp/qmp_ui.py's geometry helpers.
               Those coordinates are a function of the display mode, the app
               count and the window cascade, and every one of the three has
               changed under a driver already. Reported as a warning.
  4  ONLY-EMULATOR  the script's only non-zero exit is "QEMU died". It can
               report that the emulator crashed and nothing about the guest --
               which is qmp_freeze.py's exact shape, and the reason it passed on
               a broken build and a working one alike. Reported as a warning,
               because telling this apart from a real check needs judgement.

What no static rule catches -- and what the fscrash fixture did -- is a test
whose failing path is real but never REACHED: its own fixture drifted, so only
the easy branch ever ran. The tool cannot see that. What it can do is stop the
easiest version of it from being added again.

Everything rule 1 finds must be listed in tools/test-liveness-allow.txt with a
reason, or this exits 1. That file is the honest form of "known and accepted":
adding a line is a decision somebody made, whereas a warning nobody reads is
not. Warnings never fail the build -- they are a map, not a gate.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALLOW = os.path.join(ROOT, "tools", "test-liveness-allow.txt")

# What counts as "this script has a way to report failure".
#
# Being generous here is deliberate. A false positive -- flagging a test that
# can in fact fail -- costs an allowlist entry that is a lie, and a checker
# whose output you learn to dismiss is worse than no checker. So every ordinary
# way of propagating a non-zero status counts: an explicit `exit 3`, `exit $rc`,
# `set -e`, a `fail()` helper, `exec`ing the real harness (its status becomes
# yours), and a trailing test expression. What is left over is the genuine
# article: a script that runs a machine, prints what it saw, and returns 0.
SH_FAIL = re.compile(
    r"(^|[;&|\s(])exit\s+[1-9]"          # exit 1
    r"|(^|[;&|\s(])exit\s+[\"']?\$"      # exit $rc / exit "$?"
    r"|\bset\s+-[a-z]*e"                 # set -e / set -eu
    r"|\bfail\s*\(\s*\)"                 # a fail() helper
    r"|\bdie\s*\(\s*\)"
    r"|(^|\s)exec\s+"                    # exec python3 harness.py  (status passes through)
    r"|\|\|\s*exit"
    r"|^\s*\[\s.*\]\s*$",                # a trailing test expression IS the verdict
    re.M)
PY_FAIL = re.compile(
    r"sys\.exit\(\s*[1-9]"
    r"|sys\.exit\(\s*(main|run|[a-z_]*\()"   # sys.exit(main(...)) -- main returns a code
    r"|os\._exit\(\s*[1-9]"
    r"|\braise\b|\bassert\b"
    r"|\bdie\s*\("
    r"|^\s*return\s+[1-9]"                   # a checker function's non-zero return
    r"|exit\(\s*[1-9]",
    re.M)
# A driver aiming at literal device pixels rather than qmp_ui's geometry.
RAW_COORD = re.compile(r"\b(?:goto|click_at|click_at_confirmed)\s*\(\s*"
                       r"(?:[a-z_]+\s*,\s*)?\d{2,}\s*,\s*\d{2,}")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def collect():
    tests = []
    for d, exts in (("tests/boot", (".sh",)), ("tests/qmp", (".py",)),
                    ("tests/unit", (".sh",))):
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if name.endswith(exts) and not name.startswith("_"):
                tests.append(os.path.join(d, name).replace("\\", "/"))
    return tests


def referrers(tests):
    """Which files mention each test. A test nothing runs is not a test."""
    pool = []
    for base, _dirs, files in os.walk(ROOT):
        if any(p in base for p in (os.sep + ".git", os.sep + "build",
                                   os.sep + "third_party", os.sep + ".cache")):
            continue
        for f in files:
            if f.endswith((".sh", ".py", ".mk")) or f == "Makefile":
                pool.append(os.path.join(base, f))
    text = {p: read(p) for p in pool}
    out = {}
    for t in tests:
        base = os.path.basename(t)
        refs = [os.path.relpath(p, ROOT).replace("\\", "/")
                for p, body in text.items()
                if base in body and os.path.relpath(p, ROOT).replace("\\", "/") != t]
        out[t] = refs
    return out


def allowlist():
    allowed = {}
    if not os.path.exists(ALLOW):
        return allowed
    for line in read(ALLOW).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        path, _, why = line.partition(":")
        allowed[path.strip()] = why.strip()
    return allowed


def main():
    tests = collect()
    refs = referrers(tests)
    allowed = allowlist()

    nofail, unref, rawcoord, onlyemu = [], [], [], []
    for t in tests:
        body = read(os.path.join(ROOT, t))
        lines = body.splitlines()
        pat = PY_FAIL if t.endswith(".py") else SH_FAIL
        hits = [i for i, ln in enumerate(lines) if pat.search(ln)]
        if not hits:
            nofail.append(t)
        elif all(any(k in " ".join(lines[max(0, i - 2):i + 2]).lower()
                     for k in ("qemu_died", "qemu exited", "poll() is not none",
                               "qemu.poll", "proc.poll", "kill -0"))
                 for i in hits):
            onlyemu.append(t)
        if not refs[t]:
            unref.append(t)
        if t.endswith(".py"):
            coords = RAW_COORD.findall(body)
            if coords:
                rawcoord.append((t, len(coords)))

    print("test-liveness: %d test scripts examined\n" % len(tests))

    print("[1] tests with NO FAILING PATH -- they cannot report anything:")
    unexpected = []
    if not nofail:
        print("    (none)")
    for t in nofail:
        if t in allowed:
            print("    allowed  %-34s %s" % (t, allowed[t]))
        else:
            print("    FAIL     %s" % t)
            unexpected.append(t)
    for t in allowed:
        if t not in nofail:
            print("    stale    %-34s (allowlisted, but it can fail now -- "
                  "drop the line)" % t)

    print("\n[2] tests NOTHING RUNS (no Makefile/target/script mentions them):")
    for t in unref:
        print("    warn     %s" % t)
    if not unref:
        print("    (none)")

    print("\n[3] QMP drivers aiming at HARDCODED device pixels rather than "
          "tests/qmp/qmp_ui.py geometry:")
    for t, n in rawcoord:
        print("    warn     %-34s %d site(s)" % (t, n))
    if not rawcoord:
        print("    (none)")

    print("\n[4] tests whose ONLY failing path is 'the emulator died' -- they "
          "cannot report anything\n    about the GUEST:")
    for t in onlyemu:
        print("    warn     %s" % t)
    if not onlyemu:
        print("    (none)")

    print("\nRules 2, 3 and 4 are warnings: they are a map of where the next "
          "dead test will come\nfrom, not a gate. Rule 1 is a gate.")
    if unexpected:
        print("\nFAIL: %d test script(s) have no failing path and are not in %s"
              % (len(unexpected), os.path.relpath(ALLOW, ROOT)))
        return 1
    print("\nPASS: every test script can fail, or is listed with a reason")
    return 0


if __name__ == "__main__":
    sys.exit(main())
