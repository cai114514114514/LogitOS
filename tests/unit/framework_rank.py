#!/usr/bin/env python3
"""framework_rank -- rank what kills a framework build, ACROSS builds.

    python3 tests/unit/framework_rank.py build/webapi_probe tests/fixtures/frameworks
    ... --baseline tests/fixtures/frameworks/BASELINE   assert, do not just report

WHAT QUESTION THIS ANSWERS, AND WHY IT IS NOT PER-FRAMEWORK
The hypothesis this corpus was built to test is that modern frameworks fail
here for ONE shared reason -- that the framework is not the variable, the
bundler runtime under it is. React, Vue, Angular and Svelte ship through three
or four bundlers, so if that hypothesis holds the failures collapse onto a
handful of mechanisms and the ranked table is short and top-heavy. If instead
each framework dies of its own thing, the table is flat, and THAT is the
finding -- a flat table means there is no single fix and the work is a list.

So the unit of the output is the CAUSE, counted in frameworks, never the
framework counted in exceptions. "N of 7 die on X" is a work order; "angular
throws 3" is a symptom with no address on it.

HOW A CAUSE IS DERIVED FROM A MESSAGE
By the table in CAUSES below, which maps an exception message to the platform
feature whose absence produces it, and to the code that produced the message.
Each row was established by reading the bundle at the throw site, and the
evidence is quoted in the row -- not inferred from the wording. A message that
matches no row is reported as `unclassified` and counted; it is never dropped,
because a silent drop is how a cause table stays comfortably short.

THE CHROME COLUMN
`tests/chrome/webapi_chromediff.mjs` (the JS-exception line's tool, not a
second one) runs the same committed bytes through real headless Chrome and
subtracts. Its verdict is folded in here with --chromediff <log>. Read the
comment on CHROMEDIFF_BLIND_SPOT before trusting a `clean` in that column for a
bare `Error:`.
"""

import os
import re
import subprocess
import sys

# The bundler family each fixture's toolchain emits a runtime from. This is the
# axis the hypothesis is about, so it is stated as data and printed as its own
# breakdown rather than left for the reader to reconstruct from the names.
FAMILY = {
    "react":   "rollup/vite",
    "vue":     "rollup/vite",
    "svelte":  "rollup/vite",
    "vite":    "rollup/vite",
    "webpack": "webpack",
    "next":    "webpack/turbopack",
    "angular": "esbuild/angular-cli",
}

# message pattern -> (cause, the platform feature, the evidence)
#
# The evidence column is the point. Every row here was read out of the built
# bundle at the throw site; a cause table assembled from the wording of error
# messages would have put webpack and Next in different buckets, since they
# word the same missing property two entirely different ways.
CAUSES = [
    (r"chunk path empty but not in a worker",
     "document.currentScript",
     "next/turbopack chunk loader derives the chunk base URL from "
     "document.currentScript.src; undefined -> empty base -> throw"),
    # The message Next throws CHANGED under this measurement, on 2026-08-08,
    # when the JS-exception line landed document.currentScript: the Turbopack
    # "chunk path empty" throw above became this invariant. Same cause, one
    # layer further in -- currentScript now exists but reads null on the path
    # the page actually takes (see `_platform`, where c1 and c2 disagree). Both
    # patterns are kept because the older bundles in tests/fixtures/webapi
    # (nodejs.org) still produce the first one.
    (r"Expected document\.currentScript to be a <script> element",
     "document.currentScript",
     "next/react: currentScript is published but reads NULL in channel 2 -- the "
     "unwrapped path the browser really runs -- so Next's invariant trips"),
    (r"Automatic publicPath is not supported",
     "document.currentScript",
     "webpack runtime: currentScript.src, then a getElementsByTagName('script') "
     "fallback that requires an ABSOLUTE .src (/^http(s?):/); both fail"),
    (r"cannot read property 'cloneNode' of undefined",
     "HTMLTemplateElement.content",
     "svelte template(): document.createElement('template').innerHTML = ...; "
     "returns .content, which is undefined here, and the caller clones it"),
    (r"'SVGElement' is not defined",
     "SVGElement (global constructor)",
     "vue runtime-dom tests `el instanceof SVGElement` to choose the "
     "setAttribute vs property patch path"),
    (r"Invalid URL: /",
     "document.baseURI",
     "angular router: new URL(path, document.baseURI); baseURI undefined, so "
     "the relative '/' has no base to resolve against"),
]

