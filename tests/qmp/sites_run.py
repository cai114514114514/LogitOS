#!/usr/bin/env python3
"""Run the site scoreboard: one boot per site, one JSON record per site, one
dated snapshot per run, and a diff between any two snapshots.

    # the whole corpus, four boots at a time
    python3 tests/qmp/sites_run.py --iso build/logit.iso --disk build/disk.img

    # a few of them, twice each, to see which ones are flaky
    python3 tests/qmp/sites_run.py --only bing,github --repeat 2

    # what changed since yesterday -- the point of the whole exercise
    python3 tests/qmp/sites_run.py --diff tests/scoreboard/2026-08-07.json \
                                          tests/scoreboard/2026-08-08.json

ONE BOOT PER SITE. Not a preference: three sites in one boot produced two false
failures (stale window geometry after the first navigation, and residual JS
state), and both were reported before being caught. Parallelism is across BOOTS,
never within one -- each worker is a whole separate QEMU with its own SLIRP, its
own snapshot of the disk and its own serial log, so there is nothing for two
sites to share.

FLAKINESS IS A VERDICT, NOT NOISE TO BE SMOOTHED. With --repeat > 1 a site whose
verdicts disagree across runs is recorded as FLAKY with every verdict it
produced. It is not scored, and it is not quietly reported as its best or its
worst result.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DRIVER = os.path.join(HERE, "qmp_site.py")
CORPUS = os.path.join(HERE, "sites_corpus.tsv")

# Ordered worst-to-best, which is the order the table is printed in: the whole
# point is that the top of the table is the work.
ORDER = ["CRASH", "HARNESS", "TIMEOUT", "FETCH-FAIL", "BLANK", "ERRORS",
         "FLAKY", "NETWORK", "OK"]


def load_corpus(path, only=None):
    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            name, url = parts[0].strip(), parts[1].strip()
            note = parts[2].strip() if len(parts) > 2 else ""
            if only and name not in only:
                continue
            rows.append({"name": name, "url": url, "reported": note})
    return rows


def stage(iso, disk):
    """Copy the ISO and the disk somewhere no build can touch them.

    A full pass is half an hour and this worktree is shared with several lines
    that run `make`. The first attempt at a full run was taken while a
    concurrent build had build/disk.img deleted: every one of the thirty-six
    boots died before executing an instruction, and the snapshot recorded
    eighteen HARNESS rows in nine seconds. Even had it survived that, a build
    finishing halfway through would have scored the first nine sites on one
    kernel and the last nine on another -- and the table would not have said so.
    So: copy once, hash what was copied, and record the hash in the snapshot."""
    d = tempfile.mkdtemp(prefix="scoreboard_img_")
    out = []
    for p in (iso, disk):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            sys.exit("sites_run: %s is missing or empty -- is a build running? "
                     "(this used to be reported as 'the kernel never booted')" % p)
        q = os.path.join(d, os.path.basename(p))
        shutil.copy2(p, q)
        out.append(q)
    h = hashlib.sha256()
    with open(out[0], "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return out[0], out[1], h.hexdigest()[:16], d


def run_one(row, args, outdir, attempt):
    name = row["name"] if attempt == 1 else "%s#%d" % (row["name"], attempt)
    out = os.path.join(outdir, "%s.json" % name)
    # Delete any record from a previous pass into the same directory FIRST. The
    # fallback below adopts whatever file it finds, and a driver that dies
    # without writing would otherwise silently republish yesterday's verdict for
    # today's build -- the one failure mode that makes the day-to-day diff lie.
    try:
        os.remove(out)
    except OSError:
        pass
    cmd = [sys.executable, DRIVER, "--iso", args.iso, "--disk", args.disk,
           "--name", name, "--url", row["url"], "--out", out,
           "--shots", outdir]
    t0 = time.time()
    try:
        subprocess.run(cmd, cwd=ROOT, timeout=args.per_site_timeout,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        pass
    if os.path.exists(out):
        with open(out, encoding="utf-8") as fh:
            rec = json.load(fh)
    else:
        rec = {"name": name, "url": row["url"], "verdict": "HARNESS",
               "why": "the driver produced no record in %.0fs"
                      % args.per_site_timeout,
               "host": {}, "guest": {}, "pixels": {}}
    rec["reported"] = row["reported"]
    rec["wall_seconds"] = round(time.time() - t0, 1)
    print("  %-18s %-11s %s" % (name, rec["verdict"], rec["why"]), flush=True)
    return rec


def merge_repeats(name, url, reported, recs):
    """Collapse N runs of one site into one row. Disagreement is preserved."""
    verdicts = [r["verdict"] for r in recs]
    if len(set(verdicts)) == 1:
        out = dict(recs[0])
        out["name"] = name          # recs[0] may be the "<name>#2" repeat
        out["url"] = url
        out["reported"] = reported
        out["runs"] = len(recs)
        out["all_verdicts"] = verdicts
        return out
    # Prefer the record of the worst verdict as the one to keep the detail from,
    # so a FLAKY row still carries a stack to look at.
    recs = sorted(recs, key=lambda r: ORDER.index(r["verdict"])
                  if r["verdict"] in ORDER else 0)
    out = dict(recs[0])
    out["name"] = name
    out["url"] = url
    out["reported"] = reported
    out["runs"] = len(recs)
    out["all_verdicts"] = verdicts
    out["verdict"] = "FLAKY"
    out["why"] = "verdicts disagreed across %d runs: %s" % (len(recs), ", ".join(verdicts))
    return out


def render_table(snap):
    rows = sorted(snap["sites"],
                  key=lambda r: (ORDER.index(r["verdict"]) if r["verdict"] in ORDER
                                 else 0, r["name"]))
    w = ["site", "verdict", "load s", "reqs", "exc", "changed px", "host"]
    lines = []
    lines.append("| %-18s | %-10s | %6s | %5s | %3s | %10s | %-8s |"
                 % tuple(w))
    lines.append("|%s|%s|%s|%s|%s|%s|%s|"
                 % ("-" * 20, "-" * 12, "-" * 8, "-" * 7, "-" * 5, "-" * 12, "-" * 10))
    for r in rows:
        g = r.get("guest", {}) or {}
        p = r.get("pixels", {}) or {}
        h = r.get("host", {}) or {}
        nexc = (len(g.get("exceptions", [])) + len(g.get("timer_exceptions", []))
                + len(g.get("module_exceptions", [])))
        host = ("HTTP %s" % h["status"]) if h.get("ok") else "unreachable"
        lines.append("| %-18s | %-10s | %6s | %5s | %3s | %10s | %-8s |" % (
            r["name"], r["verdict"],
            g.get("load_seconds", "-"), g.get("requests", "-"),
            nexc if g else "-", p.get("changed_px", "-"), host))
    return "\n".join(lines)


def render_detail(snap):
    out = []
    for r in sorted(snap["sites"],
                    key=lambda x: (ORDER.index(x["verdict"]) if x["verdict"] in ORDER
                                   else 0, x["name"])):
        g = r.get("guest", {}) or {}
        out.append("")
        out.append("### %s -- %s" % (r["name"], r["verdict"]))
        out.append("url:      %s" % r.get("url", "?"))
        out.append("reported: %s" % (r.get("reported") or "-"))
        out.append("verdict:  %s" % r.get("why", ""))
        if r.get("all_verdicts") and len(set(r["all_verdicts"])) > 1:
            out.append("runs:     %s" % ", ".join(r["all_verdicts"]))
        h = r.get("host", {}) or {}
        if h.get("ok"):
            out.append("host:     HTTP %s, %s bytes, %ss, %d <script> (%d src), %d <img>%s"
                       % (h.get("status"), h.get("bytes"), h.get("elapsed"),
                          h.get("script_tags", 0), h.get("script_src", 0),
                          h.get("img_tags", 0),
                          (", redirects: " + " ".join(h["redirects"]))
                          if h.get("redirects") else ""))
        elif h:
            out.append("host:     UNREACHABLE (%s)" % h.get("error"))
        p = r.get("pixels", {}) or {}
        if p:
            out.append("pixels:   changed %s (blank control ink %s), ink %s, "
                       "colours %s, rich-tile proxy %s, bbox %s"
                       % (p.get("changed_px"), p.get("ink_px_blank"),
                          p.get("ink_px"), p.get("colours"),
                          p.get("rich_tiles_proxy"), p.get("changed_bbox")))
        if g.get("requests") is not None:
            out.append("network:  %s requests, %s connections dialled, %s reused, "
                       "%s modules (%s failed)"
                       % (g["requests"], g["dials"], g["reused"],
                          g["modules"], g["modules_failed"]))
        if g.get("fetch_failed"):
            out.append("sub-resource failures (%d):" % len(g["fetch_failed"]))
            for f in g["fetch_failed"][:8]:
                out.append("    %s" % f)
        if g.get("cannot_fetch"):
            out.append("unfetchable refs (%d): %s"
                       % (len(g["cannot_fetch"]), "; ".join(g["cannot_fetch"][:4])))
        if g.get("skipped_scripts"):
            out.append("non-executable <script> blocks skipped: %d" % g["skipped_scripts"])
        for e in g.get("exceptions", []):
            out.append("EXCEPTION x%d: %s" % (e["count"], e["message"]))
            for fr in e["stack"][:8]:
                out.append("    %s" % fr)
        for e in g.get("timer_exceptions", []):
            out.append("EXCEPTION (timer/event): %s" % e)
        for e in g.get("module_exceptions", []):
            out.append("EXCEPTION (module): %s" % e)
        if r.get("shot"):
            out.append("shot:     %s" % os.path.relpath(r["shot"], ROOT))
    return "\n".join(out)


def do_diff(a_path, b_path):
    a = json.load(open(a_path, encoding="utf-8"))
    b = json.load(open(b_path, encoding="utf-8"))
    av = {r["name"]: r for r in a["sites"]}
    bv = {r["name"]: r for r in b["sites"]}
    print("%s -> %s" % (a.get("date", a_path), b.get("date", b_path)))
    moved = 0
    for name in sorted(set(av) | set(bv)):
        x = av.get(name, {}).get("verdict", "(absent)")
        y = bv.get(name, {}).get("verdict", "(absent)")
        if x != y:
            moved += 1
            print("  %-18s %-11s -> %-11s   %s"
                  % (name, x, y, bv.get(name, {}).get("why", "")))
    if not moved:
        print("  no verdict changed")
    def tally(s):
        t = {}
        for r in s["sites"]:
            t[r["verdict"]] = t.get(r["verdict"], 0) + 1
        return t
    print("  before: %s" % tally(a))
    print("  after:  %s" % tally(b))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/logit.iso")
    ap.add_argument("--disk", default="build/disk.img")
    ap.add_argument("--corpus", default=CORPUS)
    ap.add_argument("--only", default="", help="comma-separated site names")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("SITE_JOBS", "4")))
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--label", default=time.strftime("%Y-%m-%d"))
    ap.add_argument("--per-site-timeout", type=float, default=900.0)
    # The commit the ISO WAS BUILT FROM, which is not always this worktree's
    # HEAD: this tree is shared, HEAD moves during a half-hour pass, and the
    # honest baseline is built from a clean clone of a named commit. Recording
    # the wrong one makes tomorrow's diff meaningless.
    ap.add_argument("--commit", default=None)
    ap.add_argument("--diff", nargs=2, default=None)
    args = ap.parse_args()

    if args.diff:
        sys.exit(do_diff(*args.diff))

    only = set(x for x in args.only.split(",") if x) or None
    rows = load_corpus(args.corpus, only)
    if not rows:
        sys.exit("sites_run: the corpus selected nothing")

    outdir = args.outdir or os.path.join(ROOT, "tests", "scoreboard", args.label)
    os.makedirs(outdir, exist_ok=True)

    src_iso = args.iso
    args.iso, args.disk, iso_sha, stage_dir = stage(args.iso, args.disk)

    print("scoreboard %s: %d sites x %d run(s), %d boots at a time, into %s"
          % (args.label, len(rows), args.repeat, args.jobs,
             os.path.relpath(outdir, ROOT)))
    print("  ISO %s sha256:%s, staged in %s (a concurrent build cannot swap it)"
          % (src_iso, iso_sha, stage_dir))
    t0 = time.time()
    jobs = [(row, att) for att in range(1, args.repeat + 1) for row in rows]
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_one, row, args, outdir, att): (row, att)
                for row, att in jobs}
        for f in concurrent.futures.as_completed(futs):
            row, _att = futs[f]
            results.setdefault(row["name"], []).append(f.result())

    sites = [merge_repeats(r["name"], r["url"], r["reported"], results[r["name"]])
             for r in rows if r["name"] in results]
    # Paths go in RELATIVE. The snapshot is committed and diffed against
    # tomorrow's, and an absolute path bakes in whose machine it ran on -- every
    # row would then show a difference on a run from a different checkout.
    for r in sites:
        for k in ("shot", "serial_log"):
            if r.get(k) and os.path.isabs(r[k]):
                try:
                    r[k] = os.path.relpath(r[k], ROOT).replace("\\", "/")
                except ValueError:
                    pass
    shutil.rmtree(stage_dir, ignore_errors=True)
    snap = {"date": args.label,
            "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "iso": src_iso, "iso_sha256_16": iso_sha,
            "commit": args.commit or subprocess.run(
                ["git", "rev-parse", "--short", "HEAD"],
                cwd=ROOT, capture_output=True, text=True).stdout.strip(),
            "wall_seconds": round(time.time() - t0, 1),
            "repeat": args.repeat,
            "sites": sites}

    snap_path = os.path.join(ROOT, "tests", "scoreboard", "%s.json" % args.label)
    os.makedirs(os.path.dirname(snap_path), exist_ok=True)
    with open(snap_path, "w", encoding="utf-8") as fh:
        json.dump(snap, fh, indent=1, ensure_ascii=False)

    table = render_table(snap)
    detail = render_detail(snap)
    md_path = os.path.join(ROOT, "tests", "scoreboard", "%s.md" % args.label)
    tally = {}
    for r in sites:
        tally[r["verdict"]] = tally.get(r["verdict"], 0) + 1
    header = ("# Site scoreboard %s\n\ncommit %s, ISO %s (sha256:%s), %d sites, "
              "%d run(s) each, %.0f s wall\n\n%s\n" %
              (args.label, snap["commit"], src_iso, iso_sha, len(sites), args.repeat,
               snap["wall_seconds"],
               ", ".join("%s %d" % (k, tally[k]) for k in ORDER if k in tally)))
    with open(md_path, "w", encoding="utf-8") as fh:
        fh.write(header + "\n" + table + "\n\n## Detail\n" + detail + "\n")

    print()
    print(header)
    print(table)
    print()
    print("snapshot: %s" % os.path.relpath(snap_path, ROOT))
    print("table:    %s" % os.path.relpath(md_path, ROOT))
    print("shots:    %s" % os.path.relpath(outdir, ROOT))


if __name__ == "__main__":
    main()
