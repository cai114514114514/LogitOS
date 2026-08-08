#!/usr/bin/env python3
"""tools/wpt_rank.py -- turn a WPT failure list into a WORK ORDER.

    make wpt-rank                      the table
    python3 tools/wpt_rank.py <report.tsv> [--top N] [--cause SUBSTRING]

`make test-wpt` prints a rate. A rate says where you are; it does not say what
to do next. This groups every failing subtest by the CAUSE its message names --
a missing global, a property read off undefined, a wrong computed value -- and
ranks the causes by how many subtests each one takes out. That ranking is the
thing this layer has never had: the DOM and the Web APIs were previously
debugged by loading a real site and reading a stack, one site at a time.

Two columns exist because the ranking has to be actionable, not just sorted:

  LAYER  which of the four owners a failure belongs to. For CSS especially --
         parsing, cascade, computed value, layout -- because a testharness test
         that fails on a computed value and a reftest that fails on a
         transform have nothing to do with each other and are fixed by
         different people in different files.

  SITES  which of the fifteen real-site failures the user reported this cause
         plausibly explains. That list is the VALIDATION SET. A high pass rate
         that explains none of them is a failure of this exercise, not a
         success: this project once went from 2/1643 to 1723/1818 on HTML tree
         construction while the browser still could not run one real site's
         JavaScript, and that is the trap this column exists to avoid.

The input is the runner's --report TSV: status, path, subtest, message.
"""
import sys, re, collections

# --- the validation set ----------------------------------------------------
# The fifteen failures the user collected by hand across real sites. Each entry
# is (symptom, sites, [regexes that would produce it]). A cause is credited
# with a site when the failure it names is the same shape as the site's error,
# NOT merely when it is in the same file -- "plausibly explains" is a claim
# about mechanism.
SITE_FAILURES = [
    ("HTMLBodyElement is not defined", "white-screen search page",
     [r"\bHTML\w*Element\b", r"\bNode\b", r"\bCharacterData\b", r"instanceof",
      r"NOT REFLECTED", r"interface", r"class string"]),
    ("jQuery is not defined", "jd.com",
     [r"\bjQuery\b", r"script (execution|order|error)", r"\bdocument\.write\b"]),
    # A property that reads `undefined` is what a framework then calls .charAt
    # / .split / .toString on, so the reflection gap is a direct producer of all
    # three of these site errors, not merely a neighbour of them.
    ("cannot read property 'charAt' of undefined", "doubao (webpack module 3454)",
     [r"charAt", r"of undefined", r"of null", r"NOT REFLECTED"]),
    ("cannot read property 'split' of undefined", "bilibili, apple",
     [r"\bsplit\b", r"of undefined", r"of null", r"NOT REFLECTED"]),
    ("cannot read property 'toString' of undefined", "2345",
     [r"toString", r"of undefined", r"NOT REFLECTED"]),
    ("cannot read property 'document' of undefined", "openai",
     [r"\bcontentDocument\b", r"\bcontentWindow\b", r"\bframes\b", r"\bdefaultView\b",
      r"\bownerDocument\b"]),
    ("Worker is not defined", "chat.deepseek",
     [r"\bWorker\b", r"\bSharedWorker\b"]),
    ("Uncaught (in promise) undefined", "qq",
     [r"\bpromise\b", r"unhandled", r"rejection"]),
    ("... is not a function", "anthropic",
     [r"is not a function", r"not a function"]),
    ("garbled titles (byte-truncated UTF-8)", "several",
     [r"\bUTF-8\b", r"TextDecoder", r"TextEncoder", r"\bencoding\b", r"surrogate"]),
    ("nothing loads at all", "douyin, github",
     [r"\bfetch\b", r"\bXMLHttpRequest\b", r"\bmodule\b", r"\bimport\b"]),
    ("ran script, no output", "weixin, stripe",
     [r"\bMutationObserver\b", r"\bcustomElements\b", r"\bappendChild\b",
      r"\binnerHTML\b", r"\bcreateElement\b"]),
]

