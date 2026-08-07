#!/usr/bin/env python3
"""
tools/perf/report.py -- turn a sweep.json into the table and the verdict.

Prints, per metric, the value at every measured commit in history order, and
then flags the STEPS: consecutive measured pairs where the metric moved by more
than a factor, with the commits that bracket the step. A step's blame is the
RANGE between two measured commits, not a single commit -- a coarse sweep of
every Nth commit cannot name one, and saying otherwise would be inventing
precision. Feed the named range to tools/perf/bisect.sh to get the commit.

    python3 tools/perf/report.py sweep.json --repo /mnt/d/ststem
    python3 tools/perf/report.py sweep.json --metric read_net_ms --factor 1.3
"""

import argparse
import json
import subprocess
import sys

DEFAULT_METRICS = ["boot_ok_ms", "desktop_ms", "shell_net_ms", "read_net_ms",
                   "launch_net_ms", "page_net_ms", "mouse_tax_ms"]


def subject(repo, sha):
    try:
        return subprocess.run(
            ["git", "-C", repo, "log", "-1", "--pretty=%ad %s",
             "--date=format:%m-%d %H:%M", sha],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json")
    ap.add_argument("--repo", default=".")
    ap.add_argument("--metric", action="append", default=None)
    ap.add_argument("--factor", type=float, default=1.25,
                    help="report a step when the ratio exceeds this")
    args = ap.parse_args()

    doc = json.load(open(args.json))
    order = doc["commits"]
    res = doc["results"]
    status = doc["status"]
    metrics = args.metric or DEFAULT_METRICS

    print("reference: %s" % (doc.get("reference") or "(none)"))
    print("rounds:    %d" % doc.get("rounds", 0))
    nbuilt = sum(1 for s in order if s in res)
    print("commits:   %d sampled, %d measured, %d unmeasurable\n"
          % (len(order), nbuilt, len(order) - nbuilt))

    unbuilt = [(s, status.get(s, "?")) for s in order if s not in res]
    if unbuilt:
        print("NOT MEASURED (a build break is not a regression):")
        for s, why in unbuilt:
            print("  %-9s %-18s %s" % (s[:9], why, subject(args.repo, s)))
        print()

    for m in metrics:
        series = [(s, res[s][m]["median"], res[s].get(m + "_rel", {}).get("median"),
                   res[s][m]["min"], res[s][m]["max"], res[s][m]["n"])
                  for s in order if s in res and m in res[s]]
        if not series:
            continue
        print("=== %s ===" % m)
        print("  %-9s %9s %6s %9s %9s %2s  %s"
              % ("commit", "median", "rel", "min", "max", "n", "when / subject"))
        for s, med, rel, lo, hi, n in series:
            print("  %-9s %9.0f %6s %9.0f %9.0f %2d  %s"
                  % (s[:9], med, ("%.2f" % rel) if rel else "-", lo, hi, n,
                     subject(args.repo, s)[:64]))
        steps = []
        for i in range(1, len(series)):
            a, b = series[i - 1], series[i]
            if a[1] <= 0:
                continue
            r = b[1] / a[1]
            if r >= args.factor or r <= 1.0 / args.factor:
                steps.append((a[0], b[0], a[1], b[1], r))
        if steps:
            print("  -- steps (>= %.2fx) --" % args.factor)
            for a, b, av, bv, r in steps:
                word = "SLOWER" if r > 1 else "FASTER"
                print("     %s  %s..%s  %.0f -> %.0f  (%.2fx)"
                      % (word, a[:9], b[:9], av, bv, r))
                print("       bisect this range: PERF_METRIC=%s "
                      "PERF_THRESHOLD=%.0f git bisect start %s %s"
                      % (m, (av * bv) ** 0.5, b[:9], a[:9]))
        else:
            print("  -- no step >= %.2fx --" % args.factor)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
