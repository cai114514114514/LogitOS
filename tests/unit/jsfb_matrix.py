#!/usr/bin/env python3
"""jsfb_matrix -- drive every js-framework-benchmark implementation, rank the causes.

    python3 tests/unit/jsfb_matrix.py build/webapi_probe
    ... --root build/jsfb          where the corpus is (default)
    ... --baseline tests/jsfb/BASELINE     assert instead of merely reporting
    ... --only vanillajs,halogen           a substring filter, for iterating

WHAT THIS IS FOR, AND WHAT IT IS NOT FOR.  It is not a benchmark.  Absolute
timings on this machine are worthless -- QEMU/TCG, x86_64 guest on an arm64
host, no acceleration, a contended host -- and no number below is a duration.
It is a CONFORMANCE MATRIX: thirty independent authors wrote thirty
implementations of ONE specified application, so a failure that appears in
twenty of them is a property of the platform underneath and a work order with
an address.  Decisively, it cannot be fitted to: you cannot special-case thirty
independent codebases into working.

THE UNIT OF THE OUTPUT IS THE CAUSE, COUNTED IN IMPLEMENTATIONS.  That is
tests/unit/framework_rank.py's rule and it transfers unchanged: "N of 30 die on
X" is a work order, "solarite fails 6 operations" is a symptom with no address.

NO FIXTURES ARE GENERATED.  The probe is pointed at the corpus on disk, with
`--docroot=<corpus>` so URL `/` names the corpus root exactly as upstream's own
server does, and `NAME=PATH` so each row is labelled with its implementation
rather than with its leaf directory (two implementations' documents are both in
a directory called `bundled-dist`).  An earlier version of this line generated a
fixture tree with a manifest per implementation; that was a second door onto the
same jar and it is gone.  Which implementations exist, which need no npm build,
and where each one's document is are all asked of tests/unit/jsfb_corpus.py --
the one reader, which tools/jsfb_fetch.sh also asks.

THE CONTROL RUNS FIRST AND ITS RESULT GATES THE REPORT.
The control is BUILT here, from the corpus: the reference implementation's own
index.html with its <script> elements deleted.  Same skeleton, same six button
ids, same table, and provably no behaviour -- which is the only combination that
lets a failure be attributed to absent behaviour rather than to an absent page.
Three of the eight operations -- run, add, runlots -- are defined from any state
and so have no precondition; all three must FAIL on it.  If ANY operation passes
there, this file prints the control's row and refuses to print the matrix,
because a driver that reports a page with no code in it as working cannot report
anything.  See the fifth of the rules in CLAUDE.md, and the live examples in
this tree of controls that pass for the wrong reason.

NOTE THAT `keyed/no-js` IS NOT THAT CONTROL, despite its name.  It is No.JS
v1.20.0, a 136 KB HTML-first reactive framework whose src/main.js registers
three globals with it.  It appears in the matrix as an ordinary row.

ONE PROBE PROCESS PER IMPLEMENTATION, for the reason framework_rank.run_paint()
gives: the driver's report lines cannot name themselves, so with several
fixtures in one process the only thing tying a line to an implementation is
output order, and a report that attributes one page's result to another is worse
than no report.
"""

import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jsfb_corpus                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = os.path.join(HERE, "jsfb_drive.js")

OPS = ["run", "runlots", "add", "update", "select", "swaprows", "remove", "clear"]
# The order the driver runs them in, which is not the order they are printed in.
DRIVE_ORDER = ["run", "update", "select", "swaprows", "remove", "add", "clear", "runlots"]

# The implementation the control's skeleton is taken from: the specification's
# own reference implementation. Named as data because it is a choice.
CONTROL_REF = "keyed/vanillajs"
CONTROL_NAME = "_control/noscript"

JSFB_RE = re.compile(r"^#JSFB\t([^\t]*)\t([^\t]*)\t([^\t]*)\t(.*)$")
UNCAUGHT_RE = re.compile(r"^\s+(\S+)\s+\.\.\. UNCAUGHT: (\d+)")
NOTIN_RE = re.compile(r"^\s+(\S+)\s+\.\.\. NOT IN FIXTURE \(([^)]*)\): (.*)$")
DRIVERTHREW_RE = re.compile(r"^\s+(\S+)\s+\.\.\. DRIVER THREW: (.*)$")
SCRIPTS_RE = re.compile(r"^\s+(\S+)\s+(\d+) scripts \(")
JSON_RE = re.compile(r"^#JSON\t([^\t]*)\t([^\t]*)\t([^\t]*)\t(.*)$")

