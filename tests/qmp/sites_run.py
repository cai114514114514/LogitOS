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
#
# THE TOP VERDICT IS `PAINTED`, AND IT USED TO BE `OK`. That rename is not
# cosmetic. `OK` promised correctness the instrument cannot check -- it measures
# that pixels changed against a blank tab and that no script threw, and neither
# says the right pixels changed. deepseek painted, painted WRONG, and was
# published as OK for it. `PAINTED` says exactly what was observed and nothing
# more. `GAP` sits directly below it for a page that painted and threw nothing
# but never asked for part of what its document requires.
ORDER = ["CRASH", "HARNESS", "TIMEOUT", "FETCH-FAIL", "BLANK", "ERRORS", "GAP",
         "FLAKY", "NETWORK", "PAINTED"]

# What the table's header has to say about itself, because a limit the
# instrument states is worth more than one the user discovers.
LIMITS = """\
WHAT THIS TABLE IS NOT. `changed px` counts pixels that differ from an empty tab
photographed in the same boot. It cannot tell a rendered page from a flat dark
block: bing scores 625,312 changed pixels and is exactly the "一坨黑黑的" that was
reported by hand. Nothing here checks whether the RIGHT pixels changed -- that is
what reftests are for, and none of WPT's 17,155 of them run on this machine. The
top verdict is `PAINTED`, which means pixels changed, no script threw, and the
guest asked for everything the document requires. It does not mean correct.

The two `control-` rows are NOT results. They exist to prove the harness, the
network and the build were working during the pass; if either fails, no other row
in the snapshot means anything. They say nothing about the browser."""


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
    if getattr(args, "boxes", False):
        cmd.append("--boxes")
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
    #
    # BUT A HARNESS RUN IS NOT A MEASUREMENT OF THE BROWSER, and keeping it
    # threw away four real ones. HARNESS records carry `guest: {}` and
    # `pixels: {}` -- there is no stack in them to look at, which is the very
    # thing this rule exists to preserve -- so the row published a line of
    # dashes while a complete measurement sat in the same pass, in the same
    # directory, unread. In tests/scoreboard/0820-g4b that happened to kimi
    # (ERRORS, 5 text runs), openai (PAINTED, 10) and weixin (BLANK, 23);
    # all three were HARNESS-plus-a-good-run, and all three published `-`.
    #
    # So: rank only the runs that actually measured. And if exactly one
    # verdict survives that filter, the row is NOT flaky -- the browser did
    # not disagree with itself, the harness broke once. Calling that FLAKY
    # is how a harness fault gets filed as browser volatility.
    # SITES_MERGE_WORST_WINS restores the rule that shipped until
    # 2026-08-25, so tests/sites.mk's negative control can watch it discard a
    # real measurement instead of that behaviour living only in a commit
    # message. It is the plausible wrong rule, not a mutilation: ranking all
    # runs by severity is what anybody would write, and it is right up to the
    # moment one of the runs measured nothing.
    if os.environ.get("SITES_MERGE_WORST_WINS"):
        recs = sorted(recs, key=lambda r: ORDER.index(r["verdict"])
                      if r["verdict"] in ORDER else 0)
        out = dict(recs[0])
        out.update(name=name, url=url, reported=reported, runs=len(recs),
                   all_verdicts=verdicts, verdict="FLAKY")
        out["why"] = ("verdicts disagreed across %d runs: %s"
                      % (len(recs), ", ".join(verdicts)))
        return out
    NOT_A_MEASUREMENT = ("HARNESS",)
    measured = [r for r in recs if r["verdict"] not in NOT_A_MEASUREMENT]
    dropped = [r["verdict"] for r in recs if r["verdict"] in NOT_A_MEASUREMENT]
    pool = measured if measured else recs
    pool = sorted(pool, key=lambda r: ORDER.index(r["verdict"])
                  if r["verdict"] in ORDER else 0)
    out = dict(pool[0])
    out["name"] = name
    out["url"] = url
    out["reported"] = reported
    out["runs"] = len(recs)
    out["all_verdicts"] = verdicts
    if dropped:
        out["harness_failures"] = dropped
    if len(set(r["verdict"] for r in pool)) == 1:
        # One surviving verdict: report it, and say what was set aside.
        out["why"] = ("%s; %d run(s) set aside as harness faults: %s"
                      % (out.get("why", ""), len(dropped), ", ".join(dropped)))
        return out
    out["verdict"] = "FLAKY"
    out["why"] = ("verdicts disagreed across %d measured run(s): %s"
                  % (len(pool), ", ".join(r["verdict"] for r in pool))
                  + ("; %d harness fault(s) excluded: %s" % (len(dropped), ", ".join(dropped))
                     if dropped else ""))
    return out


