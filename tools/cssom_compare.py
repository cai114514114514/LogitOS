#!/usr/bin/env python3
"""Before/after over two WPT --report TSVs, counted the way this line is judged.

The pass rate is the WRONG headline and this script exists to stop it being
quoted as one. A file that never completed contributes NO subtests to the
denominator; revive it and it contributes its subtests, most of which fail at
first. So the raw pass count can fall while the result is strictly better.

Three numbers, in this order:

    files revived      dead before, completing after     <- the headline
    files newly dead   completing before, dead after     <- must be 0
    subtests           pass/total on both sides, with the denominator delta

    python3 tools/cssom_compare.py build/cssom/before.tsv build/cssom/after.tsv
"""
import collections
import sys


def read(path):
    """path -> (state, why). state is 'dead', 'ok' or 'notrun'."""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 3:
                continue
            kind, p = fields[0], fields[1]
            if kind == "HARNESS":
                out[p] = ("dead", fields[3] if len(fields) > 3 else "")
            elif kind == "NOHARNESS":
                out[p] = ("notrun", "")
            else:
                # PASS / FAIL rows: one per subtest. Presence is what matters.
                prev = out.get(p)
                if prev is None or prev[0] == "dead":
                    out[p] = ("ok", "")
    return out


def subtest_counts(path):
    npass = total = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            kind = line.split("\t", 1)[0]
            if kind == "PASS":
                npass += 1
                total += 1
            elif kind == "FAIL":
                total += 1
    return npass, total


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    before, after = read(sys.argv[1]), read(sys.argv[2])

    revived = [p for p, (s, _) in before.items()
               if s == "dead" and after.get(p, ("", ""))[0] == "ok"]
    killed = [p for p, (s, _) in before.items()
              if s == "ok" and after.get(p, ("", ""))[0] == "dead"]
    still = [p for p, (s, w) in after.items() if s == "dead"]

    dead_before = sum(1 for s, _ in before.values() if s == "dead")
    print("harness deaths:  %d before  ->  %d after   (%+d)"
          % (dead_before, len(still), len(still) - dead_before))
    print("  files REVIVED (dead -> completing):   %d" % len(revived))
    print("  files NEWLY DEAD (completing -> dead): %d%s"
          % (len(killed), "   <-- a regression" if killed else ""))
    for p in killed[:10]:
        print("      %s  %s" % (p, after[p][1][:70]))

    bp, bt = subtest_counts(sys.argv[1])
    ap, at = subtest_counts(sys.argv[2])
    print()
    print("subtests:  %d/%d passing before  ->  %d/%d after" % (bp, bt, ap, at))
    print("  denominator grew by %d -- those are the subtests the dead files hid."
          % (at - bt))
    print("  passes moved %+d." % (ap - bp))
    if at > bt and ap - bp < at - bt:
        print("  So the RATE fell while the measurement got more honest. Both numbers,")
        print("  always: quoting either alone misrepresents the change.")

    if still:
        print()
        print("still dead, top remaining causes:")
        c = collections.Counter(w[:70] for _, w in
                                ((p, after[p][1]) for p in still))
        for k, n in c.most_common(12):
            print("  %5d  %s" % (n, k))
    return 0


if __name__ == "__main__":
    sys.exit(main())