# --- layer attribution -----------------------------------------------------
# Which owner a failing path belongs to. Order matters: first match wins.
LAYERS = [
    (r"^css/css-(cascade|variables)/",           "cascade"),
    (r"^css/cssom",                              "CSSOM"),
    (r"^css/css-syntax/",                        "CSS parsing"),
    (r"^css/(css-values|css-color|css-fonts)/",  "computed value"),
    (r"^css/(css-flexbox|css-grid|css-position|css-transforms|css-display|"
     r"css-box|css-sizing|css-align|css-overflow|css-text|css-inline)/", "layout"),
    (r"^css/css-pseudo/",                        "layout (pseudo-elements)"),
    (r"^css/",                                   "CSS (other)"),
    (r"^dom/events",                             "DOM events"),
    (r"^dom/ranges|^dom/traversal",              "DOM ranges/traversal"),
    (r"^dom/nodes",                              "DOM core"),
    (r"^dom/",                                   "DOM (other)"),
    (r"^html/dom/",                              "HTML DOM / reflection"),
    (r"^html/semantics/forms",                   "forms"),
    (r"^html/semantics/scripting",               "script execution"),
    (r"^html/semantics",                         "HTML elements"),
    (r"^encoding/",                              "encoding"),
    (r"^url/",                                   "URL"),
    (r"^console/",                               "console"),
]

def layer_of(path):
    for pat, name in LAYERS:
        if re.search(pat, path):
            return name
    return "?"

# --- cause extraction ------------------------------------------------------
# Each rule turns a message into a canonical cause. The point is to collapse
# 300 differently-worded failures into the ONE missing thing behind them, so
# the ranking counts causes and not phrasings.
CAUSE_RULES = [
    # A name the runtime does not have at all. The most actionable cause there
    # is: it names the exact identifier to add.
    (r"ReferenceError:\s*(\w+) is not defined",       lambda m: "missing global: %s" % m.group(1)),
    (r"\b(\w+) is not defined\b",                     lambda m: "missing global: %s" % m.group(1)),
    # A property read off a value that should have been an object. The property
    # name is what identifies the gap; the undefined is a symptom.
    (r"cannot read property '([^']+)' of (undefined|null)",
                                                      lambda m: "property read off %s: .%s" % (m.group(2), m.group(1))),
    (r"cannot convert (undefined|null) to object",    lambda m: "undefined where an object was required"),
    # A method that exists as a name but not as a callable.
    (r"([\w.]+) is not a function",                   lambda m: "not a function: %s" % m.group(1)),
    (r"\bnot a function\b",                           lambda m: "not a function (callee unnamed)"),
    (r"\bnot a constructor\b",                        lambda m: "not a constructor"),
    # The harness's own assertion vocabulary. assert_throws_* failing means an
    # error the spec requires is not being raised -- a different fix from a
    # wrong value, so a different bucket.
    (r"assert_throws_dom:.*",                         lambda m: "missing DOMException (assert_throws_dom)"),
    (r"assert_throws_js:.*",                          lambda m: "missing JS error (assert_throws_js)"),
    (r"assert_throws_exactly:.*",                     lambda m: "wrong thrown value (assert_throws_exactly)"),
    (r"assert_idl_attribute:.*",                      lambda m: "missing IDL attribute"),
    (r"assert_readonly:.*",                           lambda m: "attribute is not readonly"),
    (r"assert_own_property:.*expected property \"?([\w$]+)",
                                                      lambda m: "missing own property: %s" % m.group(1)),
    (r"assert_inherits:.*property \"?([\w$]+)",       lambda m: "missing prototype property: %s" % m.group(1)),
    (r"assert_class_string:.*expected \"?\[object ([\w]+)\]",
                                                      lambda m: "wrong class string (needs interface %s)" % m.group(1)),
    (r"assert_true: .*expected true got false",       lambda m: "assert_true failed"),
    (r"assert_false: .*expected false got true",      lambda m: "assert_false failed"),
    # html/dom/reflection-*: the single largest cause in the corpus, and it
    # collapses into "wrong value" unless it is split out. "IDL get ... got
    # (undefined)" means the element object has no such property AT ALL -- the
    # reflected IDL attribute surface (input.type, link.as, img.referrerPolicy,
    # ...) does not exist. Getting a value of the wrong TYPE is a different and
    # much smaller job than not having the property.
    (r"assert_equals: IDL get expected .* but got \(undefined\)",
                                                      lambda m: "IDL attribute NOT REFLECTED (property absent)"),
    (r"assert_equals: IDL get expected \((\w+)\).* but got \((\w+)\)",
                                                      lambda m: "IDL attribute reflected as %s, want %s" % (m.group(2), m.group(1))),
    (r"assert_equals: IDL get .*",                    lambda m: "IDL attribute reflects the wrong value"),
    (r"assert_equals: getAttribute\(\) .*",           lambda m: "getAttribute() returns the wrong string"),
    (r"assert_equals: hasAttribute\(\) .*",           lambda m: "hasAttribute() wrong"),
    (r"assert_not_equals: property should be set.*",  lambda m: "CSS property not settable through the CSSOM"),
    (r"assert_equals:.*expected \(undefined\) undefined",
                                                      lambda m: "value is undefined where a value was expected"),
    (r"assert_equals:.*",                             lambda m: "wrong value (assert_equals)"),
    (r"assert_array_equals:.*",                       lambda m: "wrong list (assert_array_equals)"),
    (r"assert_unreached:.*",                          lambda m: "unreachable code was reached"),
    (r"promise_test:.*",                              lambda m: "promise_test rejected"),
    (r"InternalError: interrupted|watchdog",          lambda m: "script never stopped running (watchdog)"),
    (r"harness never completed",                      lambda m: "harness never completed"),
    (r"completed with zero subtests",                 lambda m: "no subtests registered"),
    (r"uncaught:\s*(\w*Error)",                       lambda m: "uncaught %s at load" % m.group(1)),
]

