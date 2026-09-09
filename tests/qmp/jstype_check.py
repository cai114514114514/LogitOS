#!/usr/bin/env python3
"""Run the type census in the guest and judge it against a baseline ratchet.

    python3 tests/qmp/jstype_check.py --iso build/logit.iso --disk build/disk.img \
        --baseline tests/fixtures/jstype/BASELINE
    python3 tests/qmp/jstype_check.py ... --negctl
    python3 tests/qmp/jstype_check.py ... --write        # re-cut the baseline

WHAT IS JUDGED, AND WHY IT IS A RATCHET RATHER THAN A COUNT
===========================================================
The census (tests/fixtures/jstype/typecensus.html) reports one FIND line per
member whose TYPE cannot be right in any browser. A count would be satisfied by
fixing one member and breaking another; a ratchet against the committed list is
not. The rule is the one this tree already uses for html5lib and WPT: a finding
that is in the baseline is debt, a finding that is not is a regression, and a
baseline entry that stops firing is progress and must be removed in the same
commit that earns it.

THE FIRST THING CHECKED IS THAT THE CENSUS RAN AT ALL
=====================================================
`0 findings` and `the page never executed` are the same output, and this tree
has a scar for exactly that: `test-url` reported 32/32 (100.0%) while printing
that both corpora were absent. So the summary line is required, and it must
account for a plausible surface -- a browser that published four interfaces
would otherwise sail through with nothing to report.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(os.path.dirname(HERE), "fixtures", "jstype", "typecensus.html")

# A surface smaller than this means the census did not see the browser it was
# pointed at. Chosen well under the measured 209/2920 so an ordinary change
# cannot trip it, and well over anything a broken run could produce.
MIN_IFACES, MIN_MEMBERS = 80, 1200

# The three members ?negctl installs, one per rule. Named here so the control
# fails loudly if the census stops reporting any ONE of the three shapes --
# a control that only proves P1 still works is not a control for P2 and P3.
NEGCTL_EXPECT = [
    "FIND P1 Element.prototype.negctlNumberOp",
    "FIND P2 Element.prototype.__negctlInternal",
    "FIND P3 Element.prototype.negctlEnumerableOp",
]


def run_guest(iso, disk, query, out_dir):
    out = os.path.join(out_dir, "census.json")
    ser = os.path.join(out_dir, "census.serial.txt")
    cmd = [sys.executable, os.path.join(HERE, "qmp_replay.py"),
           "--iso", iso, "--disk", disk, "--page", PAGE,
           "--name", "typecensus", "--out", out, "--serial", ser]
    if query:
        cmd += ["--query", query]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    with open(out, encoding="utf-8") as fh:
        rec = json.load(fh)
    if rec.get("verdict") != "REPLAYED":
        print("test-jstype: HARNESS -- %s" % rec.get("why"))
        sys.exit(2)
    with open(ser, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def parse(log):
    finds, summary = [], None
    for ln in log.splitlines():
        i = ln.find("FIND P")
        if i >= 0:
            finds.append(ln[i:].strip())
        j = ln.find("TYPECENSUS-SUM ")
        if j >= 0:
            summary = ln[j:].strip()
    return finds, summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/logit.iso")
    ap.add_argument("--disk", default="build/disk.img")
    ap.add_argument("--baseline", default=os.path.join(
        os.path.dirname(HERE), "fixtures", "jstype", "BASELINE"))
    ap.add_argument("--negctl", action="store_true")
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="jstype_")
    log = run_guest(args.iso, args.disk, "negctl" if args.negctl else "", tmp)
    finds, summary = parse(log)

    if not summary:
        print("test-jstype: the census never printed its summary -- it did not "
              "run. Nothing was measured; a finding count of %d is not a result."
              % len(finds))
        sys.exit(2)
    m = re.search(r"ifaces=(\d+) members=(\d+)", summary)
    if not m or int(m.group(1)) < MIN_IFACES or int(m.group(2)) < MIN_MEMBERS:
        print("test-jstype: %s -- fewer than %d interfaces / %d members reached. "
              "The census ran against something that is not this browser."
              % (summary, MIN_IFACES, MIN_MEMBERS))
        sys.exit(2)
    print(summary)

    if args.negctl:
        missing = [e for e in NEGCTL_EXPECT
                   if not any(f.startswith(e) for f in finds)]
        if missing:
            print("test-jstype-negctl: FAILED -- the census did not report %d of "
                  "the %d members ?negctl deliberately installed:"
                  % (len(missing), len(NEGCTL_EXPECT)))
            for e in missing:
                print("   never reported: " + e)
            sys.exit(1)
        print("test-jstype-negctl: ok -- all %d injected members were named "
              "(P1 wrong type, P2 internal name, P3 enumerable)"
              % len(NEGCTL_EXPECT))
        return 0

    if args.write:
        with open(args.baseline, "w", encoding="utf-8") as fh:
            fh.write("# tests/fixtures/jstype/BASELINE -- the type findings this\n"
                     "# browser currently has. A RATCHET, not a score: a line here\n"
                     "# is debt, a line NOT here is a regression, and a line that\n"
                     "# stops firing must be deleted in the commit that earns it.\n"
                     "# Re-cut with: make test-jstype-write\n")
            fh.write("# %s\n" % summary)
            for f in sorted(finds):
                fh.write(f + "\n")
        print("test-jstype: baseline written, %d finding(s)" % len(finds))
        return 0

    base = set()
    try:
        with open(args.baseline, encoding="utf-8") as fh:
            for ln in fh:
                ln = ln.strip()
                if ln and not ln.startswith("#"):
                    base.add(ln)
    except OSError:
        print("test-jstype: no baseline at %s -- run `make test-jstype-write` "
              "once and read what it wrote before committing it." % args.baseline)
        sys.exit(2)

    now = set(finds)
    new = sorted(now - base)
    gone = sorted(base - now)
    for f in gone:
        print("  FIXED (delete from the baseline): " + f)
    if new:
        print("test-jstype: FAILED -- %d NEW type finding(s):" % len(new))
        for f in new:
            print("   " + f)
        print("\nA member with the wrong TYPE is worse than an absent one: a page\n"
              "feature-tests for presence, gets a truthy value of the wrong kind,\n"
              "and calls it. If the correct type cannot be delivered honestly,\n"
              "DELETE THE MEMBER -- absent beats present-and-wrong.")
        sys.exit(1)
    print("test-jstype: ok -- %d finding(s), all baselined; %d newly fixed"
          % (len(now), len(gone)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