UNCAUGHT_RE = re.compile(r"^\s+(\S+)\s+\.\.\. UNCAUGHT: (\d+)")
CLEAN_RE = re.compile(
    r"^\s+(\S+)\s+\.\.\. ran clean: c1 (\d+)/(\d+) classic, c2 (\d+)/(\d+) classic, "
    r"(\d+)/(\d+) modules\s+\(chunks loaded (\d+), missing (\d+)\)")
JSON_RE = re.compile(r"^#JSON\t([^\t]*)\t([^\t]*)\t([^\t]*)\t(.*)$")


def classify(msg):
    for pat, cause, why in CAUSES:
        if re.search(pat, msg):
            return cause, why
    return "unclassified: " + msg[:60], ""


def probe_dirs(probe, dirs):
    out = subprocess.run([probe, "--errors", "--json"] + dirs,
                         capture_output=True, text=True, errors="replace")
    return (out.stdout or "") + (out.stderr or "")


def run_probe(probe, corpus):
    """The framework builds. `_`-prefixed directories are NOT applications and
    are excluded from the ranking -- `_platform` is the reduction of these
    bundles' throw sites to one script, and counting it as an eighth framework
    would put a diagnostic in a table whose unit is real builds."""
    dirs = sorted(os.path.join(corpus, d) for d in os.listdir(corpus)
                  if os.path.isdir(os.path.join(corpus, d))
                  and not d.startswith("_")
                  and os.path.exists(os.path.join(corpus, d, "index.html")))
    if not dirs:
        print("framework_rank: no fixtures under %s" % corpus)
        sys.exit(1)
    return probe_dirs(probe, dirs), [os.path.basename(d) for d in dirs]


API_RE = re.compile(r"^#API\t([^\t]*)\t(.*)$")


def run_platform(probe, corpus):
    """The `_platform` fixture: each feature the corpus dies on, asked directly.

    Values are collected as an ORDERED LIST rather than a single answer,
    because the probe runs every script twice -- channel 1 wrapped in the
    recording Proxy, channel 2 exactly as the browser runs it -- and the two do
    not always agree. On 2026-08-08 `document.currentScript` answered `element`
    in channel 1 and `null` in channel 2, which is the difference between "the
    API exists" and "the API works on the path a page takes", and collapsing
    the two into one value would have hidden precisely that."""
    d = os.path.join(corpus, "_platform")
    if not os.path.exists(os.path.join(d, "index.html")):
        return []
    text = probe_dirs(probe, [d])
    seen = []
    order = []
    vals = {}
    for line in text.replace("\x00", "").split("\n"):
        m = API_RE.match(line.rstrip("\r"))
        if not m:
            continue
        name, v = m.group(1), m.group(2).strip()
        if name not in vals:
            vals[name] = []
            order.append(name)
        if v not in vals[name]:
            vals[name].append(v)
    for name in order:
        seen.append((name, vals[name]))
    return seen


def parse(text):
    sites = {}
    for line in text.split("\n"):
        m = CLEAN_RE.match(line)
        if m:
            s = sites.setdefault(m.group(1), {"exc": [], "uncaught": None})
            s["c2_clean"], s["c2_total"] = int(m.group(4)), int(m.group(5))
            s["mod_clean"], s["mod_total"] = int(m.group(6)), int(m.group(7))
            s["chunks"] = int(m.group(8))
            continue
        m = UNCAUGHT_RE.match(line)
        if m:
            sites.setdefault(m.group(1), {"exc": [], "uncaught": None})["uncaught"] = int(m.group(2))
            continue
        m = JSON_RE.match(line)
        if m:
            sites.setdefault(m.group(1), {"exc": [], "uncaught": None})["exc"].append(m.group(4))
    return sites