def cause_of(msg):
    m = msg.strip()
    if not m:
        return "no message"
    for pat, fn in CAUSE_RULES:
        g = re.search(pat, m, re.I)
        if g:
            return fn(g)
    return m[:70]

# --- resolving an unnamed callee through the stack -------------------------
# QuickJS reports a failed call as a bare "not a function" with no callee name,
# and in the first dom/ run that ONE message accounted for 613 of 1816 failures.
# A bucket of 613 is not a work order. The stack does name a script and a line
# ("at <anonymous> (<inline 0>:29)"), so the callee can be recovered from the
# corpus itself: find the Nth inline <script> in the test file, count the lines
# before it, read the offending source line, and take the call on it.
#
# The harness's own vocabulary is filtered out because every one of these lines
# is inside a test() callback and would otherwise resolve to "test" or
# "assert_equals" every time.
HARNESS_NAMES = set("""test async_test promise_test promise_rejects_dom
    promise_rejects_js assert_equals assert_not_equals assert_true assert_false
    assert_array_equals assert_object_equals assert_throws_dom assert_throws_js
    assert_throws_exactly assert_unreached assert_in_array assert_regexp_match
    assert_own_property assert_inherits assert_idl_attribute assert_class_string
    assert_readonly assert_implements assert_implements_optional assert_approx_equals
    assert_less_than assert_greater_than assert_not_own_property step step_func
    step_func_done step_timeout done setup forEach map filter push String Number
    Array Object function if for while return typeof new catch switch""".split())

_srccache = {}

def _inline_scripts(root, relpath):
    """[(start_line, text)] for each inline <script> in the test file, in the
    same order collect_scripts() numbers them."""
    key = (root, relpath)
    if key in _srccache:
        return _srccache[key]
    out = []
    try:
        with open("%s/%s" % (root, relpath), encoding="utf-8", errors="replace") as f:
            src = f.read()
    except OSError:
        _srccache[key] = out
        return out
    for m in re.finditer(r"<script([^>]*)>(.*?)</script\s*>", src, re.S | re.I):
        attrs = m.group(1)
        if re.search(r"\bsrc\s*=", attrs, re.I):
            continue                    # external: numbered but not inline
        if re.search(r'type\s*=\s*["\']?(?!text/javascript|application/javascript|module)',
                     attrs, re.I):
            continue                    # a data block, not a script
        start = src.count("\n", 0, m.start(2)) + 1
        out.append((start, m.group(2)))
    _srccache[key] = out
    return out

