#!/usr/bin/env python3
"""Split every test target into "boots QEMU" and "does not", from ONE make run.

The obvious way -- `make -n <target>` per target and look for qemu -- is
correct and takes an hour and a half here: this Makefile has 524 test targets
and enough $(shell ...) in it that evaluating it 524 times dominates the sweep
it was meant to plan. `make -pRrq` prints the whole database, recipes included,
in one pass.

A recipe rarely says "qemu" itself; it says `bash tests/boot/run-X.sh`, and the
qemu is in there. So a target is a DEVICE target when its own recipe mentions
qemu, or when it invokes a script that does, or when it depends on a target
that does -- resolved transitively, because `test-h265-b: $(BUILD)/h265_test`
is a host target whose prerequisite is not.

Usage: sweep-classify.py <outdir>
Writes host.txt, dev.txt.
"""
import os
import re
import subprocess
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/sweep/sweeplogs"
os.makedirs(OUT, exist_ok=True)

db = subprocess.run(["make", "-pRrq"], capture_output=True, text=True,
                    errors="replace").stdout

# --- parse "target: prereqs" + the tab-indented recipe that follows ---------
recipes, prereqs = {}, {}
cur = None
for ln in db.split("\n"):
    if ln.startswith("\t"):
        if cur:
            recipes.setdefault(cur, []).append(ln)
        continue
    m = re.match(r"^([A-Za-z0-9_./$()%-]+)\s*:{1,2}\s*(.*)$", ln)
    if m and not ln.startswith("#") and "=" not in m.group(1):
        cur = m.group(1)
        prereqs.setdefault(cur, m.group(2).split())
    elif not ln.strip():
        cur = None

# --- does a shell script reach qemu? ---------------------------------------
script_cache = {}


def script_boots(path):
    if path in script_cache:
        return script_cache[path]
    ok = False
    if os.path.exists(path):
        try:
            ok = "qemu" in open(path, "r", errors="replace").read()
        except OSError:
            ok = False
    script_cache[path] = ok
    return ok


def recipe_boots(t):
    for ln in recipes.get(t, []):
        if "qemu" in ln.lower():
            return True
        for m in re.findall(r"tests/[A-Za-z0-9_./-]+\.sh", ln):
            if script_boots(m):
                return True
        # $(QEMU...) variables expand to a qemu command line
        if "$(QEMU" in ln:
            return True
    return False


DEVICE_ARTIFACTS = ("$(ISO)", "$(DISK)", "build/logit.iso", "build/disk.img")

seen = {}


def boots(t, depth=0):
    if t in seen:
        return seen[t]
    if depth > 6:
        return False
    seen[t] = False                       # cycle guard: assume host until proven
    r = recipe_boots(t)
    if not r:
        for p in prereqs.get(t, []):
            if p in DEVICE_ARTIFACTS:
                r = True
                break
            if p.startswith("test-") and boots(p, depth + 1):
                r = True
                break
    seen[t] = r
    return r


targets = sorted(t for t in recipes if t.startswith("test-"))
targets += sorted(t for t in prereqs
                  if t.startswith("test-") and t not in recipes)
# Drop what is not a real target name. `make -p` prints pattern rules and
# unexpanded $(1) template bodies too, and a sweep that runs `make
# test-as-$(1)-negctl` reports a failure that is a parsing artifact of this
# file rather than anything about the tree.
targets = sorted(set(t for t in targets
                     if "$" not in t and "%" not in t and t.startswith("test-")))

# TARGETS THAT CANNOT BE RUN BARE, each with the reason it is here.
#
# A target that prints its usage and exits nonzero is not a failing target -- it
# is a target that was never callable without an argument, and recording it as
# FAIL puts an entry in the list that no amount of investigation will resolve.
# The sweep exists to find real breakage; two permanent false entries are two
# reasons to stop reading the list.
#
# An explicit table, not a heuristic. "exited nonzero after printing something
# that looks like a usage line" would also match a real failure whose error
# message happens to contain the word usage, and a rule that silently
# reclassifies a genuine break is far worse than a table somebody has to extend.
# Extending it is one line and requires naming the argument.
NEEDS_ARGS = {
    "test-net-ab":    "BEFORE=<other.iso> -- an A/B comparison needs the other side",
    "test-perf-gate": "PERF_METRIC=<name> -- a gate on one metric needs the metric",
    # Not an argument but the same category: a target that cannot be run as it
    # stands. The Makefile says so in capitals above the target -- "REQUIRES A
    # CHURN BUILD: make CHURN=1 && make CHURN=1 build/disk.img && make
    # test-leak-os" -- because the driver is compiled into the WM and the flag
    # is not object-tracked, so a normal build simply does not contain it.
    #
    # Deliberately in THIS table rather than a second one. The sweep's question
    # is "can this target be run right now", and splitting the no by its reason
    # would give two lists to check instead of one.
    "test-leak-os":   "CHURN=1, at BUILD time -- the app-churn driver is compiled "
                      "into the WM and a normal build does not contain it",
}

def aggregate(t, known):
    """A target whose whole job is to run other targets this sweep already runs.

    THREE OF THE SWEEP'S FOUR TIMEOUTS WERE THESE. test-fs is
    `test-fs-host test-fs-boot`, and test-fs-boot is six boot targets; it was
    killed at 900 s in the middle of test-fscrash's third crash round. The
    per-target timeout cannot be right for a target that is fifteen targets.

    Raising the timeout for them would be the wrong fix, because running them at
    all is pure duplication: every member is already in this sweep, run
    individually, with its own log and its own verdict. The aggregate adds no
    coverage and costs the sum of its parts a second time.

    The test is deliberately narrow -- EVERY prerequisite must itself be a test
    target the sweep will run, and the recipe must do nothing but announce. A
    target that also checks something (compares two runs, sums a score) is NOT
    an aggregate no matter how many test- prerequisites it has, and gets run.
    """
    pre = prereqs.get(t, [])
    if not pre or not all(p in known for p in pre):
        return False
    for ln in recipes.get(t, []):
        body = ln.strip().lstrip("@-")
        if body and not body.startswith(("echo ", "true", ":")):
            return False
    return True


host, dev, args, agg = [], [], [], []
known = set(targets)
for t in targets:
    if t in NEEDS_ARGS:
        args.append("%s\t%s" % (t, NEEDS_ARGS[t]))
        continue
    if aggregate(t, known):
        agg.append("%s\t%s" % (t, " ".join(prereqs.get(t, []))))
        continue
    (dev if boots(t) else host).append(t)

open(os.path.join(OUT, "host.txt"), "w").write("\n".join(host) + "\n")
open(os.path.join(OUT, "dev.txt"), "w").write("\n".join(dev) + "\n")
open(os.path.join(OUT, "args.txt"), "w").write("\n".join(args) + ("\n" if args else ""))
open(os.path.join(OUT, "agg.txt"), "w").write("\n".join(agg) + ("\n" if agg else ""))
print("classify: %d targets -- %d host, %d device, %d need arguments, "
      "%d aggregates (members run individually)"
      % (len(targets), len(host), len(dev), len(args), len(agg)))