HEAD = "| %-18s | %-10s | %6s | %5s | %8s | %3s | %10s | %11s | %-8s |"
RULE = ("|%s|%s|%s|%s|%s|%s|%s|%s|%s|"
        % ("-" * 20, "-" * 12, "-" * 8, "-" * 7, "-" * 10, "-" * 5, "-" * 12,
           "-" * 13, "-" * 10))


def _row(r):
    g = r.get("guest", {}) or {}
    p = r.get("pixels", {}) or {}
    h = r.get("host", {}) or {}
    s = r.get("subresources", {}) or {}
    nexc = (len(g.get("exceptions", [])) + len(g.get("timer_exceptions", []))
            + len(g.get("module_exceptions", [])))
    host = ("HTTP %s" % h["status"]) if h.get("ok") else "unreachable"
    # `asked/got`: the document's mandatory subresource set against the requests
    # the guest issued. This is the column that would have caught stripe.
    inv = (s.get("host_inventory") or {})
    asked = inv.get("mandatory")
    got = g.get("requests")
    gap = s.get("gap")
    cell = "-" if asked is None or got is None else (
        "%d/%d%s" % (asked, got, " !" if gap else ""))
    # `text`: runs/bytes of DOCUMENT text the browser's last paint actually
    # emitted (Ctrl+Alt+D -> c/apps/browser/browser_paint.h). It is the column
    # `changed px` could never be: pixels cannot tell a rendered page from a
    # dark block, and this says which WORDS are among them. It judges no
    # layout -- 0 runs on a page with text is a finding; a number is not a
    # pass. `-` means the dump did not arrive, which is the harness's problem
    # and not the browser's, and the two must not read alike.
    tr, tb = r.get("text_runs"), r.get("text_bytes")
    tcell = "-" if tr is None else "%d/%d" % (tr, tb or 0)
    return HEAD % (r["name"], r["verdict"], g.get("load_seconds", "-"),
                   got if got is not None else "-", cell,
                   nexc if g else "-", p.get("changed_px", "-"), tcell, host)


def _sorted(rows):
    return sorted(rows, key=lambda r: (ORDER.index(r["verdict"])
                                       if r["verdict"] in ORDER else 0, r["name"]))


def render_table(snap):
    """Two tables, never one. The controls prove the harness ran; folding them in
    with the corpus lets four rows read as 'four sites work' when two of them are
    a page with no CSS and a Wikipedia article nobody reported a problem with."""
    corpus = [r for r in snap["sites"] if not r["name"].startswith("control-")]
    ctrl = [r for r in snap["sites"] if r["name"].startswith("control-")]
    out = [HEAD % ("site", "verdict", "load s", "reqs", "asked/got", "exc",
                   "changed px", "text run/B", "host"), RULE]
    out += [_row(r) for r in _sorted(corpus)]
    if ctrl:
        out.append("")
        out.append("CONTROLS -- harness health, not results:")
        out.append(HEAD % ("control", "verdict", "load s", "reqs", "asked/got",
                           "exc", "changed px", "text run/B", "host"))
        out.append(RULE)
        out += [_row(r) for r in _sorted(ctrl)]
    return "\n".join(out)


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
        s = r.get("subresources") or {}
        inv = s.get("host_inventory")
        if inv:
            out.append("document: %d stylesheets, %d script src, %d inline script, "
                       "%d img, %d preload, %d font -> %d mandatory subresources"
                       % (inv["stylesheets"], inv["script_src"],
                          inv["inline_scripts"], inv["images"], inv["preloads"],
                          inv["fonts"], inv["mandatory"]))
        if s.get("gap"):
            gp = s["gap"]
            out.append("GAP:      the guest issued %d requests against %d mandatory "
                       "-- SHORT BY %d. A request never made cannot appear in "
                       "fetch_failed; this is the only column that sees it."
                       % (gp["requested"], gp["mandatory"], gp["short_by"]))
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