def callee_from_stack(root, relpath, stack):
    """The method name behind a bare 'not a function', or None."""
    m = re.search(r"at [^(]*\((<inline (\d+)>|[^:)]+):(\d+)\)", stack or "")
    if not m:
        return None
    line_no = int(m.group(3))
    if m.group(2) is not None:
        scripts = _inline_scripts(root, relpath)
        idx = int(m.group(2))
        # collect_scripts numbers ALL scripts, inline and external, in one
        # sequence; the inline list here is a subsequence. Index defensively.
        if idx >= len(scripts):
            idx = len(scripts) - 1
        if idx < 0:
            return None
        base, text = scripts[idx]
        lines = text.split("\n")
        if not (1 <= line_no <= len(lines)):
            return None
        line = lines[line_no - 1]
    else:
        path = m.group(1)
        p = path.lstrip("/")
        try:
            with open("%s/%s" % (root, p), encoding="utf-8", errors="replace") as f:
                lines = f.read().split("\n")
        except OSError:
            return None
        if not (1 <= line_no <= len(lines)):
            return None
        line = lines[line_no - 1]
    # Prefer a method call (.foo(), the usual shape of a missing DOM method),
    # then a bare call.
    for pat in (r"\.(\w+)\s*\(", r"\bnew\s+(\w+)\s*\(", r"\b(\w+)\s*\("):
        for name in re.findall(pat, line):
            if name not in HARNESS_NAMES:
                return name
    return None

def sites_for(cause, examples):
    """Which of the fifteen real-site failures this cause plausibly explains."""
    blob = cause + " " + " ".join(examples[:8])
    hits = []
    for symptom, sites, pats in SITE_FAILURES:
        if any(re.search(p, blob, re.I) for p in pats):
            hits.append(sites)
    return hits