SCRIPT_EL = re.compile(rb"<script\b.*?</script\s*>", re.I | re.S)

# Per-implementation wall clock ceiling. NOT a performance measurement -- it is
# the difference between "this implementation does not finish" and "this run
# hung and took the whole matrix with it". A hit is reported as TIMEOUT, never
# as a set of failed operations.
#
# IT IS ALSO THE ONE VERDICT IN THIS FILE THAT CAN FLAP, and saying so is
# cheaper than discovering it. Every other verdict is a DOM assertion and does
# not depend on the clock; a TMO row depends on how loaded the host is, and
# tools/perf/'s rule -- "the host is contended, other agents run QEMU
# concurrently, so host wall clock is worthless here" -- applies to it and to
# nothing else here. If a baseline diff shows only a TMO row moving, re-run it
# alone before believing it:
#     JSFB_TIMEOUT=900 python3 tests/unit/jsfb_matrix.py build/webapi_probe \
#         --only binding.scala
TIMEOUT = int(os.environ.get("JSFB_TIMEOUT", "300"))


class Result(object):
    def __init__(self, name):
        self.name = name
        self.env = {}
        self.ops = {}          # op -> (verdict, detail)
        self.uncaught = 0
        self.gaps = []
        self.driver_threw = None
        self.timeout = False
        self.scripts = None
        self.errors = []       # (kind, where, msg) from the probe's --json ledger

    def first_error(self):
        """The page's own first uncaught failure. `netstub` is the host build
        having no network and `fetch` is a corpus gap -- neither is the engine,
        and webapi_probe.c already counts them apart for that reason. `warn` is
        kept, because a page that CAUGHT its failure and warned about it is
        reporting a gap of ours that produces no exception at all."""
        for kind, where, msg in self.errors:
            if kind in ("netstub", "fetch"):
                continue
            return "%s%s" % ("warn: " if kind == "warn" else "", msg)
        return None

    @property
    def npass(self):
        return sum(1 for o in self.ops.values() if o[0] == "PASS")

    def first_break(self):
        """The first operation in DRIVE ORDER that did not pass. A cascade has
        one cause and seven consequences; this is the cause."""
        for op in DRIVE_ORDER:
            v = self.ops.get(op)
            if v and v[0] != "PASS":
                return op, v[0], v[1]
        return None


def run_one(probe, name, docdir, docroot):
    r = Result(name)
    cmd = [probe]
    if docroot:
        cmd.append("--docroot=" + docroot)
    # --json makes the probe emit one `#JSON` line per ledger entry, which is
    # how the cause table gets the page's OWN error message. Without it every
    # failure of `run` collapses into "rows=0 want=1000" -- a bucket of eight
    # that looked like one bug and was five (a missing FileList, a `.children`
    # of undefined, and three others). A cause table that cannot tell those
    # apart is the thing framework_rank.py's header warns about: it stays
    # comfortably short by losing the distinctions that were the work order.
    cmd += ["--json", "--drive", DRIVE, "%s=%s" % (name, docdir)]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                             timeout=TIMEOUT)
        text = (out.stdout or "") + (out.stderr or "")
    except subprocess.TimeoutExpired as e:
        # THE PARTIAL OUTPUT IS KEPT, and that is the difference between a
        # finding and a shrug. A row reading TIMEOUT on all eight says only
        # "something took too long"; one reading `run ok, runlots never
        # returned` names the operation, and the operation is the address.
        r.timeout = True
        text = ""
        for part in (e.stdout, e.stderr):
            if part:
                text += part if isinstance(part, str) else part.decode("utf8", "replace")
    for line in text.replace("\x00", "").split("\n"):
        line = line.rstrip("\r")
        m = JSFB_RE.match(line)
        if m:
            kind, a, b, c = m.groups()
            if kind == "env":
                r.env = {"buttons": a, "tbody": b, "rows0": c}
            elif kind == "op":
                r.ops[a] = (b, c)
            elif kind == "done":
                r.env["done"] = "%s %s" % (b, c)
            elif kind == "disagree":
                r.env.setdefault("disagree", []).append("%s vs %s" % (a, b))
            continue
        m = JSON_RE.match(line)
        if m:
            r.errors.append((m.group(2), m.group(3), m.group(4)))
            continue
        for rx, fn in ((UNCAUGHT_RE, lambda m: setattr(r, "uncaught", int(m.group(2)))),
                       (NOTIN_RE, lambda m: r.gaps.append("%s %s" % (m.group(2), m.group(3)))),
                       (DRIVERTHREW_RE, lambda m: setattr(r, "driver_threw", m.group(2))),
                       (SCRIPTS_RE, lambda m: setattr(r, "scripts", int(m.group(2))))):
            m = rx.match(line)
            if m:
                fn(m)
                break
    return r


