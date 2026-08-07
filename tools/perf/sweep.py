#!/usr/bin/env python3
"""
tools/perf/sweep.py -- run tools/perf/perfbench.py across a list of commits.

THE PROBLEM THIS SOLVES, WHICH IS NOT "RUN THE BENCHMARK IN A LOOP"
-------------------------------------------------------------------
This machine runs other agents' QEMU instances. Measured here, a single
metric (read_ms) moved 2.3x -- 765 ms to 1750 ms -- between two runs of the
SAME ISO, purely because a build was running alongside. A sweep that visits
commit A at 10:00 and commit Z at 11:20 and compares their numbers is not
measuring the commits, it is measuring 10:00 against 11:20.

Two defences:

  ROUNDS.  Every commit is visited once per round, and the sweep does several
  rounds. A commit's samples are therefore spread across the whole wall-clock
  window rather than bunched into one minute of it, so slow minutes are shared
  out instead of being assigned to whichever commit happened to be running.

  A REFERENCE BUILD IN EVERY ROUND.  One pinned commit (--reference) is
  benchmarked in each round alongside the others. Every metric is then also
  reported RELATIVE to the reference measured in the same round. A ratio
  cancels whatever the host was doing at that moment; that ratio, not the
  absolute millisecond count, is what the verdict is read off.

Builds are cached: each commit is built once into --workdir/<sha>/ and the ISO
and disk image are kept, so the rounds cost QEMU time only.

A commit that does not build is recorded as BUILD-FAIL and is never given a
number. "Slower" and "did not compile" are different findings and the harness
must never turn the second into the first.

    python3 tools/perf/sweep.py --commits shas.txt --rounds 3 \
        --reference <sha> --out sweep.json
"""

import argparse
import json
import os
import random
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(HERE, "perfbench.py")
METRICS = ["boot_ok_ms", "desktop_ms", "prompt_ms", "floor_ms",
           "shell_net_ms", "read_net_ms", "launch_net_ms", "page_net_ms",
           "mouse_net_ms", "mouse_tax_ms"]


def sh(cmd, cwd=None, timeout=3600):
    return subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout)


def prepare(sha, args):
    """Check out and build `sha` into the cache. Returns (ok, why)."""
    dst = os.path.join(args.workdir, sha)
    iso = os.path.join(dst, "logit.iso")
    disk = os.path.join(dst, "disk.img")
    if os.path.exists(iso) and os.path.exists(disk):
        return True, "cached"
    bf = os.path.join(dst, "BUILD-FAIL")
    if os.path.exists(bf):
        with open(bf, errors="replace") as f:
            stage = (f.readline().strip() or "?")
        return False, "build-fail(%s)" % stage

    src = args.tree
    # Never a git worktree: on this host a fresh worktree rewrites line endings
    # and every .sh and .as in the tree stops working. A clone with
    # core.autocrlf=false does not.
    if not os.path.isdir(os.path.join(src, ".git")):
        return False, "no-tree"
    r = sh(["git", "-c", "core.autocrlf=false", "checkout", "-q", "-f", sha], cwd=src)
    if r.returncode != 0:
        return False, "checkout-fail"
    sh(["git", "clean", "-qxfd", "build"], cwd=src)

    os.makedirs(dst, exist_ok=True)
    log = os.path.join(dst, "build.log")

    # The ISO and the disk image are SEPARATE targets and both must be built.
    # Every ring-3 program lives on the disk image, so a sweep that stopped at
    # the ISO would benchmark one commit's kernel against another commit's
    # userland -- and, worse, would score commits that cannot produce a
    # userland at all as if they were merely slow. (Real: 23 of today's
    # commits build an ISO and fail on the disk image, because the Makefile
    # referenced c/apps/audio/audiocheck.c 66 minutes before that file was
    # committed.)
    with open(log, "w") as lf:
        r1 = subprocess.run("make -j%d" % args.jobs, cwd=src, shell=True,
                            stdout=lf, stderr=subprocess.STDOUT,
                            timeout=args.build_timeout)
        stage = "iso"
        r2 = None
        if r1.returncode == 0:
            stage = "disk"
            r2 = subprocess.run("make -j%d build/disk.img" % args.jobs, cwd=src,
                                shell=True, stdout=lf, stderr=subprocess.STDOUT,
                                timeout=args.build_timeout)

    bi = os.path.join(src, "build", "logit.iso")
    bd = os.path.join(src, "build", "disk.img")
    failed = (r1.returncode != 0 or r2 is None or r2.returncode != 0
              or not os.path.exists(bi) or not os.path.exists(bd))
    if failed:
        why = ""
        try:
            with open(log, errors="replace") as lf:
                lines = [l.rstrip() for l in lf if
                         ("error" in l.lower() or l.startswith("make:"))]
            why = lines[-1][:160] if lines else ""
        except OSError:
            pass
        with open(os.path.join(dst, "BUILD-FAIL"), "w") as f:
            f.write("%s\n%s\n" % (stage, why))
        return False, "build-fail(%s)" % stage
    shutil.copyfile(bi, iso)
    shutil.copyfile(bd, disk)
    return True, "built"