def main():
    args = [a for a in sys.argv[1:]]
    top = 30
    only = None
    root = "third_party/wpt"
    paths = []
    i = 0
    while i < len(args):
        if args[i] == "--top":
            i += 1; top = int(args[i])
        elif args[i] == "--cause":
            i += 1; only = args[i]
        elif args[i] == "--root":
            i += 1; root = args[i]
        else:
            paths.append(args[i])
        i += 1
    if not paths:
        print("usage: wpt_rank.py <report.tsv> [--top N] [--cause SUBSTRING]")
        return 2

    rows = []
    for p in paths:
        try:
            f = open(p, encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                parts += [""] * (5 - len(parts))
            rows.append(parts[:5])
        f.close()

    if not rows:
        print("wpt-rank: no rows -- run `make test-wpt` with --report first.")
        return 1

    total = len(rows)
    bystatus = collections.Counter(r[0] for r in rows)
    ran = [r for r in rows if r[0] in ("PASS", "FAIL", "TIMEOUT", "NOTRUN",
                                       "PRECONDITION_FAILED", "HARNESS")]
    failed = [r for r in ran if r[0] != "PASS"]

    print("=" * 100)
    print("WPT ranked causes -- the work order")
    print("=" * 100)
    print("results: %d total rows" % total)
    for k in ("PASS", "FAIL", "HARNESS", "TIMEOUT", "NOTRUN", "PRECONDITION_FAILED",
              "REFTEST", "NOHARNESS"):
        if bystatus.get(k):
            note = ""
            if k == "REFTEST":
                note = "   NOT RUN: needs pixel comparison against a reference render"
            if k == "NOHARNESS":
                note = "   NOT RUN: reference / manual / support page"
            print("  %-20s %6d%s" % (k, bystatus[k], note))

    npass = bystatus.get("PASS", 0)
    njudged = npass + len([r for r in failed if r[0] != "HARNESS"])
    if njudged:
        print("\nsubtest pass rate: %d/%d  (%.1f%%)" % (npass, njudged, 100.0 * npass / njudged))

    # --- the ranking -------------------------------------------------------
    causes = collections.Counter()
    files_per_cause = collections.defaultdict(set)
    layers_per_cause = collections.Counter()
    layer_by_cause = collections.defaultdict(collections.Counter)
    examples = collections.defaultdict(list)
    for status, path, subtest, msg, stack in failed:
        c = cause_of(msg)
        if c.startswith("not a function (callee unnamed)"):
            who = callee_from_stack(root, path, stack)
            if who:
                c = "not a function: .%s()" % who
        if only and only.lower() not in c.lower():
            continue
        causes[c] += 1
        files_per_cause[c].add(path)
        layer_by_cause[c][layer_of(path)] += 1
        if len(examples[c]) < 12:
            examples[c].append("%s :: %s :: %s" % (path, subtest, msg[:100]))

    print("\n%-4s %-6s %-6s %-46s %-24s %s" %
          ("#", "TESTS", "FILES", "CAUSE", "TOP LAYER", "EXPLAINS (user's real sites)"))
    print("-" * 150)
    for i, (c, n) in enumerate(causes.most_common(top), 1):
        lay = layer_by_cause[c].most_common(1)[0]
        sites = sites_for(c, examples[c])
        print("%-4d %-6d %-6d %-46.46s %-24.24s %s" %
              (i, n, len(files_per_cause[c]), c, "%s (%d)" % (lay[0], lay[1]),
               "; ".join(sites)[:60] if sites else "-"))

    # --- harness deaths, on their own -------------------------------------
    # The highest-leverage rows in the corpus and the ones a subtest-weighted
    # ranking hides. A file that dies mid-load contributes ONE row; every
    # subtest inside it is never reported, so it is absent from the denominator
    # too. The real pass rate is therefore worse than the headline and the
    # yield per unit of work is better: one missing global can unblock hundreds
    # of files at once.
    #
    # The count here is FILES BLOCKED. The number of subtests each unblocks is
    # deliberately not estimated -- it is not observable until the file runs,
    # and a made-up figure would be the most quotable number in this report.
    deaths = [r for r in failed if r[0] == "HARNESS"]
    print("\nharness deaths: %d files never completed. Grouped by cause,"
          " ranked by files blocked." % len(deaths))
    print("(each of these hides every subtest in its file, so they are absent"
          " from the denominator too)")
    dcause = collections.Counter()
    dex = collections.defaultdict(list)
    for status, path, subtest, msg, stack in deaths:
        c = cause_of(msg)
        # An uncaught throw at load is more useful named by its identifier than
        # by its type, so keep the ReferenceError/property rules' output.
        dcause[c] += 1
        if len(dex[c]) < 6:
            dex[c].append(path)
    print("  %-6s %-56s %s" % ("FILES", "CAUSE", "EXAMPLE"))
    for c, n in dcause.most_common(20):
        print("  %-6d %-56.56s %s" % (n, c, dex[c][0][:60]))

    # --- missing globals, on their own ------------------------------------
    # The single most actionable slice: each row is one identifier that does
    # not exist, and the number beside it is how many subtests that one absence
    # takes out. This is the list `HTMLBodyElement is not defined` belongs to.
    print("\nmissing globals, ranked (one identifier each -- the cheapest fixes):")
    globals_ = collections.Counter()
    gfiles = collections.defaultdict(set)
    for status, path, subtest, msg, stack in failed:
        for g in re.findall(r"['\"]?([A-Za-z_$][\w$]*)['\"]? is not defined", msg):
            globals_[g] += 1
            gfiles[g].add(path)
    # A name used by ONE file is almost always that file's own local variable
    # tripping over an earlier failure, not a platform gap. Two is the floor.
    shown_g = [(g, n) for g, n in globals_.most_common() if len(gfiles[g]) > 1]
    for g, n in shown_g[:25]:
        print("  %-28s %6d subtests in %4d files" % (g, n, len(gfiles[g])))
    if not shown_g:
        print("  (none appearing in more than one file)")

    # --- by layer ----------------------------------------------------------
    print("\nfailures by layer (four different owners -- a ranking that mixes them is not a work order):")
    bylayer = collections.Counter(layer_of(r[1]) for r in failed)
    passlayer = collections.Counter(layer_of(r[1]) for r in ran if r[0] == "PASS")
    for lay, n in bylayer.most_common():
        p = passlayer.get(lay, 0)
        print("  %-26s %6d failing, %6d passing   (%.0f%%)" %
              (lay, n, p, 100.0 * p / (p + n) if p + n else 0))

    # --- coverage of the validation set ------------------------------------
    print("\ncoverage of the user's fifteen real-site failures:")
    for symptom, sites, pats in SITE_FAILURES:
        hit = [(c, n) for c, n in causes.most_common()
               if any(re.search(p, c + " " + " ".join(examples[c][:6]), re.I) for p in pats)]
        tot = sum(n for _, n in hit)
        if hit:
            print("  %-46.46s %-28.28s %5d subtests, top cause: %s"
                  % (symptom, sites, tot, hit[0][0]))
        else:
            print("  %-46.46s %-28.28s     -  NOT COVERED by this corpus"
                  % (symptom, sites))

    if only:
        print("\nexamples for --cause %r:" % only)
        for c, n in causes.most_common(5):
            print("\n  %s  (%d)" % (c, n))
            for e in examples[c][:6]:
                print("    %s" % e[:160])
    return 0

if __name__ == "__main__":
    sys.exit(main())
