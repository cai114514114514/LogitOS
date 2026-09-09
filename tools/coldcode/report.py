#!/usr/bin/env python3
"""tools/coldcode/report.py -- WHICH FUNCTIONS DID THE REAL WEB NEVER ENTER.

The twin of tools/check-test-liveness.py. That one finds tests that cannot
fail; this one finds code that cannot run. Both exist because a hand-written
list -- CLAUDE.md's "(b) Built with no real consumer" -- rots, is incomplete by
construction, and cannot catch the instance somebody adds tomorrow.

WHAT IT READS
  llvm-cov export JSON over one or more coverage-instrumented binaries and a
  merged .profdata. Function-level entry counts only: `count == 0` means the
  function was never ENTERED by anything in the corpora that produced the
  profile.

WHAT IT IS NOT
  Not a coverage percentage. A percentage over a browser is meaningless --
  error handling alone would sink it, and a number that is always bad is a
  number nobody acts on. The headline is a LIST, ranked by lines.

THE THREE POPULATIONS, WHICH THIS TOOL DOES NOT CONFLATE
  1. CANNOT RUN     unreachable by construction (a macro nothing defines, a
                    keycode no key produces, an arm a lexer excludes).
  2. NEVER RAN HERE reachable, but no page in these corpora went there. This
                    is a statement about the corpus as much as the code.
  3. CORRECTLY COLD error paths, refusal paths, absent-feature branches. These
                    SHOULD be cold. Listing them as findings is noise, and
                    noise is what makes people stop reading.
  This phase produces the RAW TABLE and the raw cold list. It labels nothing as
  category 1 vs 2 vs 3 -- that is a judgement, and a judgement printed beside a
  measurement gets quoted as one.

THE APPARATUS CHECK, WHICH RUNS BEFORE ANY NUMBER IS PRINTED
  CLAUDE.md rule 1. A coverage report over a binary that did not link the file
  would report every function in it as cold -- the same failure in a new
  costume. So:
    * the two control functions in tests/unit/cold_control.c must read
      hot=covered and cold=uncovered. Either one wrong and this refuses to
      print.
    * every translation unit asked for must APPEAR in the coverage map. A TU
      with zero function records was not linked, and is reported as
      NOT-LINKED rather than as 100% cold.
"""
import argparse
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The control's two known answers. See tests/unit/cold_control.c.
CTRL_HOT = "coldctl_hot_marker"
CTRL_COLD = "coldctl_cold_unreachable"


def find_llvm_tool(name):
    """llvm-profdata / llvm-cov, and the keg-only trap tests/sysroot.mk paid for.

    brew keeps the llvm formula keg-only, so a plain `which llvm-cov` finds
    nothing on macOS even with LLVM installed. Xcode ships them too, and on a
    tree built with Apple clang the XCODE ones are the right ones -- a
    Homebrew llvm-cov two major versions ahead may refuse the coverage map
    version Apple clang emitted.
    """
    for cand in (
        subprocess.run(["xcrun", "-f", name], capture_output=True, text=True).stdout.strip(),
        f"/opt/homebrew/opt/llvm/bin/{name}",
        f"/usr/local/opt/llvm/bin/{name}",
    ):
        if cand and os.path.exists(cand):
            return cand
    from shutil import which
    return which(name)


def load(objects, profdata, covtool):
    cmd = [covtool, "export", objects[0]]
    for o in objects[1:]:
        cmd += ["-object", o]
    cmd += [f"-instr-profile={profdata}"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"coldcode: llvm-cov export failed:\n{p.stderr[:2000]}")
    return json.loads(p.stdout)


# llvm-cov export region layout, and the two fields that matter here:
#   [LineStart, ColStart, LineEnd, ColEnd, ExecutionCount, FileID, ExpandedFileID, Kind]
R_LINE_START, R_LINE_END, R_FILE_ID = 0, 2, 5


def fn_lines(f):
    """Lines a function record spans IN ITS OWN FILE.

    THE FileID FILTER IS NOT A REFINEMENT, IT IS THE DIFFERENCE BETWEEN A
    NUMBER AND A WRONG NUMBER. A function record carries regions for every
    file its body expanded through -- itself plus every macro and every static
    inline it pulled in from a header. Taking min/max over all of them spans
    from a header's line 22 to the .c file's line 40000, and the first version
    of this file did exactly that: it printed `c/lib/audio/audio.h  41022 cold
    lines` for a header that is 1,167 lines long, and `js_canvas.c 40773` for
    a 3,000-line file. A line count larger than the file is the cheap check
    that catches this; it fires the instant you look, which is why it is worth
    printing line counts at all.
    """
    lo = hi = None
    for r in f.get("regions", []):
        if r[R_FILE_ID] != 0:          # not this function's own file
            continue
        a, b = r[R_LINE_START], r[R_LINE_END]
        lo = a if lo is None else min(lo, a)
        hi = b if hi is None else max(hi, b)
    if lo is None:
        return 0, 0, 0
    return lo, hi, hi - lo + 1