def build_control(root, out):
    """The null control, DERIVED from the corpus: the reference implementation's
    own index.html with every <script> element deleted.

    It is written outside the corpus (never mutate fetched data) and probed
    WITHOUT --docroot, which it does not need -- it has nothing to fetch. The
    result is checked rather than trusted: if a <script> survived the strip this
    refuses, because a control with code in it is not a null control."""
    imp = [i for i in jsfb_corpus.impls(root) if i["name"] == CONTROL_REF]
    if not imp or not imp[0]["doc"]:
        return None
    with open(imp[0]["doc"], "rb") as f:
        html = f.read()
    stripped = SCRIPT_EL.sub(b"", html)
    if re.search(rb"<script\b", stripped, re.I):
        raise SystemExit("jsfb_matrix: the control still contains a <script> after "
                         "stripping; it would not be a null control")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "index.html"), "wb") as f:
        f.write(stripped)
    return out


# The second control's page: 1,000 conforming rows at load, and NO handler.
# Written as data rather than lifted from an implementation, because every
# implementation in the corpus binds handlers and the whole point of this page
# is that it does not. It uses only the markup the specification fixes -- the
# id cell, the label cell with its <a>, the remove cell -- so a driver that
# reads rows at all reads these.
STATIC_FILL = """
<script>
(function () {
  var t = document.querySelector("table.test-data tbody");
  if (!t) return;
  var h = "";
  for (var i = 1; i <= 1000; i++)
    h += "<tr><td class='col-md-1'>" + i + "</td>"
       + "<td class='col-md-4'><a class='lbl'>static row " + i + "</a></td>"
       + "<td class='col-md-1'><a class='remove'><span class='glyphicon "
       + "glyphicon-remove'></span></a></td><td class='col-md-6'></td></tr>";
  t.innerHTML = h;
})();
</script>
"""


def build_static_control(root, out):
    """THE SECOND CONTROL, and it exists because the first one cannot see the
    hole it covers.

    The null control starts at zero rows, so `run` fails there for the count
    alone -- and an assertion of "1,000 rows afterwards" would have been
    satisfied by a page that was BORN with 1,000 rows and ignored the click.
    Nothing in the matrix could have distinguished "the button worked" from
    "the button was never needed", and the null control would have stayed green
    the whole time, which is the failure mode CLAUDE.md's fifth rule is about.

    This page is the reference skeleton with its <script> elements deleted --
    the same strip, so the same provable absence of behaviour -- plus one
    inline script that renders 1,000 conforming rows and binds NOTHING. `run`
    must be watched FAILING on it. Every other operation is expected to fail
    too (nothing responds to anything), but only `run` is asserted, because
    only `run` is the one this page was built to catch.

    NOTE WHAT IS *NOT* CLAIMED: this bounds the "the rows were already there"
    shape. It does not bound "the rows appeared in response to something other
    than my click" -- a load-time timer, an observer, a microtask. That would
    need a page that fills the table on a delay, and it is not built."""
    imp = [i for i in jsfb_corpus.impls(root) if i["name"] == CONTROL_REF]
    if not imp or not imp[0]["doc"]:
        return None
    with open(imp[0]["doc"], "rb") as f:
        html = f.read()
    stripped = SCRIPT_EL.sub(b"", html)
    if re.search(rb"<script\b", stripped, re.I):
        raise SystemExit("jsfb_matrix: the static control's skeleton still contains "
                         "a <script> after stripping")
    # Appended at the end of <body> so the table exists when it runs. If the
    # document has no </body> (it does; the check is here so a corpus that
    # changes shape refuses rather than silently producing an empty control).
    low = stripped.lower()
    i = low.rfind(b"</body>")
    if i < 0:
        return None
    doc = stripped[:i] + STATIC_FILL.encode("utf8") + stripped[i:]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "index.html"), "wb") as f:
        f.write(doc)
    return out


CELL = {"PASS": "ok", "FAIL": "XX", "NOPRE": " -"}


