# tests/jsfb.mk -- the js-framework-benchmark conformance matrix: can this
# browser DRIVE an application, not merely load one.
#
# In its own fragment, like tests/frameworks.mk and tests/webapi_platform.mk,
# for the reason those give: several lines edit the top-level Makefile at once
# and a whole-file overwrite deletes targets written straight into it. Only the
# single `-include tests/jsfb.mk` line lives in the Makefile.
#
# WHAT THIS MEASURES THAT tests/frameworks.mk DOES NOT.
# That fragment measures seven framework builds up to the settle point and asks
# whether their scripts ran and whether anything appeared in the DOM. It cannot
# ask what happens when somebody presses a button, and an application does all
# of its real work after that. This one presses the buttons: create 1,000 rows,
# create 10,000, append, update every tenth, select, swap, remove, clear -- the
# eight operations of one specified application, implemented independently by
# 253 authors.
#
# AND IT CANNOT BE FITTED TO. That is the whole reason this corpus was chosen
# over any number of captured sites. You cannot special-case thirty independent
# codebases into working; you can only implement the platform they all stand on.
# Six of the implementations are JavaScript EMITTED BY A COMPILER from Scala,
# Haskell, PureScript, OCaml and Reason, which exercise the DOM in shapes no
# hand-written bundle produces -- so if the hand-written and the generated ones
# score alike, the matrix is measuring the platform and not a coding style.
#
# NOT A BENCHMARK. Upstream compares frameworks by milliseconds. Nothing here
# reports a duration, and tools/perf/'s rule is why: "the host is contended --
# other agents run QEMU concurrently -- so host wall clock is worthless here."
# The one time-shaped number below is a TIMEOUT, which is a verdict, not a
# measurement.
#
#   make jsfb-fetch          fetch the corpus at the pin (network; not a test)
#   make probe-jsfb          the matrix + the ranked causes (report; never fails)
#   make test-jsfb           the same, asserted against BASELINE
#   make test-jsfb-control   the control alone, watched failing
#
.PHONY: jsfb-fetch probe-jsfb test-jsfb test-jsfb-control

JSFB_ROOT   ?= build/jsfb
JSFB_MATRIX := tests/unit/jsfb_matrix.py
JSFB_BASE   := tests/jsfb/BASELINE

# --- jsfb-fetch: the corpus, at the revision in tools/jsfb_revision.txt ------
# NOT a prerequisite of anything below. It needs the network, and a gate whose
# first act is a clone fails on a missing network rather than on a broken
# browser -- which is the shape CLAUDE.md's host-reality table is entirely made
# of. The targets below SKIP LOUDLY when the corpus is absent and name this
# command as the thing that would settle it.
jsfb-fetch:
	@bash tools/jsfb_fetch.sh $(JSFB_ROOT)

# --- probe-jsfb: the matrix -------------------------------------------------
# Depends on $(BUILD)/webapi_probe from tests/webapi_platform.mk. Deliberately
# the SAME probe binary the webapi corpus and the framework corpus are measured
# with, not a second one: tests/frameworks.mk states the reason and it is
# unchanged here -- "two instruments measuring the same thing is how you get two
# answers." What this line adds to that binary is `--drive`, which evaluates a
# driver script in the page's own context at the settle point and dispatches
# clicks through js_dom_dispatch(), the same C entry point browser.c calls from
# the mouse path.
# --- is the corpus still upstream's? ----------------------------------------
# THE FAILURE THIS ANSWERS HAPPENED, on 2026-08-29, and nothing in the tree
# said a word. A workflow ran a causal experiment by injecting a fourteen-line
# `__retain` prelude into four documents inside build/jsfb -- keyed/svelte,
# keyed/solid-store, keyed/lit, keyed/react-hooks -- and left them there. A
# later run of the SAME probe binary inherited the intervention and disagreed
# with the published matrix: svelte 7/8 instead of 0/8, solid-store 8/8
# instead of 0/8. It was caught only because two runs of one binary disagreed
# and somebody chased it rather than publishing it.
#
# An injected experiment reads as a browser improvement, which is the worst
# direction for a corpus to drift in. build/jsfb was the one input to this gate
# that nothing checked and that workflows are expected to poke at.
#
# It hashes the DOCUMENTS (asked of jsfb_corpus, never re-derived here) plus the
# shared css/, and deliberately not node_modules or dist churn -- a gate that
# reddens on an ordinary npm build is a gate people learn to ignore.
#
# NOT WIRED AS A HARD PREREQUISITE OF probe-jsfb, and that is deliberate: this
# corpus is OPTIONAL (WPT's rule, and jsfb_matrix already SKIPs loudly without
# it), so a missing manifest must not turn a report into a failure. It runs and
# says what it found. test-jsfb, which asserts against a baseline, DOES take it
# as a prerequisite -- a ratchet measured against a corpus nobody published is
# worse than no ratchet.
.PHONY: jsfb-manifest jsfb-verify
jsfb-manifest:
	@python3 tools/jsfb_manifest.py --root $(JSFB_ROOT) --write

