#!/usr/bin/env python3
"""A negative control is a COPY. Find the ones that have drifted.

WHY. A control is built from the same sources as the thing it controls,
differing deliberately in one place -- usually a single -D. Nothing in make
expresses "these two recipes must stay identical except for that", so when the
real target gains an object, a source list or a flag, the control does not, and
NOTHING NOTICES: a control is reached by no aggregate suite, so it is not in
`make ci` and not in `make test`.

Six drifted controls were found by hand in one sweep of this tree:

  browser-noplat.elf   missing $(GFX_OBJ)          -- 20 undefined symbols
  browser-noplat.aex   missing --stack-pages 2048  -- SILENT: a control that
                       died on a deep render stack satisfies "comes up with
                       none of the platform APIs" perfectly
  test-wpt-negctl      missing $(GFX_SRC)
  test-wpt-fire-negctl missing $(GFX_SRC)
  test-encoding-negctl missing $(GFX_SRC)
  nofocus/browser.o    used $(JS_CF) where the real objects use
                       $(BROWSER_JS_CF), reviving a bug fixed a week earlier

There are 100 `*-negctl` targets. Six were found by tripping over them.

WHAT IT COMPARES, and why at this level. Recipes are compared UNEXPANDED: the
tokens `$(GFX_OBJ)`, `$(WPT_TEST_SRC)`, `-lm`, `--stack-pages` and so on. That
is deliberately the level at which the six real cases differed, and it does not
need a working build, a configured toolchain, or make to agree to expand
anything. Expanding would find more and would also drown the result in paths.

WHAT IT CANNOT DO. It pairs `X-negctl` with `X` by name. A control whose
positive counterpart is named differently is invisible to it, and so is one
built by a $(call) template. It reports; it does not judge -- a difference is
often the POINT of the control (`-DWPT_NEGCTL` had better be there), so the
output is a diff to read, not a verdict. Deliberate differences belong in
EXPECTED below, each with the reason it is deliberate.

Usage:  python3 tools/negctl_drift.py [--all]
        (--all also prints pairs whose only differences are expected)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# WHAT COUNTS AS DRIFT, and this is the whole quality of the tool.
#
# The first version compared every token and flagged 48 of 71 pairs. A detector
# that fires on two thirds of its input is not a detector -- it is the same
# failure test-audit had when 24 of its 28 MUTE entries were false, and the
# remedy is the same: fix the detector, not the tree.
#
# Look at what the six REAL cases had in common: a missing OBJECT or SOURCE LIST
# ($(GFX_OBJ), $(GFX_SRC)), and one missing packaging flag (--stack-pages 2048).
# Nothing was ever about -Wall. So:
#
#   * `$(...)` references that name a list of things to compile or link --
#     _OBJ, _OBJS, _SRC, _SRCS, _LIB, _AEX -- plus explicit $(BUILD)/*.{o,a}.
#     Missing one of these is a control built from different code.
#   * long flags (`--stack-pages`), which is how packaging differs.
#
# Explicitly NOT: -W (warnings change no behaviour), -I/-D/-U/-O/-g (the -D is
# usually the control itself), and the control's own output binary.
LIST_VAR = re.compile(r"^\$\([A-Z0-9_]*(OBJ|OBJS|SRC|SRCS|LIB|AEX)[A-Z0-9_]*\)$")
BUILD_ART = re.compile(r"^\$\(BUILD\)/.*\.(o|a)$")


# `--` separates arguments, `--no-print-directory` is a make flag, and a bare
# `---"` is the tail of an echoed banner. None of the three is a difference in
# what got built, and each was in the first tightened run's output.
FLAG_NOISE = {"--", "---\"", "--no-print-directory"}


def interesting(w):
    if LIST_VAR.match(w) or BUILD_ART.match(w):
        return True
    return w.startswith("--") and w not in FLAG_NOISE

# Differences that are deliberate or structural, keyed by "target: token", with
# the reason each is here. Same discipline as tools/audit_tests.py's ALLOW_MUTE:
# the value of this list is that what remains is short enough to read, so every
# entry has to be argued rather than added to make the output quiet.
EXPECTED = {
    # THE TOOL'S REAL BLIND SPOT, and both of these are it rather than a fact
    # about the tree: when a control's COMPILE happens in a separate rule, this
    # compares two recipes that were never meant to look alike. test-forms-negctl
    # builds a disk and runs a QMP harness; its drift risk lives in
    # $(BUILD)/browser-nofocus.elf, which is a rule this pairing never sees.
    "test-forms-negctl: $(FORMS_SRC)":
        "the control's compile is in $(BUILD)/browser-nofocus.elf, a separate "
        "rule -- check THAT against $(BUILD)/browser.elf, not this recipe",
    "test-forms-negctl: $(BUILD)/libcss_host.a": "same: the link is elsewhere",

    # Deliberate variant lists -- the control is SUPPOSED to compile something
    # else. A control that used the same list would not be a control.
    "test-html5lib-negctl: $(HTML_PARSER_SRC)":
        "uses $(H5NEG_SRC), which swaps in build/negctl/html_tree.c -- the "
        "naive adoption-agency implementation this control exists to measure",

    # Deliberate argument differences: the control runs the same binary another
    # way, which is the whole method.
    "test-reftest-negctl: --ahem":     "the control renders without the Ahem font",
    "test-reftest-negctl: --baseline": "no baseline: every failure is new, by design",
    "test-reftest-negctl: --limit":    "unbounded, so the control cannot pass by truncation",
    "test-motion-negctl: --iso":       "the control drives a differently-built ISO",

    # Deliberate, and the one difference this control is FOR.
    "test-webp-vp8-negctl: $(RUST_LIB_HOST)":
        "links $(BUILD)/rustctl instead -- the feature-flagged Rust build "
        "(vp8-tr-from-subblock, vp8-dc-always-avail) that IS the control",
}


def makefiles():
    out = [os.path.join(ROOT, "Makefile")]
    td = os.path.join(ROOT, "tests")
    for f in sorted(os.listdir(td)):
        if f.endswith(".mk"):
            out.append(os.path.join(td, f))
    return out


def rules():
    """-> {target: [recipe lines]}"""
    out, cur = {}, None
    for mf in makefiles():
        for ln in open(mf, encoding="utf-8", errors="replace").read().split("\n"):
            if ln.startswith("\t"):
                if cur:
                    out.setdefault(cur, []).append(ln)
                continue
            m = re.match(r"^([^\s:=#]+)\s*:{1,2}(?!=)", ln)
            cur = m.group(1) if m else None
    return out


def tokens(lines):
    t = set()
    for ln in lines:
        for w in ln.replace("\\", " ").split():
            if interesting(w):
                t.add(w)
    return t


def main(argv):
    show_all = "--all" in argv
    r = rules()
    pairs = [(t, t[:-len("-negctl")]) for t in sorted(r) if t.endswith("-negctl")]
    n_drift = n_pair = 0
    for neg, pos in pairs:
        if pos not in r:
            continue
        n_pair += 1
        tn, tp = tokens(r[neg]), tokens(r[pos])
        only_pos = sorted(x for x in tp - tn if EXPECTED.get(neg + ": " + x) is None)
        if not only_pos and not show_all:
            continue
        if only_pos:
            n_drift += 1
        print("%s" % neg)
        for x in only_pos:
            print("      in %s, NOT in the control:  %s" % (pos, x))
        if show_all:
            for x in sorted(tn - tp):
                print("      control-only (probably the point):  %s" % x)
    # THE OTHER DIRECTION, and its value is being zero.
    #
    # Everything above asks whether the control is missing something the real
    # target has. This asks whether the control has ANYTHING OF ITS OWN -- no
    # -D, no swapped source list, no changed argument. A control that is a
    # literal copy of its target cannot fail differently from it, so it can
    # never be watched failing, and it is counted as evidence anyway. That is
    # the worst shape a control can take, and it costs one set difference to
    # rule out.
    #
    # Currently 0 of 71, which is why it prints even when it finds nothing: a
    # check whose healthy state is silence is a check nobody knows is running.
    identical = []
    for neg, pos in pairs:
        if pos not in r:
            continue
        alln = set(w for ln in r[neg] for w in ln.replace("\\", " ").split())
        allp = set(w for ln in r[pos] for w in ln.replace("\\", " ").split())
        if not (alln - allp):
            identical.append(neg)
    if identical:
        print("\nCONTROLS THAT ARE LITERAL COPIES -- they cannot fail differently:")
        for t in identical:
            print("      %s" % t)
    else:
        print("\nevery paired control has at least one token of its own (0 literal copies)")

    print("\n%d of %d name-paired controls have a token their positive target has"
          % (n_drift, n_pair))
    print("(100 *-negctl targets exist; this pairs only those named X-negctl "
          "against an existing X)")
    # A HARD GATE FROM DAY ONE, unlike tools/audit_tests.py, and the difference
    # is not principle but arithmetic: the audit still has 14 findings, so ci
    # keeps it advisory until they are worked off. This check is at 0 in both
    # directions today, so there is nothing to grant a grace period to -- and a
    # check that starts clean and is allowed to go dirty was never a gate.
    return 1 if (n_drift or identical) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