def print_matrix(results):
    print("%-26s %3s %s %6s %s" % ("IMPLEMENTATION", "P/8",
                                   " ".join("%-4s" % o[:4] for o in OPS),
                                   "UNCGT", "NOTE"))
    print("%-26s %3s %s %6s %s" % ("-" * 26, "---",
                                   " ".join("----" for _ in OPS), "-----", "----"))
    for r in results:
        cells = " ".join("%-4s" % CELL.get(r.ops.get(o, ("?", ""))[0],
                                           "TMO" if r.timeout else "??") for o in OPS)
        if r.timeout:
            stopped = next((o for o in DRIVE_ORDER if o not in r.ops), "(load)")
            print("%-26s %3d %s %6s %s" % (r.name, r.npass, cells, "?",
                                           "TIMEOUT after %ds, stopped in `%s`"
                                           % (TIMEOUT, stopped)))
            continue
        note = ""
        if not r.ops:
            note = "no driver output"
        elif r.driver_threw:
            note = "driver threw: " + r.driver_threw[:40]
        elif not r.env.get("buttons", "").startswith("buttons=6/6"):
            note = "shell never rendered (%s)" % r.env.get("buttons", "?")
        if r.gaps:
            note = (note + " " if note else "") + "CORPUS GAP: " + "; ".join(r.gaps)[:50]
        print("%-26s %3d %s %6d %s" % (r.name, r.npass, cells, r.uncaught, note))