def tus_files(fns):
    return {fl for (fl, _lo) in fns}


def strip_tu(name):
    """`h264.c:bs_init` -> `bs_init`. llvm prefixes a STATIC function with the
    TU that emitted it, because two files may both define `static int helper`.
    """
    return name.split(":", 1)[1] if ":" in name else name


def collect(doc, want_prefixes):
    """(file, start_line) -> one SOURCE function.

    THREE THINGS THIS KEY IS DOING, each of which was a wrong table first.

    1. filenames[0] ONLY. A record's remaining filenames are the headers its
       body expanded through, not places it is defined. Attributing a function
       to all of them invented 64 functions in c/lib/audio/audio.h, which is
       the primary file of exactly ZERO of them.

    2. Keyed on the SOURCE LOCATION, not the symbol name. A `static inline` in
       a header is emitted once per including TU, and llvm names each copy
       after its TU -- `h264.c:bs_init`, `h265.c:bs_init`, fifteen more. Those
       are ONE source function. Keyed by name they are fifteen, and c/lib/video/
       bs.h reads `180 functions defined, 0 entered` when the file holds about
       a dozen. The question this instrument answers is "did this SOURCE ever
       execute", not "did this TU's copy of it".

    3. count is SUMMED across records and across BINARIES, and `entered` is
       count > 0 -- so a function that ran in webapi_probe and not in wpt_test
       has run. It ran. That is the whole question.
    """
    out = {}
    for blk in doc["data"]:
        for f in blk.get("functions", []):
            if not f["filenames"]:
                continue
            rel = os.path.relpath(f["filenames"][0], REPO)
            if not any(rel.startswith(p) for p in want_prefixes):
                continue
            lo, hi, n = fn_lines(f)
            if not lo:
                continue
            e = out.setdefault((rel, lo), {"count": 0, "lines": 0,
                                           "name": strip_tu(f["name"]), "copies": 0})
            e["count"] += f["count"]
            e["lines"] = max(e["lines"], n)
            e["copies"] += 1
    return out