def bench(sha, args, tag):
    dst = os.path.join(args.workdir, sha)
    out = os.path.join(dst, "r%s.json" % tag)
    r = sh([sys.executable, BENCH,
            "--iso", os.path.join(dst, "logit.iso"),
            "--disk", os.path.join(dst, "disk.img"),
            "--repeat", "1", "--json", out, "--label", sha],
           timeout=args.bench_timeout)
    if not os.path.exists(out):
        return {}
    with open(out) as f:
        doc = json.load(f)
    return {k: v["median"] for k, v in doc.get("metrics", {}).items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--commits", required=True,
                    help="file of SHAs, oldest first, one per line")
    ap.add_argument("--tree", default="/tmp/lo", help="a CLONE to check out in")
    ap.add_argument("--workdir", default="/tmp/perfsweep")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--reference", default=None,
                    help="SHA benchmarked in every round; results are also "
                         "reported as a ratio to it")
    ap.add_argument("--build-timeout", type=float, default=1800)
    ap.add_argument("--bench-timeout", type=float, default=900)
    ap.add_argument("--out", default="sweep.json")
    ap.add_argument("--shuffle", action="store_true", default=True)
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    with open(args.commits) as f:
        shas = [l.split()[0] for l in f if l.strip() and not l.startswith("#")]
    if args.reference and args.reference not in shas:
        shas.append(args.reference)

    # ---- phase 1: build everything, once -----------------------------------
    status = {}
    for i, s in enumerate(shas):
        t0 = time.time()
        ok, why = prepare(s, args)
        status[s] = why
        print("build %2d/%d %s -> %s (%.0fs)"
              % (i + 1, len(shas), s, why, time.time() - t0), file=sys.stderr)

    good = [s for s in shas if status[s] in ("built", "cached")]
    if not good:
        print("nothing built", file=sys.stderr)
        return 2

    # ---- phase 2: interleaved rounds ---------------------------------------
    samples = {s: [] for s in good}
    rng = random.Random(1234)
    for rd in range(args.rounds):
        order = list(good)
        if args.shuffle:
            rng.shuffle(order)
        ref_val = None
        # The reference goes first in every round so that the ratio's
        # denominator is measured inside the same round it normalises.
        if args.reference in good:
            ref_val = bench(args.reference, args, "%d_ref" % rd)
        for s in order:
            v = bench(s, args, str(rd))
            if v:
                v["_round"] = rd
                if ref_val:
                    for m in METRICS:
                        if m in v and m in ref_val and ref_val[m]:
                            v[m + "_rel"] = v[m] / ref_val[m]
                samples[s].append(v)
            print("  round %d %s %s" % (rd, s, {m: v.get(m) for m in METRICS if m in v}),
                  file=sys.stderr)

    # ---- aggregate ----------------------------------------------------------
    res = {}
    for s in good:
        agg = {}
        for m in METRICS + [m + "_rel" for m in METRICS]:
            vals = [r[m] for r in samples[s] if m in r]
            if vals:
                med = statistics.median(vals)
                agg[m] = {"median": med, "n": len(vals),
                          "min": min(vals), "max": max(vals),
                          "mad": statistics.median([abs(v - med) for v in vals])}
        res[s] = agg

    doc = {"commits": shas, "status": status, "reference": args.reference,
           "rounds": args.rounds, "results": res,
           "raw": {s: samples[s] for s in good}}
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)

    cols = ["boot_ok_ms", "desktop_ms", "shell_net_ms", "read_net_ms",
            "launch_net_ms", "page_net_ms", "mouse_tax_ms"]
    print("\n%-10s %-16s %s" % ("commit", "status",
                                " ".join("%13s" % c for c in cols)))
    for s in shas:
        if s not in res:
            print("%-10s %-16s %s" % (s[:9], status[s], "  (no numbers)"))
            continue
        row = []
        for c in cols:
            d = res[s].get(c)
            rel = res[s].get(c + "_rel")
            if d is None:
                row.append("%13s" % "-")
            elif rel is not None:
                row.append("%8.0f/%4.2f" % (d["median"], rel["median"]))
            else:
                row.append("%13.0f" % d["median"])
        print("%-10s %-16s %s" % (s[:9], status[s], " ".join(row)))
    print("\n(each cell: absolute ms / ratio to the reference measured in the "
          "same round)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