# A verdict that was RENAMED, not changed. The 2026-08-08 snapshot at fc0c38a is
# kept byte-identical -- it is the honest starting line -- and it says `OK` where
# every later snapshot says `PAINTED`. Without this the first diff across the
# rename would report every previously-passing site as having moved, which is
# exactly the kind of false movement this tool exists to make impossible.
ALIAS = {"OK": "PAINTED"}


def do_diff(a_path, b_path):
    a = json.load(open(a_path, encoding="utf-8"))
    b = json.load(open(b_path, encoding="utf-8"))
    av = {r["name"]: r for r in a["sites"]}
    bv = {r["name"]: r for r in b["sites"]}
    print("%s -> %s" % (a.get("date", a_path), b.get("date", b_path)))
    if any(r["verdict"] in ALIAS for r in a["sites"] + b["sites"]):
        print("  (note: `OK` in an older snapshot is read as `PAINTED` -- the "
              "verdict was renamed, not re-measured)")
    moved = 0
    for name in sorted(set(av) | set(bv)):
        x = ALIAS.get(av.get(name, {}).get("verdict", "(absent)"),
                      av.get(name, {}).get("verdict", "(absent)"))
        y = ALIAS.get(bv.get(name, {}).get("verdict", "(absent)"),
                      bv.get(name, {}).get("verdict", "(absent)"))
        if x != y:
            moved += 1
            print("  %-18s %-11s -> %-11s   %s"
                  % (name, x, y, bv.get(name, {}).get("why", "")))
    if not moved:
        print("  no verdict changed")

    # THE VERDICT IS NOT THE ONLY THING THAT MOVES, and reporting only the
    # verdict was hiding real work. The first diff this tool ever produced said
    # "no verdict changed" across nine commits -- while github had lost
    # `CustomEvent is not defined` outright and anthropic had lost an exception.
    # A site four bugs from rendering does not change verdict when one of the
    # four is fixed, and that fix is exactly what somebody wants to see.
    print("  exceptions, and the messages that appeared or went away:")
    quiet = True
    for name in sorted(set(av) | set(bv)):
        ra, rb = av.get(name, {}), bv.get(name, {})
        if name.startswith("control-"):
            continue

        def msgs(r):
            g = r.get("guest") or {}
            return set([e["message"] for e in g.get("exceptions", [])]
                       + list(g.get("timer_exceptions", []))
                       + list(g.get("module_exceptions", [])))
        ma, mb = msgs(ra), msgs(rb)
        gone, new = sorted(ma - mb), sorted(mb - ma)
        if not gone and not new:
            continue
        quiet = False
        print("    %s: %d -> %d" % (name, len(ma), len(mb)))
        for m in gone:
            print("      GONE %s" % m[:150])
        for m in new:
            print("      NEW  %s" % m[:150])
    if quiet:
        print("    nothing appeared and nothing went away")

    # And the subresource gap, which is the column a failure counter cannot see.
    # Only compared when BOTH snapshots measured it: the column was added after
    # the first baseline, and "absent" is not "zero" -- reporting it as a move
    # would invent a regression on every site the older run predates.
    have_a = any("subresources" in r for r in a["sites"])
    have_b = any("subresources" in r for r in b["sites"])
    if not (have_a and have_b):
        print("  (subresource gap not comparable: the %s snapshot predates the "
              "column)" % ("before" if not have_a else "after"))
    else:
        for name in sorted(set(av) | set(bv)):
            def short(r):
                return ((r.get("subresources") or {}).get("gap") or {}).get("short_by")
            sa, sb = short(av.get(name, {})), short(bv.get(name, {}))
            if sa != sb:
                print("  subresource gap %-16s %s -> %s" % (name, sa, sb))
    def tally(s):
        # Controls excluded: they are harness health, and counting them here
        # would let "the harness still works" pad the score.
        t = {}
        for r in s["sites"]:
            if r["name"].startswith("control-"):
                continue
            v = ALIAS.get(r["verdict"], r["verdict"])
            t[v] = t.get(v, 0) + 1
        return t
    print("  before: %s" % tally(a))
    print("  after:  %s" % tally(b))
    for s, lbl in ((a, "before"), (b, "after")):
        bad = [r["name"] for r in s["sites"]
               if r["name"].startswith("control-")
               and ALIAS.get(r["verdict"], r["verdict"]) != "PAINTED"]
        if bad:
            print("  WARNING (%s): control(s) did not pass: %s -- no row in that "
                  "snapshot means anything" % (lbl, ", ".join(bad)))

    # WHICH WORDS WENT AWAY. The verdict, the exception list and the
    # subresource gap all missed the most expensive regression this tree has
    # ever had, and it is worth stating exactly because it is the reason this
    # block exists: stripe went from 69 painted text runs to 38 to ZERO --
    # scoring BLANK with no failed request, no missing subresource and no new
    # exception -- because Node.isEqualNode was absent, React declared
    # hydration lost and fell back to a client render that died. Every column
    # above this one reported that page as healthy.
    #
    # `changed px` cannot see it either, and this file's own header says why:
    # it "cannot tell a rendered page from a flat dark block". TEXT RUNS is the
    # cheap middle -- not where the pixels are but WHICH WORDS are among them.
    print("  painted text runs:")
    tquiet = True
    for name in sorted(set(av) | set(bv)):
        if name.startswith("control-"):
            continue
        ta = av.get(name, {}).get("text_runs")
        tb = bv.get(name, {}).get("text_runs")
        if ta is None or tb is None or ta == tb:
            continue
        tquiet = False
        print("    %-18s %5s -> %-5s   %s" % (name, ta, tb,
              "GONE TO ZERO" if tb == 0 and ta else
              ("fewer" if tb < ta else "more")))
    if tquiet:
        print("    unchanged everywhere both snapshots measured")
    return 0