def rank_causes(results):
    """The first non-passing operation, with the reason, counted in
    implementations. A message is normalised only by stripping the numbers and
    quoted strings that vary between runs -- an over-eager normaliser is how a
    cause table stays comfortably short, so anything unrecognised keeps its own
    row and is never dropped."""
    buckets = {}
    for r in results:
        if r.timeout:
            stopped = next((o for o in DRIVE_ORDER if o not in r.ops), "(load)")
            key = ("(timeout)", "did not finish in %ds; stopped in `%s` after %d passing"
                   % (TIMEOUT, stopped, r.npass))
        elif r.gaps:
            # A FILE THE CORPUS DOES NOT HOLD IS NOT AN ENGINE FAILURE, and
            # mixing the two is the single easiest way for this instrument to
            # lie -- webapi_probe.c says so in its own words about a 404'd
            # module: "an import that 404s makes a page look broken in a way the
            # browser is not responsible for". Its own bucket, out of the
            # ranking of causes the browser owns.
            key = ("(corpus gap)", "; ".join(r.gaps)[:100])
        else:
            fb = r.first_break()
            if fb is None:
                continue
            op, verdict, detail = fb
            # THE PAGE'S OWN MESSAGE WINS OVER THE DRIVER'S OBSERVATION when
            # there is one. "rows=0 want=1000" is what the driver saw; "'FileList'
            # is not defined" is the address. Only when the page threw nothing at
            # all does the assertion text become the cause -- and that case is
            # itself a finding: an application that produced no error and no rows
            # is a silent failure, which is the hardest class to chase.
            err = r.first_error()
            if err:
                d = "throws: " + re.sub(r"\b\d+\b", "N", err)
            else:
                d = re.sub(r"\b\d+\b", "N", detail)
                d = re.sub(r'"[^"]*"', '"..."', d)
                d = "silent -- " + d
            key = (op, d[:110])
        buckets.setdefault(key, []).append(r.name)
    rows = sorted(buckets.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    print()
    print("WHAT BREAKS FIRST -- the cause, counted in implementations")
    print("  (the first operation in drive order that did not pass; whatever is")
    print("   behind it is a consequence of it, not a separate finding)")
    print()
    print("%5s  %-13s %s" % ("N", "OP", "REASON / WHICH"))
    print("%5s  %-13s %s" % ("-----", "-------------", "-" * 50))
    for (op, detail), names in rows:
        print("%5d  %-13s %s" % (len(names), op, detail))
        print("%5s  %-13s   %s" % ("", "", ", ".join(sorted(names))))


def main():
    a = sys.argv[1:]
    baseline = only = None
    root = jsfb_corpus.DEFAULT_ROOT
    # OFF BY DEFAULT, AND THE DEFAULT IS THE POINT. The 30 build-free
    # implementations are the population tests/jsfb/BASELINE was taken over,
    # and they are the only ones whose presence does not depend on somebody
    # having run npm on this host. Letting a build change the ROW SET of a
    # change detector would mean the baseline records the state of a working
    # directory rather than the state of the browser.
    #
    # `--include-built` opts in to every implementation that has a document on
    # disk, which after `python3 tests/unit/jsfb_buildcost.py <names>` is the
    # built ones too. It is measured at ~2 s and ~70 MB of node_modules per
    # implementation to get there (see that file's header), so this is the
    # flag that turns a 30-row matrix into a 199-row one -- and it REFUSES to
    # combine with --baseline, because a baseline over a row set that depends
    # on what somebody built this morning is not a baseline.
    include_built = "--include-built" in a
    if include_built:
        a.remove("--include-built")
    for flag in ("--baseline", "--root", "--only"):
        if flag in a:
            i = a.index(flag)
            v = a[i + 1]
            del a[i:i + 2]
            if flag == "--baseline":
                baseline = v
            elif flag == "--root":
                root = v
            else:
                only = [s for s in v.split(",") if s]
    if len(a) != 1:
        print("usage: jsfb_matrix.py <probe-binary> [--root DIR] [--only SUBSTR,...] "
              "[--baseline FILE] [--include-built]")
        return 2
    probe = a[0]

    if include_built and baseline:
        print("jsfb_matrix: REFUSING -- --include-built with --baseline.")
        print("  The baseline is a change detector over a FIXED row set (the")
        print("  build-free implementations). --include-built makes the row set")
        print("  depend on which implementations happen to be built on this")
        print("  host, so a mismatch would report somebody's npm run rather")
        print("  than a change in the browser. Run them separately.")
        return 2

    # SKIP LOUDLY. CLAUDE.md: a gate that cannot run on this host must name the
    # missing capability and the command that would settle it, and never pass
    # silently.
    if not os.path.isdir(os.path.join(root, "frameworks")):
        print("jsfb_matrix: SKIP -- no corpus at %s" % root)
        print("  settle it with:  make jsfb-fetch")
        return 0
    if not os.path.exists(probe):
        print("jsfb_matrix: SKIP -- no probe binary at %s" % probe)
        print("  settle it with:  make %s" % probe)
        return 0

    impls = [i for i in jsfb_corpus.impls(root)
             if i["build_free"] or include_built]
    nodoc = [i["name"] for i in impls if not i["docdir"]]
    impls = [i for i in impls if i["docdir"]]
    if only:
        impls = [i for i in impls if any(s in i["name"] for s in only)]

    # ---- the control, first, and watched failing.
    cdir = build_control(root, os.path.join(os.path.dirname(root.rstrip("/")) or ".",
                                            "jsfb-control"))
    if not cdir:
        print("jsfb_matrix: REFUSING -- could not build the null control from %s."
              % CONTROL_REF)
        print("  Without one, nothing in the matrix can be believed.")
        return 1
    ctl = run_one(probe, CONTROL_NAME, cdir, None)
    print("THE NULL CONTROL, RUN FIRST -- %s's own index.html with its <script>" % CONTROL_REF)
    print("elements deleted: corpus furniture, provably no behaviour (%s)" % cdir)
    print("  scripts loaded: %s   buttons: %s   %s" %
          (ctl.scripts, ctl.env.get("buttons", "?"), ctl.env.get("tbody", "?")))
    for op in DRIVE_ORDER:
        v = ctl.ops.get(op, ("(none)", ""))
        print("  %-9s %-5s %s" % (op, v[0], v[1]))
    if not ctl.ops:
        print()
        print("jsfb_matrix: REFUSING -- the control produced no driver output at all,")
        print("  so it did not fail, it never ran. That is not a control.")
        return 1
    if ctl.npass:
        print()
        print("jsfb_matrix: REFUSING TO PRINT A MATRIX. The null control passed %d"
              % ctl.npass)
        print("  operation(s) above. A page with no code in it cannot create a row,")
        print("  so the driver is measuring itself and every other row is void.")
        return 1
    watched = [o for o in ("run", "add", "runlots") if ctl.ops.get(o, ("", ""))[0] == "FAIL"]
    if len(watched) != 3:
        print()
        print("jsfb_matrix: REFUSING -- only %d of the 3 precondition-free operations"
              % len(watched))
        print("  were watched FAILING on the control (%s). The others reported NOPRE,"
              % ", ".join(watched) or "none")
        print("  which means the control is not exercising them at all.")
        return 1
    print("  -> control ok: 0 passed, and all 3 precondition-free operations (run,")
    print("     add, runlots) were watched FAILING. The rest are NOPRE: a cascade,")
    print("     not a pass.")
    print()

    # ---- the SECOND control, and it catches what the first cannot.
    sdir = build_static_control(root, os.path.join(
        os.path.dirname(root.rstrip("/")) or ".", "jsfb-control-static"))
    if not sdir:
        print("jsfb_matrix: REFUSING -- could not build the static control from %s."
              % CONTROL_REF)
        return 1
    sctl = run_one(probe, "_control/static", sdir, None)
    print("THE SECOND CONTROL -- the same skeleton, plus a script that renders 1,000")
    print("conforming rows AT LOAD and binds no handler (%s)." % sdir)
    print("It exists because `rows==1000 afterwards` is also true of a page that was")
    print("BORN with them: `run` must be watched FAILING here or a PASS anywhere in")
    print("the matrix could mean the click was never needed.")
    srun = sctl.ops.get("run", ("(none)", ""))
    print("  rows at load: %s" % sctl.env.get("rows0", "?"))
    print("  run       %-5s %s" % (srun[0], srun[1]))
    if sctl.env.get("rows0") != "rows0=1000":
        print()
        print("jsfb_matrix: REFUSING -- the static control loaded with %s, not 1000."
              % sctl.env.get("rows0", "nothing"))
        print("  It cannot test the property it exists for. This is the control")
        print("  itself being broken, NOT a finding about the browser -- most")
        print("  likely innerHTML or the table markup; run the probe on %s." % sdir)
        return 1
    if srun[0] != "FAIL":
        print()
        print("jsfb_matrix: REFUSING TO PRINT A MATRIX. `run` reported %s on a page"
              % srun[0])
        print("  whose 1,000 rows were there before any click and which binds no")
        print("  handler at all. Every `run ok` below would then be unattributable:")
        print("  it could mean the button worked or that it was never needed.")
        return 1
    print("  -> second control ok: `run` watched FAILING on 1,000 rows nobody clicked for.")
    print()

    results = [run_one(probe, i["name"], i["docdir"], root) for i in impls]

    print("THE MATRIX -- %d implementations, %d operations each" % (len(results), len(OPS)))
    print("  ok = the specified assertion held   XX = it did not")
    print("   - = NOPRE: a previous operation left the precondition unmet (a cascade)")
    print("  UNCGT = uncaught exceptions the page produced during load AND drive")
    if nodoc:
        print("  %d %s implementations have no document and are not rows: %s"
              % (len(nodoc),
                 "considered" if include_built else "build-free",
                 ", ".join(nodoc)))
    print()
    print_matrix(results)

    total = len(results)
    full = sum(1 for r in results if r.npass == len(OPS))
    any_pass = sum(1 for r in results if r.npass)
    print()
    print("%d of %d implementations complete all %d operations; %d complete at least one."
          % (full, total, len(OPS), any_pass))

    rank_causes(results)

    if baseline:
        # ONE CELL PER OPERATION, COMMA SEPARATED. The first version of this
        # concatenated `ok` / `XX` / `-` with no separator, which made
        # `XXXXXX----XX` -- a string in which the boundaries between cells are
        # not recoverable, so a diff could say a line moved and not which
        # operation moved. A baseline you cannot read a diff of is a baseline
        # nobody will update correctly.
        cell = {"PASS": "ok", "FAIL": "XX", "NOPRE": "--"}
        got = ["control %d" % ctl.npass,
               "control-static-run %s" % srun[0]]
        got += ["%s %d %s" % (r.name, r.npass,
                              ",".join("TMO" if (r.timeout and o not in r.ops)
                                       else cell.get(r.ops.get(o, ("?", ""))[0], "??")
                                       for o in OPS))
                for r in results]
        if not os.path.exists(baseline):
            print("\njsfb_matrix: no baseline at %s -- writing one." % baseline)
            os.makedirs(os.path.dirname(baseline) or ".", exist_ok=True)
            with open(baseline, "w") as f:
                f.write("\n".join(got) + "\n")
            return 0
        # `#` lines are provenance, not data: the baseline has to be able to say
        # WHICH browser it was taken against, because the corpus is pinned and
        # the engine is not.
        with open(baseline) as f:
            want = [l.rstrip("\n") for l in f
                    if l.strip() and not l.lstrip().startswith("#")]
        if want != got:
            print("\njsfb_matrix: BASELINE MOVED. This is a change detector, not a")
            print("  wish list -- update %s in the same commit and say WHICH CAUSE moved."
                  % baseline)
            n = max(len(want), len(got))
            for w, g in zip(want + [""] * (n - len(want)), got + [""] * (n - len(got))):
                if w != g:
                    print("    was: %s\n    now: %s" % (w, g))
            return 1
        print("\njsfb_matrix: matches %s" % baseline)
    return 0


if __name__ == "__main__":
    sys.exit(main())
