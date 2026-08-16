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

host, dev = [], []
for t in targets:
    (dev if boots(t) else host).append(t)

open(os.path.join(OUT, "host.txt"), "w").write("\n".join(host) + "\n")
open(os.path.join(OUT, "dev.txt"), "w").write("\n".join(dev) + "\n")
print("classify: %d targets -- %d host, %d device" % (len(targets), len(host), len(dev)))