# THE JUDGE. do_diff() reports; this decides, and the split is deliberate.
#
# WHY THIS IS NOT A ci-host: LINE, AND MUST NOT BECOME ONE. Every row here is
# a LIVE site fetched over the real internet. Sites redesign, A/B test, serve
# different markup to different exit IPs and go down. A gate that fails for
# those reasons is CLAUDE.md's rule 1 in its purest form -- "a gate that fails
# for a reason unrelated to the code under test is noise that trains people to
# ignore red" -- and the people it would train are the ones who most need to
# read this output. So this is run deliberately, by a person, across a change;
# it is a judge, not a watchdog.
#
# WHAT IT REFUSES TO CALL A REGRESSION, and each exclusion is a claim:
#   - a text-run count that merely MOVED. A site that redesigns its front page
#     legitimately paints a different number of words, and charging that to the
#     engine would make every run red within a week.
#   - a site absent from one of the two snapshots. Nothing can be concluded
#     from a comparison with a measurement that was never taken.
#   - the control rows. They are harness health, and they are handled first
#     and separately, below.
#
# WHAT IT DOES CALL A REGRESSION:
#   - a verdict that got WORSE. PAINTED is the top; anything below it, after
#     having been PAINTED, is a page that used to render and now does not.
#   - TEXT RUNS FALLING TO ZERO from a non-zero count. That is the isEqualNode
#     shape exactly, and it is unambiguous in a way a percentage is not: a
#     redesign changes how many words a page paints, it does not stop the page
#     painting words.
#
# AND A FAILED CONTROL EXITS 2, LOUDER THAN A REGRESSION. If the harness, the
# network or the build was broken during a pass, then "no regression" is not a
# result -- it is the absence of a measurement wearing the costume of one. That
# is the worst outcome this file can produce, so it is the loudest.
def do_regress(a_path, b_path):
    rc = do_diff(a_path, b_path)
    a = json.load(open(a_path, encoding="utf-8"))
    b = json.load(open(b_path, encoding="utf-8"))
    av = {r["name"]: r for r in a["sites"]}
    bv = {r["name"]: r for r in b["sites"]}

    for s, lbl, path in ((a, "before", a_path), (b, "after", b_path)):
        bad = [r["name"] for r in s["sites"]
               if r["name"].startswith("control-")
               and ALIAS.get(r["verdict"], r["verdict"]) != "PAINTED"]
        if bad:
            print("\nREGRESS: INCONCLUSIVE -- control(s) failed in %s (%s): %s"
                  % (lbl, path, ", ".join(bad)))
            print("         No row in that snapshot means anything, so neither "
                  "does a comparison against it.")
            return 2

    # RANK FROM ORDER, NOT FROM A SECOND LIST. The first version of this
    # function carried its own {"PAINTED":3,"ERRORS":2,"BLANK":1,"HARNESS":0},
    # which is four of the TEN verdicts this file defines -- so a site falling
    # from PAINTED to GAP, FLAKY, NETWORK, TIMEOUT, FETCH-FAIL or CRASH was not
    # a regression, because those words were not in the dictionary. It was
    # found within the hour by looking at a real snapshot in which a live site
    # scored GAP. ORDER is already a total ordering, worst-first, maintained
    # beside the verdicts themselves; its index IS the rank.
    rank = {v: i for i, v in enumerate(ORDER)}
    worse = []
    for name in sorted(set(av) & set(bv)):
        if name.startswith("control-"):
            continue
        x = ALIAS.get(av[name].get("verdict", ""), av[name].get("verdict", ""))
        y = ALIAS.get(bv[name].get("verdict", ""), bv[name].get("verdict", ""))
        if x in rank and y in rank and rank[y] < rank[x]:
            worse.append("%s: verdict %s -> %s" % (name, x, y))
        ta, tb = av[name].get("text_runs"), bv[name].get("text_runs")
        if ta and tb == 0:
            worse.append("%s: painted text runs %d -> 0 (the page stopped "
                         "painting words)" % (name, ta))

    # A PAGE THAT PAINTS NO WORDS AT ALL, WHATEVER ITS VERDICT SAYS.
    #
    # Measured 2026-08-29, on the first snapshot this judge ever read: a live
    # site scored the TOP verdict, PAINTED, with 17,144 changed pixels and
    # ZERO text runs. That is this file's own header warning coming true --
    # "`changed px` cannot tell a rendered page from a flat dark block" -- and
    # the verdict scale cannot express it, because PAINTED means pixels
    # changed, no script threw and every mandatory subresource arrived. All
    # three were true. No word was on the screen.
    #
    # This does NOT reclassify the verdict, and that restraint is the point: a
    # page may legitimately paint no text (a canvas application, an image
    # viewer), and a scoreboard that called those broken would be wrong in a
    # way its own header spends a paragraph warning against. It is reported as
    # a STANDING condition rather than a change, so it shows up on the very
    # first comparison instead of waiting for a regression that already
    # happened before anyone was watching.
    silent = [r["name"] for r in b["sites"]
              if not r["name"].startswith("control-")
              and r.get("text_runs") == 0
              and ALIAS.get(r.get("verdict", ""), r.get("verdict", "")) == "PAINTED"]
    if silent:
        print("\n  NOTE: %d site(s) scored PAINTED with ZERO painted text runs: %s"
              % (len(silent), ", ".join(silent)))
        print("        Pixels changed and no word reached the screen. Not "
              "counted as a regression -- a page may legitimately paint no "
              "text -- but PAINTED is the top verdict and it is not describing "
              "a page a person can read.")

    if worse:
        print("\nREGRESS: FAIL -- %d site(s) got worse:" % len(worse))
        for w in worse:
            print("  %s" % w)
        return 1
    print("\nREGRESS: ok -- no site lost its verdict and none stopped painting "
          "words. (A moved text-run count is not counted; see do_regress.)")
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
    ap.add_argument("--regress", nargs=2, default=None,
                    help="like --diff, but EXITS NON-ZERO when a site got worse "
                         "(1) or when a control failed so the comparison is "
                         "inconclusive (2)")
    # Passed straight through to the driver; see its --boxes help for why it
    # is off by default.
    ap.add_argument("--boxes", action="store_true",
                    help="dump each page's display list to its serial log")
    args = ap.parse_args()

    if args.diff:
        sys.exit(do_diff(*args.diff))
    if args.regress:
        sys.exit(do_regress(*args.regress))

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
    scored = {}
    for r in sites:
        if not r["name"].startswith("control-"):
            scored[r["verdict"]] = scored.get(r["verdict"], 0) + 1
    header = ("# Site scoreboard %s\n\ncommit %s, ISO %s (sha256:%s), %d sites "
              "(+%d controls), %d run(s) each, %.0f s wall\n\n%s\n\n%s\n" %
              (args.label, snap["commit"], src_iso, iso_sha,
               sum(scored.values()), len(sites) - sum(scored.values()),
               args.repeat, snap["wall_seconds"],
               ", ".join("%s %d" % (k, scored[k]) for k in ORDER if k in scored),
               LIMITS))
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