# ---- the Chrome column ---------------------------------------------------
#
# CHROMEDIFF_BLIND_SPOT, and it is worth reading before this column is trusted.
# webapi_chromediff.mjs reduces a message to (error type, identifier) before
# subtracting, and it extracts the type with /^([A-Z]\w*Error|DOMException)\b/.
# That pattern cannot match the BASE class `Error`, because it requires at least
# one character before the literal "Error" -- and bucket() then discards
# anything with neither a type nor an identifier as "not an error object".
#
# Every bundler-runtime failure is a bare `throw new Error(...)`: webpack's
# "Automatic publicPath is not supported in this browser" and Turbopack's
# "chunk path empty but not in a worker" are both dropped, from BOTH sides of
# the diff. So the differential is currently blind to exactly the class of
# failure this corpus was built to measure, and reports `OURS ONLY 0` for the
# two fixtures where the answer is "ours".
#
# That is the JS-exception line's file, so it is reported rather than patched
# here (a one-token fix: add `Error` to the alternation). Until it lands, a
# `clean` in the Chrome column for one of those two rows means "not measured",
# and this script says so instead of printing a number it cannot support.
CHROME_BLIND = re.compile(r"^(Uncaught\s+)?Error:")


def read_chromediff(path):
    """site -> set of messages Chrome did NOT throw ("[OURS]" in its report)."""
    ours, seen = {}, set()
    site = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^\s+(\S+)\s+chrome (\d+) distinct", line)
        if m:
            site = m.group(1)
            seen.add(site)
            ours.setdefault(site, set())
            continue
        m = re.match(r"^\s+\[OURS\]\s+(.*)$", line)
        if m and site:
            ours[site].add(m.group(1).strip())
    return ours, seen


def _bare(msg):
    """The comparable part of a message: the two engines and the two channels
    prefix the same throw differently ('Uncaught (in promise) ', 'ERROR ')."""
    m = re.sub(r"^(ERROR\s+|Uncaught\s*(\(in promise\)\s*)?)+", "", msg.strip())
    return m[:70]


def chrome_verdict(cause_msgs, ours, seen):
    """ours | shared | NOT MEASURED, for one cause across the sites it hits.

    `ours` = the exception survived the subtraction: Chrome, given the same
    committed bytes, did not throw it. `shared` = Chrome threw it too, so it is
    the page's own bug and not a work item. `NOT MEASURED` = the differential
    cannot see this class at all -- see CHROMEDIFF_BLIND_SPOT above."""
    if not seen:
        return "no-diff"
    verdicts = set()
    for site, msg in cause_msgs:
        if site not in seen:
            verdicts.add("no-diff")
        elif CHROME_BLIND.match(_bare(msg)):
            verdicts.add("NOT MEASURED")
        elif any(_bare(msg) == _bare(o) for o in ours.get(site, ())):
            verdicts.add("ours")
        else:
            verdicts.add("shared")
    if len(verdicts) == 1:
        return verdicts.pop()
    return "/".join(sorted(verdicts))


PAINT_RE = re.compile(r"^#PAINT\t(\S+)\t(.*)$")


