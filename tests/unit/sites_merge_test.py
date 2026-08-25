#!/usr/bin/env python3
"""merge_repeats: a HARNESS run is not a measurement of the browser.

WHY THIS FILE EXISTS. sites_run.py collapses N runs of one site into one row.
When the runs disagreed it kept the record of the WORST verdict, on the stated
and reasonable ground that "a FLAKY row still carries a stack to look at".

A HARNESS record has `guest: {}` and `pixels: {}`. There is no stack in it.
So the rule threw away the very thing it existed to preserve, and in
tests/scoreboard/0820-g4b it did that to three sites at once -- kimi (ERRORS,
5 text runs), openai (PAINTED, 10) and weixin (BLANK, 23) each had ONE
harness fault and ONE complete measurement, and each published a row of
dashes while the measurement sat unread in the same directory.

The second half matters as much: with one verdict surviving the filter, the
row is NOT flaky. The browser did not disagree with itself; the harness broke
once. Calling that FLAKY is how a harness fault gets filed as browser
volatility -- and six of seventeen rows were FLAKY in that snapshot.

Run: python3 tests/unit/sites_merge_test.py
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE)) if os.path.basename(HERE) == "unit" else HERE
SR = os.path.join(os.path.dirname(HERE), "qmp", "sites_run.py")


def load():
    spec = importlib.util.spec_from_file_location("sites_run", SR)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def rec(verdict, px=None):
    """A run record. px=None is the shape a HARNESS record really has."""
    return {"verdict": verdict, "why": "w",
            "guest": ({"load_seconds": 1.0} if px else {}),
            "pixels": ({"changed_px": px} if px else {})}


CASES = [
    # label, verdicts, expected verdict, expect metrics kept
    ("HARNESS + PAINTED (the kimi/openai/weixin shape)", ["HARNESS", "PAINTED"], "PAINTED", True),
    ("HARNESS + ERRORS", ["HARNESS", "ERRORS"], "ERRORS", True),
    ("FETCH-FAIL + PAINTED", ["FETCH-FAIL", "PAINTED"], "FLAKY", True),
    ("BLANK + PAINTED (the browser really did disagree)", ["BLANK", "PAINTED"], "FLAKY", True),
    ("HARNESS + HARNESS (nothing was measured at all)", ["HARNESS", "HARNESS"], "HARNESS", False),
    ("PAINTED + PAINTED", ["PAINTED", "PAINTED"], "PAINTED", True),
]


def main():
    m = load()
    bad = 0
    for label, verdicts, want, want_metrics in CASES:
        recs = [rec(v, 100 if v != "HARNESS" else None) for v in verdicts]
        out = m.merge_repeats("t", "u", True, recs)
        got = out["verdict"]
        px = out.get("pixels", {}).get("changed_px")
        ok = (got == want) and (bool(px) == want_metrics)
        bad += not ok
        print("  %s %-50s -> %-10s changed_px=%s"
              % ("ok  " if ok else "FAIL", label, got, px))
        if got != want:
            print("       wanted verdict %s" % want)
        if bool(px) != want_metrics:
            print("       wanted metrics %s" % ("kept" if want_metrics else "absent"))
    # The disagreement must survive in the record either way: a row that
    # silently drops a run is the failure this file is about.
    recs = [rec("HARNESS"), rec("PAINTED", 100)]
    out = m.merge_repeats("t", "u", True, recs)
    for key, want in (("all_verdicts", ["HARNESS", "PAINTED"]),
                      ("harness_failures", ["HARNESS"])):
        ok = out.get(key) == want
        bad += not ok
        print("  %s %-50s -> %s" % ("ok  " if ok else "FAIL",
                                    "the discarded run is still recorded (%s)" % key,
                                    out.get(key)))
    print("\n%d check(s) failed" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