jsfb-verify:
	@python3 tools/jsfb_manifest.py --root $(JSFB_ROOT) --check

probe-jsfb: $(BUILD)/webapi_probe
	@-python3 tools/jsfb_manifest.py --root $(JSFB_ROOT) --check
	@python3 $(JSFB_MATRIX) $(BUILD)/webapi_probe --root $(JSFB_ROOT)

# --- test-jsfb: the assertion -----------------------------------------------
# Host, no QEMU, no network once the corpus is on disk -- so tools/audit_tests.py
# classifies it `host` and tools/ci.sh picks it up automatically.
#
# It is a CHANGE DETECTOR, not a wish list. Everything in $(JSFB_BASE) is the
# measured state on the day it was written, so this is green today and goes red
# the moment a DOM or event gap is closed -- which is exactly the acceptance
# check webapi_probe.c's header asks for, one layer up: when a platform feature
# lands, a row in this matrix must move, and if none does, the implementation
# did not matter to any of thirty independent applications.
#
# When it fires, update $(JSFB_BASE) in the same commit AND SAY WHICH CAUSE
# MOVED. A baseline updated without that sentence is a baseline that records
# nothing.
#
# THE CORPUS IS PINNED; THE BROWSER IS NOT, AND THAT ASYMMETRY IS WHY THE
# BASELINE HAS TO BE RE-TAKEN ON A SETTLED TREE.
# tools/jsfb_revision.txt pins the corpus, so the left column of this matrix
# cannot move by itself. Nothing pins the engine, and the first baseline was
# taken while three lines were landing DOM and Web API changes: two consecutive
# full runs scored non-keyed/incr_dom at 1/8 and the next at 8/8, and the
# apparatus was innocent -- eleven runs in isolation were 8/8 and the difference
# was that `make` had relinked the probe against a js_dom.c edited four minutes
# earlier. If a row moves and you cannot account for it, check that first:
#     stat -f '%Sm %N' build/webapi_probe; git status --short c/apps/browser
# A baseline taken during an edit storm records a browser that existed for four
# minutes, which is worse than no baseline because it reads like one.
test-jsfb: $(BUILD)/webapi_probe jsfb-verify
	@python3 $(JSFB_MATRIX) $(BUILD)/webapi_probe --root $(JSFB_ROOT) \
	    --baseline $(JSFB_BASE)

# --- test-jsfb-control: the control, on its own line ------------------------
# WIRED AS A PREREQUISITE OF test-jsfb, not merely declared. CLAUDE.md counts 61
# "stranded" controls in this tree, dropped from every suite on the untested
# ground that a control is "run by its positive counterpart", and names the fix:
# "The fix for one is a single line -- test-X: test-X-negctl -- and naming it on
# a ci-host: line instead satisfies the audit and still runs it never, which is
# worse because it looks fixed."
#
# THERE ARE TWO CONTROLS, and the second exists because the first cannot see
# the hole it covers -- which is the same lesson one layer up from the one that
# put NOPRE in the driver.
#
#   1. THE NULL CONTROL. The reference implementation's own index.html with its
#      <script> elements deleted: the corpus's furniture, provably no
#      behaviour. All three precondition-free operations must be watched
#      FAILING on it.
#   2. THE STATIC CONTROL. The same skeleton PLUS a script that renders 1,000
#      conforming rows at load and binds no handler. It is here because `run`
#      asserted "1,000 rows afterwards", which is equally true of a page that
#      was BORN with them and ignored the click -- and the null control cannot
#      catch that, because it starts at zero and so fails for the count alone.
#      Every `run ok` in the matrix was therefore unattributable: it could mean
#      the button worked or that it was never needed. `run` is now asserted as
#      a TRANSITION and must be watched FAILING here.
#
#      Watched in BOTH directions, which is the only reason to believe it: with
#      the pre-fix count-only assertion restored, this control reports `run
#      PASS` and jsfb_matrix.py refuses to print a matrix. And the fix is
#      additive -- no row of the matrix moved, because every implementation in
#      this corpus loads with 0 or 1 rows, so the count always had to move.
#
# jsfb_matrix.py builds both, runs them FIRST, and REFUSES to print a matrix if
# either misbehaves -- so this target is that refusal, exercised alone so it can
# be watched.
#
# `--only` reduces the run to nothing, which is the point: the control is not a
# row in the matrix, it is the gate on the matrix, and this proves the gate
# without paying for thirty page loads.
test-jsfb: test-jsfb-control
test-jsfb-control: $(BUILD)/webapi_probe
	@python3 $(JSFB_MATRIX) $(BUILD)/webapi_probe --root $(JSFB_ROOT) \
	    --only __none__

ci-host: test-jsfb