def control_check(doc):
    hot = cold = None
    for blk in doc["data"]:
        for f in blk.get("functions", []):
            if f["name"] == CTRL_HOT:
                hot = (hot or 0) + f["count"]
            elif f["name"] == CTRL_COLD:
                cold = (cold or 0) + f["count"]
    problems = []
    if hot is None:
        problems.append(f"{CTRL_HOT} is not in the coverage map at all -- "
                        "tests/unit/cold_control.c was not linked.")
    elif hot == 0:
        problems.append(f"{CTRL_HOT} reads COLD. It is a constructor; it runs "
                        "before main() on every invocation. The reader is broken.")
    if cold is None:
        problems.append(f"{CTRL_COLD} is not in the coverage map -- the optimiser "
                        "deleted it, so its absence would read as 'no finding'.")
    elif cold != 0:
        problems.append(f"{CTRL_COLD} reads COVERED (count={cold}). Its only call "
                        "site is behind a macro nothing defines. The reader is broken.")
    return hot, cold, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--object", action="append", required=True,
                    help="instrumented binary; repeat for more than one")
    ap.add_argument("--profdata", required=True)
    ap.add_argument("--prefix", action="append",
                    default=None, help="repo-relative path prefix to report on")
    ap.add_argument("--expect-tu", action="append", default=[],
                    help="file that MUST appear in the coverage map (apparatus check)")
    ap.add_argument("--json", help="write the raw table here")
    ap.add_argument("--top", type=int, default=60)
    a = ap.parse_args()

    prefixes = a.prefix or ["c/apps/browser/", "c/lib/", "c/net/", "tests/unit/cold_control.c"]

    covtool = find_llvm_tool("llvm-cov")
    if not covtool:
        print("coldcode: SKIP -- llvm-cov is not on this host.")
        print("  settle it with:  brew install llvm   (it is KEG-ONLY: look in")
        print("  /opt/homebrew/opt/llvm/bin, not on PATH), or install Xcode.")
        return 2

    doc = load(a.object, a.profdata, covtool)

    # ---- the apparatus, before any number ---------------------------------
    hot, cold, problems = control_check(doc)
    if problems:
        print("coldcode: REFUSING TO REPORT -- the control did not hold.")
        for p in problems:
            print("  " + p)
        return 1
    print(f"control: {CTRL_HOT} count={hot} (covered, as it must be); "
          f"{CTRL_COLD} count={cold} (cold, as it must be)")

    fns = collect(doc, prefixes)

    # ---- a second apparatus check, and it is the one that caught the first
    # version of this file: no function can span more lines than its file has.
    # A span larger than the file means regions from another file were folded
    # in, which is how `audio.h` came to report 41,022 cold lines.
    # A function may not span past the end of its own file. Two very different
    # things break that, and telling them apart is the whole value:
    #
    #   DRIFT   the file was EDITED after the binary was built. Three workflows
    #           are live in c/apps/browser and third_party/css; this instrument
    #           found css_extra.c edited nine minutes after its own link. The
    #           MEASUREMENT is still sound -- it measured the code that was
    #           compiled -- but the line numbers no longer address the file on
    #           disk, so they are marked and not silently printed as if fresh.
    #   FOLDING regions from an included file folded into the span. That is an
    #           apparatus bug and every line count is fiction. REFUSE.
    #
    # The discriminator is mtime against the newest object. Cheap, and it is
    # the only signal available after the fact.
    built_at = max(os.path.getmtime(o) for o in a.object)
    drift, overs = set(), []
    for (fl, lo), e in fns.items():
        p = os.path.join(REPO, fl)
        try:
            nlines = sum(1 for _ in open(p, "rb"))
            mt = os.path.getmtime(p)
        except OSError:
            continue
        if lo + e["lines"] - 1 > nlines + 1:
            (drift.add(fl) if mt > built_at else overs.append(
                (fl, e["name"], lo, e["lines"], nlines)))
    if overs:
        print("coldcode: REFUSING TO REPORT -- %d functions span past the end of"
              % len(overs))
        print("  their own file, and the file was NOT edited after the build, so")
        print("  this is region folding and every line count below is fiction:")
        for fl, nm, lo, n, nl in overs[:5]:
            print("    %s:%d %s spans %d lines; the file has %d" % (fl, lo, nm, n, nl))
        return 1
    # Drift is wider than the files that happened to overflow: ANY source newer
    # than the binary was not the source that was measured.
    for fl in list(tus_files(fns)):
        p = os.path.join(REPO, fl)
        if os.path.exists(p) and os.path.getmtime(p) > built_at:
            drift.add(fl)
    if drift:
        print("\nSOURCE DRIFT -- %d file(s) were edited AFTER this binary was "
              "linked." % len(drift))
        print("  The coverage is of what was COMPILED and is sound; the line")
        print("  numbers address a file that has since moved. Rebuild to settle:")
        for fl in sorted(drift):
            print("    " + fl)

    # per-TU roll-up
    tus = {}
    for (fl, lo), e in fns.items():
        t = tus.setdefault(fl, {"defined": 0, "entered": 0, "cold": [], "cold_lines": 0})
        t["defined"] += 1
        if e["count"] > 0:
            t["entered"] += 1
        else:
            t["cold"].append((e["name"], e["lines"], lo))
            t["cold_lines"] += e["lines"]

    missing = [t for t in a.expect_tu if t not in tus]
    if missing:
        print("coldcode: REFUSING TO REPORT -- these TUs have NO function records,")
        print("  which means the binary did not link them. Every function in them")
        print("  would otherwise be reported cold, which is this instrument's own")
        print("  failure mode wearing the answer's clothes:")
        for m in missing:
            print("    " + m)
        return 1

    total_def = sum(t["defined"] for t in tus.values())
    total_ent = sum(t["entered"] for t in tus.values())
    print(f"\n{len(tus)} translation units, {total_def} functions defined, "
          f"{total_ent} entered, {total_def - total_ent} NEVER ENTERED.")

    print("\n=== PER TRANSLATION UNIT "
          "(defined / entered / never entered / cold lines) ===")
    rows = sorted(tus.items(), key=lambda kv: (-(kv[1]["defined"] - kv[1]["entered"]),
                                               -kv[1]["cold_lines"]))
    print(f"{'file':<46} {'def':>5} {'ent':>5} {'cold':>5} {'coldln':>7}")
    for fl, t in rows:
        print(f"{fl:<46} {t['defined']:>5} {t['entered']:>5} "
              f"{t['defined'] - t['entered']:>5} {t['cold_lines']:>7}")

    print(f"\n=== NEVER ENTERED, ranked by lines (top {a.top}) ===")
    flat = sorted(((e["lines"], fl, e["name"], lo)
                   for (fl, lo), e in fns.items() if e["count"] == 0),
                  reverse=True)
    for n, fl, nm, ln in flat[:a.top]:
        print(f"{n:>5}  {fl}:{ln}  {nm}")

    if a.json:
        with open(a.json, "w") as fh:
            json.dump({
                "control": {"hot": hot, "cold": cold},
                "tus": {fl: {"defined": t["defined"], "entered": t["entered"],
                             "cold_lines": t["cold_lines"],
                             "cold": sorted(t["cold"], key=lambda c: -c[1])}
                        for fl, t in tus.items()},
            }, fh, indent=1)
        print(f"\nraw table -> {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