def run_paint(probe, corpus, names):
    """Did the framework put anything on the page?

    "Its scripts ran clean" is not "it rendered", and treating the two as the
    same would have reported react and vite as working on a number that is also
    what a silent no-op produces. `_paint/<name>/` is the built document plus
    one extra module that reads the DOM back; the application's own scripts are
    NOT copied, the manifest points at the real fixture, so this cannot drift
    from the bytes the ranking measured.

    Each fixture is probed in its OWN process. The reporter cannot name itself,
    so with several fixtures in one run the only thing tying a line to a site
    would be output order -- and a report that attributes one page's result to
    another is worse than no report.

    The LAST line is the verdict: the probe runs every script twice, and the
    second pass is channel 2, the unwrapped path the browser really takes."""
    out = {}
    for name in names:
        d = os.path.join(corpus, "_paint", name)
        if not os.path.exists(os.path.join(d, "index.html")):
            continue
        last = None
        for line in probe_dirs(probe, [d]).replace("\x00", "").split("\n"):
            m = PAINT_RE.match(line.rstrip("\r"))
            if m:
                last = m.group(2).strip()
        if last:
            out[name] = last
    return out


def main():
    a = sys.argv[1:]
    baseline = chromelog = None
    if "--baseline" in a:
        i = a.index("--baseline"); baseline = a[i + 1]; del a[i:i + 2]
    if "--chromediff" in a:
        i = a.index("--chromediff"); chromelog = a[i + 1]; del a[i:i + 2]
    if len(a) != 2:
        print("usage: framework_rank.py <probe-binary> <corpus-dir> "
              "[--chromediff LOG] [--baseline FILE]")
        return 2
    probe, corpus = a

    text, names = run_probe(probe, corpus)
    sites = parse(text)
    missing = [n for n in names if n not in sites or sites[n]["uncaught"] is None]
    if missing:
        print("framework_rank: the probe produced no result for: %s" % ", ".join(missing))
        print(text[-2000:])
        return 1

    ours, seen = ({}, set())
    if chromelog and os.path.exists(chromelog):
        ours, seen = read_chromediff(chromelog)

    # cause -> {sites: set, msgs: [(site,msg)], why: str}
    causes = {}
    for name in names:
        for msg in sites[name]["exc"]:
            cause, why = classify(msg)
            c = causes.setdefault(cause, {"sites": set(), "msgs": [], "why": why})
            c["sites"].add(name)
            c["msgs"].append((name, msg))
    clean = [n for n in names if sites[n]["uncaught"] == 0]

    n = len(names)
    print()
    print("== framework corpus: what kills the page, ranked across %d real builds ==" % n)
    print("   (committed built output, no network; the unit is the CAUSE, counted in frameworks)")
    print()
    print("%-34s %-9s %-14s %s" % ("CAUSE", "FRAMEWKS", "CHROME", "WHICH"))
    print("%-34s %-9s %-14s %s" % ("-" * 34, "-" * 9, "-" * 14, "-" * 28))
    ranked = sorted(causes.items(), key=lambda kv: (-len(kv[1]["sites"]), kv[0]))
    for cause, c in ranked:
        v = chrome_verdict(c["msgs"], ours, seen)
        print("%-34s %d of %-4d %-14s %s"
              % (cause[:34], len(c["sites"]), n, v, ",".join(sorted(c["sites"]))))
    if clean:
        print("%-34s %d of %-4d %-14s %s"
              % ("(no uncaught exception at all)", len(clean), n, "-", ",".join(sorted(clean))))
    print()
    for cause, c in ranked:
        if c["why"]:
            print("  %s" % cause)
            print("      %s" % c["why"])
    print()

    print("PER FRAMEWORK -- uncaught exceptions, and whether the chunk loader ran")
    print("%-9s %-20s %8s %9s %8s %s"
          % ("FIXTURE", "BUNDLER", "UNCAUGHT", "SCRIPTS", "CHUNKS", "CAUSE"))
    for name in names:
        s = sites[name]
        cl = "%d/%d cls %d/%d mod" % (s.get("c2_clean", 0), s.get("c2_total", 0),
                                      s.get("mod_clean", 0), s.get("mod_total", 0))
        cs = sorted({classify(m)[0] for m in s["exc"]})
        print("%-9s %-20s %8d %9s %8d %s"
              % (name, FAMILY.get(name, "?"), s["uncaught"], cl, s.get("chunks", 0),
                 ",".join(c[:40] for c in cs) or "-"))
    print()

    # The hypothesis, answered in one line rather than left to the reader.
    fams = {}
    for name in names:
        fams.setdefault(FAMILY.get(name, "?"), []).append(
            (name, sites[name]["uncaught"], sorted({classify(m)[0] for m in sites[name]["exc"]})))
    print("BY BUNDLER FAMILY -- the hypothesis under test")
    for fam in sorted(fams):
        rows = fams[fam]
        allc = sorted({c for _, _, cs in rows for c in cs})
        print("  %-20s %d builds, %d with exceptions, causes: %s"
              % (fam, len(rows), sum(1 for _, u, _ in rows if u), ", ".join(c[:38] for c in allc) or "none"))
    print()

    paint = run_paint(probe, corpus, names)
    if paint:
        print("DID IT RENDER -- the DOM read back after the app's own module ran")
        print("   (tests/fixtures/frameworks/_paint; scripts running clean is not a page)")
        print("%-9s %s" % ("FIXTURE", "DOM AFTER LOAD"))
        for name in names:
            if name in paint:
                print("%-9s %s" % (name, paint[name]))
        print()

    apis = run_platform(probe, corpus)
    if apis:
        print("PLATFORM FEATURES THE CORPUS DIES ON -- asked one at a time, from a")
        print("deferred classic script (tests/fixtures/frameworks/_platform)")
        print("%-38s %s" % ("FEATURE", "VALUE(S) OBSERVED  [c1 then c2]"))
        for name, vs in apis:
            flag = "  <-- c1 and c2 DISAGREE" if len(vs) > 1 else ""
            print("%-38s %s%s" % (name, " | ".join(vs), flag))
        print()

    # ---- the assertion ---------------------------------------------------
    # A report nobody can fail is a report. The baseline pins the number that
    # matters -- uncaught exceptions per framework -- and the cause each one is
    # attributed to, so that implementing one of these causes MUST move this
    # file. That is the acceptance check webapi_probe.c's header asks for, and
    # it is the only thing here that can go red.
    got = ["%s %d %s" % (nm, sites[nm]["uncaught"],
                         ",".join(sorted({classify(m)[0] for m in sites[nm]["exc"]})) or "-")
           for nm in names]
    # The platform answers are pinned too, and that is the half of the baseline
    # that moves FIRST. A Web API landing changes an `undefined` here one commit
    # before it changes an exception count over there, so this is where the
    # acceptance check fires earliest.
    got += ["api %s = %s" % (nm, " | ".join(vs)) for nm, vs in apis]
    # And what reached the DOM. Pinned because the failure this catches is the
    # quiet one: an API landing can take a framework from "throws" to "throws
    # nothing and renders nothing", and only this half of the baseline moves.
    got += ["paint %s %s" % (nm, paint[nm]) for nm in names if nm in paint]
    if not baseline:
        print("(no --baseline: reporting only)")
        return 0
    if not os.path.exists(baseline):
        open(baseline, "w", newline="\n").write("\n".join(got) + "\n")
        print("framework_rank: wrote a new baseline to %s -- review it" % baseline)
        return 1
    want = [l.rstrip() for l in open(baseline, encoding="utf-8").read().split("\n") if l.strip()]
    if want == got:
        print("test-frameworks: ok -- %d fixtures, %d uncaught in total, baseline matches"
              % (n, sum(sites[x]["uncaught"] for x in names)))
        return 0
    print("test-frameworks: FAILED -- the corpus no longer matches %s" % baseline)
    for line in sorted(set(want) - set(got)):
        print("  -  %s" % line)
    for line in sorted(set(got) - set(want)):
        print("  +  %s" % line)
    print("  If a Web API landed, this is the acceptance check firing: update the")
    print("  baseline in the same commit and say which cause moved.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
