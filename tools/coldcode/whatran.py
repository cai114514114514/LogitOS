#!/usr/bin/env python3
"""tools/coldcode/whatran.py -- I JUST WROTE 400 LINES.  DID ANY OF IT RUN?

tools/coldcode/report.py answers that question about the whole tree, once, and
a whole-tree report is read once and is wallpaper by Friday.  This file answers
it about ONE CHANGE, which is the only shape in which the answer arrives while
somebody can still act on it.

Every one of the six failures of 2026-08-28/29 -- GLASS_FIELD_SLOW, the
forward-Delete keycode no key produces, F12, the @supports custom-property arm,
the two singleton iterations that publish Screen and Crypto, layout_text.c --
was a diff that a person could have run this against at the moment they wrote
it.  All six would have come back with lines that never ran.

WHAT IT DOES
  Takes a diff (or a file list), takes a merged .profdata from a corpus run,
  and reports the coverage of ONLY the lines that changed.  The answer is a
  sentence, not a percentage:

      Of the 412 lines you changed in js_webapi.c, 47 never ran.
      They are these functions: ...

WHY IT ASKS llvm-cov FOR THE LINE COUNTS INSTEAD OF DERIVING THEM
  CLAUDE.md rule 3, one jar two doors.  Turning coverage REGIONS into per-line
  counts is a real algorithm -- innermost enclosing region, gap regions
  excluded, skipped regions distinguished from unexecuted ones, max over the
  region entries that start on the line.  Spelling it a second time here would
  be a second door onto the same jar, and it would disagree with llvm-cov about
  exactly the lines that matter: the never-taken branch inside a function that
  ran.  `llvm-cov export -format=lcov` emits DA:<line>,<count> records that
  llvm-cov computed itself.  One door.  It also costs 0.36 s over both probes,
  against 264 MB and half a minute for the JSON export.

THE FOUR ANSWERS, AND THEY ARE NOT THREE
  RAN                 the line executed.  See THE HONEST LIMIT below -- this
                      does NOT mean it worked.
  NEVER RAN           the line carries executable code and no page in the
                      corpus reached it.  This is the finding.
  NO EXECUTABLE CODE  blank, comment, declaration, close brace, `#include`.
                      Not a finding, never counted as one.  A tool that calls
                      a blank line cold is a tool nobody finishes reading.
  NOT IN THE MAP      the probe did not link this file at all.  Reported
                      SEPARATELY and loudly, because a coverage report over a
                      binary that did not link the file calls every function in
                      it cold -- CLAUDE.md rule 1, the same failure in a new
                      costume, and the one this instrument is most able to
                      commit.

THE APPARATUS CHECKS, ALL OF WHICH RUN BEFORE ANY NUMBER IS PRINTED
  1. THE CONTROL, WATCHED BOTH WAYS, at line granularity.  See
     tests/unit/cold_control.c.  Four known answers in two functions; any one
     of them wrong and this refuses to print.
  2. SOURCE DRIFT, and for a diff tool this is the whole ballgame.  You just
     edited the file.  If the profile predates the edit, every line number in
     it addresses a file that has since moved, and the answer is not merely
     stale -- it is misaligned, so it will confidently name the wrong function.
     Any reported file newer than the newest instrumented object is a REFUSAL,
     not a warning.
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

CTRL_SRC = "tests/unit/cold_control.c"
CTRL_HOT_FN = "coldctl_hot_marker"
CTRL_COLD_FN = "coldctl_cold_unreachable"
# The marker comments are the anchor, not the line numbers -- net.c:249.
CTRL_RAN_MARK = "COLDCTL-RAN"
CTRL_COLD_MARK = "COLDCTL-COLD"
# A TRAILING marker comment and nothing else.  The first version matched the
# bare token anywhere on the line and so matched cold_control.c's own header
# comment, which NAMES the markers while explaining them -- line 79, prose,
# carrying no executable code.  The control refused to report, correctly, and
# that refusal is the first thing this instrument ever caught: itself.
CTRL_MARK_RE = re.compile(r"/\*\s*(COLDCTL-RAN|COLDCTL-COLD)\s*\*/\s*$")


def ctrl_marks(path):
    """-> (lines carrying the RAN marker, lines carrying the COLD marker)."""
    ran, cold = [], []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for i, t in enumerate(fh, 1):
            m = CTRL_MARK_RE.search(t.rstrip("\n"))
            if not m:
                continue
            (ran if m.group(1) == CTRL_RAN_MARK else cold).append(i)
    return ran, cold


def find_llvm_tool(name):
    """llvm-cov, and the keg-only trap tests/sysroot.mk paid for.

    brew keeps the llvm formula KEG-ONLY, so a plain `which llvm-cov` finds
    nothing on macOS even with LLVM installed.  Xcode ships them too, and on a
    tree built with Apple clang the Xcode one is the RIGHT one -- a Homebrew
    llvm-cov two major versions ahead may refuse the coverage-map version
    Apple clang emitted, and its refusal looks like an empty report.
    """
    cands = []
    try:
        cands.append(subprocess.run(["xcrun", "-f", name], capture_output=True,
                                    text=True).stdout.strip())
    except OSError:
        pass
    cands += [f"/opt/homebrew/opt/llvm/bin/{name}", f"/usr/local/opt/llvm/bin/{name}"]
    for c in cands:
        if c and os.path.exists(c):
            return c
    from shutil import which
    return which(name)


# ---------------------------------------------------------------- lcov parse

def load_lcov(objects, profdata, covtool):
    cmd = [covtool, "export", "-format=lcov", objects[0]]
    for o in objects[1:]:
        cmd += ["-object", o]
    cmd += [f"-instr-profile={profdata}"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit("coldcode/whatran: llvm-cov export failed:\n" + p.stderr[:2000])

    lines, fns = {}, {}          # rel -> {line: count} ;  rel -> [(start, name)]
    cur = None
    for ln in p.stdout.splitlines():
        if ln.startswith("SF:"):
            cur = os.path.relpath(ln[3:], REPO)
            lines.setdefault(cur, {})
            fns.setdefault(cur, [])
        elif cur is None:
            continue
        elif ln.startswith("DA:"):
            a, b = ln[3:].split(",", 1)
            n, c = int(a), int(b)
            # Merge across the two probes: a line that ran in EITHER binary
            # ran.  It ran.  That is the whole question.
            lines[cur][n] = max(lines[cur].get(n, 0), c)
        elif ln.startswith("FN:"):
            a, nm = ln[3:].split(",", 1)
            fns[cur].append((int(a), nm))
        elif ln == "end_of_record":
            cur = None

    brk = os.environ.get("COLDCODE_BREAK", "")
    if brk:
        lines, fns = _inject_defect(brk, lines, fns)
    return lines, fns


def _inject_defect(mode, lines, fns):
    """DELIBERATE DEFECTS, so the control can be WATCHED FAILING.

    CLAUDE.md: "a green test that has never been shown to fail is not
    evidence" -- the argument tools/break-build.sh already makes for the
    full-system harness.  tests/coldcode-negctl.mk sets COLDCODE_BREAK and
    requires this tool to REFUSE.  Nothing in a build or a corpus run sets it;
    it is read here and nowhere else.

    fnlevel   the coarse reader.  Every line in a function gets the function's
              entry count.  This passes a function-level control perfectly and
              is wrong about exactly the thing that matters: a never-taken
              branch inside a function that ran.
    nolink    the control TU absent from the map -- what a report over a binary
              that did not link the file looks like from the inside.
    """
    if mode == "nolink":
        lines.pop(CTRL_SRC, None)
        fns.pop(CTRL_SRC, None)
        return lines, fns
    if mode == "fnlevel":
        for f, da in lines.items():
            ranges = fn_ranges(fns.get(f, []))
            if not ranges:
                continue
            for start, end, _nm in ranges:
                c = da.get(start, 0)
                for n in list(da):
                    if start <= n <= end:
                        da[n] = c
        return lines, fns
    sys.exit(f"coldcode/whatran: unknown COLDCODE_BREAK={mode!r} "
             f"(fnlevel | nolink)")


def fn_ranges(fnlist):
    """[(start, name)] -> [(start, end, name)], sorted.

    C functions are contiguous and do not nest, so the next function's start
    line bounds this one.  Attribution is nearest-preceding-definition; a
    changed line above the first function belongs to file scope.  This is an
    approximation and is stated as one -- it names the function a line SITS IN,
    which is what a person needs to go and look, and it cannot mislabel a line
    as covered or cold because it plays no part in that decision.
    """
    fl = sorted(set(fnlist))
    out = []
    for i, (start, name) in enumerate(fl):
        end = fl[i + 1][0] - 1 if i + 1 < len(fl) else 10 ** 9
        out.append((start, end, name))
    return out


def fn_of(ranges, line):
    lo, hi = 0, len(ranges) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if ranges[mid][0] <= line:
            best = ranges[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if best is None or line > best[1]:
        return None
    return best


# ---------------------------------------------------------------- diff parse

HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def parse_diff(text):
    """unified diff -> {repo-relative path: set(new-file line numbers added)}.

    Only '+' lines.  A deleted line has no line number in the new file and no
    coverage to report; a context line is not your change.
    """
    out, cur, n = {}, None, 0
    for ln in text.splitlines():
        if ln.startswith("+++ "):
            p = ln[4:].split("\t", 1)[0].strip()
            if p == "/dev/null":
                cur = None
                continue
            cur = p[2:] if p.startswith(("a/", "b/")) else p
            out.setdefault(cur, set())
        elif ln.startswith("--- ") or ln.startswith("diff "):
            continue
        elif ln.startswith("@@"):
            m = HUNK.match(ln)
            n = int(m.group(1)) if m else 0
        elif cur is None:
            continue
        elif ln.startswith("+"):
            out[cur].add(n)
            n += 1
        elif ln.startswith("-") or ln.startswith("\\"):
            pass
        else:
            n += 1
    return {k: v for k, v in out.items() if v}


# ---------------------------------------------------------------- the control

def control_check(lines, fns):
    """Four known answers in two functions.  Any one wrong and we refuse.

    The pair coldctl_hot_marker / coldctl_cold_unreachable controls a reader
    that answers "did this FUNCTION run".  It is NOT sufficient here: a reader
    that hands the enclosing function's entry count to every line in its body
    passes both halves of it and still calls every never-taken branch in the
    tree covered.  So the line control lives INSIDE ONE FUNCTION THAT RAN --
    coldctl_line_marker -- with a branch that cannot be taken.  A coarse reader
    gets the COLDCTL-COLD lines wrong and is caught here.
    """
    bad = []
    if CTRL_SRC not in lines:
        return [f"{CTRL_SRC} is not in the coverage map at all. The control was "
                f"not linked into the probe, so nothing below is checked. "
                f"Rebuild with tools/coldcode/build.sh."]

    da = lines[CTRL_SRC]
    byname = {}
    for start, nm in fns.get(CTRL_SRC, []):
        byname[nm.split(":", 1)[-1]] = start

    # (a) the function-level pair
    for nm, want_hot in ((CTRL_HOT_FN, True), (CTRL_COLD_FN, False)):
        if nm not in byname:
            bad.append(f"{nm} is not in the coverage map -- the optimiser deleted "
                       f"it, and its ABSENCE would read as 'no finding'.")
            continue
        c = da.get(byname[nm], 0)
        if want_hot and c == 0:
            bad.append(f"{nm} reads COLD at line {byname[nm]}. It is a constructor; "
                       f"it runs before main() on every invocation. The reader is broken.")
        if not want_hot and c != 0:
            bad.append(f"{nm} reads COVERED (count={c}) at line {byname[nm]}. Its only "
                       f"call site is behind a macro nothing defines. The reader is broken.")

    # (b) the line-level pair, found by marker text rather than line number
    src = os.path.join(REPO, CTRL_SRC)
    try:
        ran_l, cold_l = ctrl_marks(src)
    except OSError as e:
        return [f"cannot read {CTRL_SRC}: {e}"]

    if not ran_l or not cold_l:
        bad.append(f"the {CTRL_RAN_MARK}/{CTRL_COLD_MARK} markers are gone from "
                   f"{CTRL_SRC} ({len(ran_l)} ran, {len(cold_l)} cold). The line "
                   f"half of the control cannot be evaluated, so it is not a control.")
    for i in ran_l:
        if i not in da:
            bad.append(f"{CTRL_SRC}:{i} ({CTRL_RAN_MARK}) carries NO executable code "
                       f"in the map. The control moved; it is measuring the wrong line.")
        elif da[i] == 0:
            bad.append(f"{CTRL_SRC}:{i} ({CTRL_RAN_MARK}) reads NEVER RAN. It is the "
                       f"first statement of a constructor. The reader is broken.")
    for i in cold_l:
        if i not in da:
            bad.append(f"{CTRL_SRC}:{i} ({CTRL_COLD_MARK}) carries NO executable code "
                       f"in the map -- the compiler folded the volatile test away, so "
                       f"the branch's ABSENCE reads as 'no finding'.")
        elif da[i] != 0:
            bad.append(f"{CTRL_SRC}:{i} ({CTRL_COLD_MARK}) reads RAN (count={da[i]}). "
                       f"It is inside `if (one != 1)` on a volatile that is 1. THIS IS "
                       f"THE READER ATTRIBUTING A FUNCTION'S COUNT TO ITS WHOLE BODY, "
                       f"and it is the exact failure this control exists to catch.")
    return bad


def control_summary(lines, fns):
    da = lines.get(CTRL_SRC, {})
    byname = {nm.split(":", 1)[-1]: s for s, nm in fns.get(CTRL_SRC, [])}
    ran_l, cold_l = ctrl_marks(os.path.join(REPO, CTRL_SRC))
    return (f"control held, both ways and at both granularities:\n"
            f"    function  {CTRL_HOT_FN} count={da.get(byname.get(CTRL_HOT_FN, -1), 0)}"
            f" (RAN, as it must)   "
            f"{CTRL_COLD_FN} count={da.get(byname.get(CTRL_COLD_FN, -1), 0)}"
            f" (NEVER RAN, as it must)\n"
            f"    line      {CTRL_SRC}:{ran_l[0] if ran_l else '?'} count="
            f"{da.get(ran_l[0], 0) if ran_l else '?'} (RAN)   "
            f"{len(cold_l)} lines of a never-taken branch INSIDE THAT SAME FUNCTION"
            f" count=0 (NEVER RAN)")


# Only a C translation unit or header can appear in a coverage map.  A Makefile
# or a .mk or a .py in the diff is not a finding of any kind, and listing it
# under "NOT IN THE COVERAGE MAP" would be noise -- and noise is what makes
# people stop reading, which is the failure mode this whole instrument is one
# step away from at all times.
CODE_EXT = (".c", ".h", ".cc", ".cpp", ".hpp", ".inc")

# THE KLAXON THRESHOLD, AND WHY IT IS NOT ZERO.
# "Nothing you wrote ran" is the loudest thing this tool says, and the rule
# CLAUDE.md states about controls applies to alarms too: one that fires on a
# one-line change is one people learn to scroll past, and then it is not there
# when it matters. A one-line uncovered edit is reported plainly. The klaxon is
# for a BODY of new code that nothing reaches -- js_devtools.c's 196 lines,
# layout_text.c's 782, the 400 lines somebody just wrote. Ten is the smallest
# number that is unambiguously a body of work rather than a touch-up.
KLAXON_MIN = 10


def plural(n, word):
    return f"{n} {word}" + ("" if n == 1 else "s")


def span_of(ls):
    """Explicit line numbers while they still fit on a line, a range after.

    `[891-964]` for two lines 73 apart reads as 74 cold lines and is how a
    number gets quoted wrong. Six is what fits.
    """
    if len(ls) <= 6:
        return ",".join(str(x) for x in ls)
    return f"{ls[0]}..{ls[-1]}, {len(ls)} of them"


LIMIT = """\
THE LIMIT OF THE WORD "RAN", SAID OUT LOUD BECAUSE A PERSON READING IT MUST NOT
CONCLUDE "WORKING":
  This instrument reports EXECUTION, not effect.  The live example is in this
  tree right now: js_webapi.c publishes interface objects for seven singletons
  in one loop, and two of those iterations -- Screen and Crypto -- do nothing,
  because those globals are installed after the loop runs.  The lines execute.
  Coverage calls them RAN, which is the correct answer to the question this
  tool asks and the wrong answer to the question you probably care about.
  NEVER RAN is a finding you can act on.  RAN is the absence of that finding
  and nothing more."""


def main():
    ap = argparse.ArgumentParser(description="did the lines you just changed run?")
    ap.add_argument("--object", action="append", required=True)
    ap.add_argument("--profdata", required=True)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--diff", help="unified diff file, or - for stdin")
    src.add_argument("--rev", help="git rev to diff the working tree against")
    src.add_argument("--file", action="append", help="whole file counts as changed")
    src.add_argument("--self-test", action="store_true",
                     help="evaluate ONLY the planted control and report both halves")
    ap.add_argument("--corpus-note", help="file describing what produced the profile")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()

    covtool = find_llvm_tool("llvm-cov")
    if not covtool:
        print("coldcode/whatran: SKIP -- llvm-cov is not on this host, so there is")
        print("  no coverage number to report and this gate will not invent one.")
        print("  settle it with:  brew install llvm")
        print("  (it is KEG-ONLY -- look in /opt/homebrew/opt/llvm/bin, NOT on PATH),")
        print("  or install Xcode and re-run:  xcrun -f llvm-cov")
        return 2
    for o in a.object:
        if not os.path.exists(o):
            sys.exit(f"coldcode/whatran: no such object {o} -- build it with "
                     f"tools/coldcode/build.sh")
    if not os.path.exists(a.profdata):
        sys.exit(f"coldcode/whatran: no profile at {a.profdata} -- produce one with "
                 f"tools/coldcode/run.sh")

    lines, fns = load_lcov(a.object, a.profdata, covtool)

    # ---- the apparatus, before any number -------------------------------
    bad = control_check(lines, fns)
    if bad:
        print("coldcode/whatran: REFUSING TO REPORT -- the control did not hold.")
        print("Nothing below this line would have been evidence about your change;")
        print("it would have been evidence about this tool.")
        for b in bad:
            print("  * " + b)
        return 1
    print(control_summary(lines, fns))

    if a.self_test:
        print("\nself-test: both halves of the control answered correctly.")
        print("The instrument can tell a line that ran from a line that did not,")
        print("INSIDE THE SAME FUNCTION, on this binary and this profile.")
        return 0

    # ---- what changed -----------------------------------------------------
    if a.diff:
        text = sys.stdin.read() if a.diff == "-" else open(a.diff, encoding="utf-8",
                                                           errors="replace").read()
        changed = parse_diff(text)
        what = f"the diff in {a.diff}"
    elif a.rev:
        p = subprocess.run(["git", "-C", REPO, "diff", a.rev, "--"],
                           capture_output=True, text=True)
        if p.returncode != 0:
            sys.exit("coldcode/whatran: git diff failed:\n" + p.stderr[:800])
        changed = parse_diff(p.stdout)
        what = f"git diff {a.rev}"
    else:
        changed = {}
        for f in a.file:
            rel = os.path.relpath(os.path.abspath(f), REPO)
            try:
                n = sum(1 for _ in open(os.path.join(REPO, rel), "rb"))
            except OSError as e:
                sys.exit(f"coldcode/whatran: {e}")
            changed[rel] = set(range(1, n + 1))
        what = f"{len(a.file)} whole file(s)"

    noncode = sorted(f for f in changed if not f.endswith(CODE_EXT))
    for f in noncode:
        changed.pop(f)
    if not changed:
        print(f"\ncoldcode/whatran: {what} adds no lines to any C translation "
              f"unit. Nothing to say.")
        if noncode:
            print(f"  ({plural(len(noncode), 'non-C file')} in the diff, skipped: "
                  f"{', '.join(noncode[:6])}{' ...' if len(noncode) > 6 else ''})")
        return 0

    # ---- SOURCE DRIFT: the check that matters most for a diff tool ---------
    built_at = max(os.path.getmtime(o) for o in a.object)
    drifted = []
    for f in sorted(changed):
        p = os.path.join(REPO, f)
        if os.path.exists(p) and os.path.getmtime(p) > built_at:
            drifted.append((f, os.path.getmtime(p) - built_at))
    if drifted:
        print("\ncoldcode/whatran: REFUSING TO REPORT -- SOURCE DRIFT.")
        print("These files were edited AFTER the instrumented probe was linked, so")
        print("the profile describes a DIFFERENT version of them. The line numbers")
        print("below would not merely be stale, they would be MISALIGNED -- this")
        print("tool would confidently name the wrong function. That is the failure")
        print("CLAUDE.md rule 1 calls 'is the file it read the file you edited?'")
        for f, dt in drifted:
            print(f"    {f}   edited {dt / 60:.0f} min after the link")
        print("\nSettle it -- rebuild the probe and re-run the corpus:")
        print("    make cold-corpus            (quick: the captured-site fixtures)")
        print("    make cold-corpus COLD_FULL=1 (adds jsfb and WPT; ~40 min)")
        return 1

    note = ""
    if a.corpus_note and os.path.exists(a.corpus_note):
        note = open(a.corpus_note, encoding="utf-8", errors="replace").read().strip()
    print(f"\nprofile: {a.profdata}")
    if note:
        for l in note.splitlines():
            print("  " + l)

    # ---- the answer -------------------------------------------------------
    unmapped, results = [], []
    for f in sorted(changed):
        if f not in lines:
            unmapped.append(f)
            continue
        da = lines[f]
        ranges = fn_ranges(fns.get(f, []))
        ran, cold, nocode = [], [], []
        for n in sorted(changed[f]):
            if n not in da:
                nocode.append(n)
            elif da[n] > 0:
                ran.append(n)
            else:
                cold.append(n)
        byfn = {}
        for n in cold:
            r = fn_of(ranges, n)
            key = (r[2].split(":", 1)[-1], r[0]) if r else ("(file scope)", 0)
            byfn.setdefault(key, []).append(n)
        results.append((f, len(changed[f]), ran, cold, nocode, byfn))

    if noncode:
        print(f"\n{plural(len(noncode), 'non-C file')} in the diff skipped -- only a C "
              f"translation unit can be in a coverage map:")
        print("  " + ", ".join(noncode[:8]) + (" ..." if len(noncode) > 8 else ""))

    print("\n" + "=" * 74)
    tot_ch = sum(r[1] for r in results) + sum(len(changed[f]) for f in unmapped)
    tot_code = sum(len(r[2]) + len(r[3]) for r in results)
    tot_cold = sum(len(r[3]) for r in results)
    print(f"{what}: {tot_ch} changed lines in "
          f"{len(results) + len(unmapped)} file(s).")
    if results:
        print(f"{tot_code} of them carry executable code. "
              f"{tot_code - tot_cold} RAN. {tot_cold} NEVER RAN.")
    print("=" * 74)

    for f, nch, ran, cold, nocode, byfn in results:
        code = len(ran) + len(cold)
        print(f"\n{f}")
        if code == 0:
            print(f"  {nch} changed lines, NONE of which carry executable code")
            print("  (blank, comment, declaration, close brace). This tool has no")
            print("  opinion on your change -- there is nothing here to execute.")
            continue
        if not ran and code >= KLAXON_MIN:
            # The sharpest case, and it gets the loudest voice available.
            print("  " + "#" * 70)
            print(f"  #  NOTHING YOU WROTE IN {os.path.basename(f)} RAN.")
            print(f"  #  {code} of your {nch} changed lines carry executable code and")
            print("  #  ZERO of them were entered by anything in this corpus.")
            print("  #  You have built a thing nothing reaches. Before doing")
            print("  #  anything else, find out which: is it unreachable by")
            print("  #  construction (a macro nothing defines, a keycode no key")
            print("  #  produces, a call site that does not exist), or is it merely")
            print("  #  absent from the corpus -- and if so, name the page that")
            print("  #  would reach it and add that page.")
            print("  " + "#" * 70)
        elif not ran:
            print(f"  Of the {plural(nch, 'line')} you changed here, {code} carry "
                  f"executable code, and NONE of them ran.")
            print(f"  (Under the {KLAXON_MIN}-line threshold for the loud version; "
                  f"still zero.)")
        else:
            print(f"  Of the {plural(nch, 'line')} you changed here, {code} carry "
                  f"executable code.")
            print(f"  {len(ran)} RAN.  {len(cold)} NEVER RAN.")
        if cold:
            print(f"\n  The {plural(len(cold), 'line')} that never ran "
                  f"{'is' if len(cold) == 1 else 'are'} in:")
            rows = sorted(byfn.items(), key=lambda kv: -len(kv[1]))
            for (nm, start), ls in rows[:a.top]:
                print(f"    {len(ls):>4} {'line ' if len(ls) == 1 else 'lines'}  "
                      f"{nm:<34} {f}:{start}   at {span_of(ls)}")
            if len(rows) > a.top:
                print(f"    ... and {len(rows) - a.top} more function(s)")
        if nocode:
            print(f"\n  ({plural(len(nocode), 'changed line')} carry no executable "
                  f"code -- blank, comment,")
            print("   declaration, close brace. Not counted above, and not a finding.)")

    if unmapped:
        print("\n" + "!" * 74)
        print("NOT IN THE COVERAGE MAP -- this instrument has NOTHING to say about")
        print("your change to these files, WHICH IS NOT THE SAME AS 'IT NEVER RAN':")
        for f in unmapped:
            print(f"    {f}   ({len(changed[f])} changed lines)")
        print("")
        print("The probe did not link them. A coverage report over a binary that")
        print("did not link the file reports every line in it cold, and that number")
        print("would be about the harness, not about your code -- CLAUDE.md rule 1.")
        print("Either the file is kernel-side / ring-0 (this probe is a HOST build of")
        print("the browser and the c/lib TUs it links, so browser.c, wm.c, fb.c and")
        print("everything under c/kernel are legitimately outside it), or it is a new")
        print("TU that no probe source list names yet -- which is itself worth")
        print("knowing, and is the fifth of this tree's rules: linking a translation")
        print("unit is not running it, and NOT linking it is not measuring it.")
        print("!" * 74)

    print("\n" + LIMIT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
