# tests/reftest.mk -- WPT REFTESTS: the first measurement of layout correctness
# this project has ever had.
#
# third_party/wpt holds tens of thousands of reftests and until this fragment
# existed not one of them ran. Everything else in the corpus tests API surface;
# the reftests are the ones that judge by PIXELS -- whether a box is in the right
# place at the right size -- so layout correctness here was, precisely, unmeasured.
#
# A reftest is a page carrying <link rel="match" href="...">. Both pages render
# and the test passes when the two renderings are IDENTICAL, pixel for pixel. The
# elegance is that both sides go through the SAME engine, so antialiasing, glyph
# rasterization and rounding cancel out: a correct engine matches exactly even if
# its text looks nothing like Chrome's. Exact comparison is the DEFAULT here, not
# a tolerance. `<meta name=fuzzy>` is honoured where the corpus asks for it and
# the report separates the tests that pass only because of it.
#
# The model is test-h264, and the Makefile comment that goes with it: bit-exactness
# is the bar, and test-h264-diff prints per-case wrong-byte totals because "the
# first mismatch moved" says nothing. Every failing test here carries a wrong-pixel
# count, and `make reftest-diff` writes the three images for one of them.
#
#   make test-reftest         THE GATE. The rate, the ranked cause table, the
#                             ratchet against the expected-failure list, and the
#                             always-equal control run INLINE (see below).
#   make reftest              the same measurement with no gate, for a tree that
#                             is already broken
#   make reftest-one TEST=... judge exactly one test, verbosely -- the bisect form
#   make reftest-diff TEST=.. ... and write its test/ref/difference PNGs
#   make reftest-rank         the ranked failure table on its own: the work order
#   make reftest-baseline     rewrite the expected-failure list from this run
#   make reftest-manifest     rebuild the cached test list
#   make test-reftest-ahem    CAN WE RENDER AHEM AT ALL -- seconds, run it first
#   make test-reftest-negctl       the always-equal comparator, full corpus
#   make test-reftest-css-negctl   CSS withheld, full corpus, both readings
#
# Both control targets end in -negctl, which is how tools/audit_tests.py's CI
# discovery knows to skip them -- they are slow and they are expected to report
# strange numbers. The guarantees they provide are NOT left outside CI for that
# reason: the always-equal control also runs inline at the top of test-reftest,
# and the discrimination check (control 2, folded per-test) is asserted there
# too, so the gate cannot print a rate without both having fired.
#
# THE NUMBER TO KNOW, and it is not the one a normal reftest harness prints.
# On the first 400 tests this reports 142 exact matches -- 35.77% -- and then
# reports that 140 of those 142 STILL PASS with the test's own stylesheet
# deleted. The discriminating rate is 2/397 = 0.50%. A pass that survives having
# the CSS removed is not evidence about CSS or layout; it is a test whose two
# sides we render identically for reasons unrelated to what it is testing. The
# runner measures this on every passing test and prints both numbers, and the
# lower one is the one to quote. See tests/unit/reftest.c's opt_nocss comment for
# how the negative control turned into a per-test measurement.
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment here gives: a whole-file Makefile write from a concurrent line deletes
# targets written straight into it.

.PHONY: test-reftest reftest reftest-one reftest-diff reftest-rank \
        reftest-baseline reftest-manifest test-reftest-ahem \
        test-reftest-negctl test-reftest-css-negctl reftest-ahem-fetch

WPT_ROOT      ?= third_party/wpt
REFT_BIN      := $(BUILD)/reftest/reftest
REFT_MANIFEST := $(BUILD)/reftest/manifest.txt
REFT_BASELINE := tests/unit/reftest_expected_fail.txt
REFT_FILTER   ?= css/
REFT_AHEM     ?= .cache/ahem/Ahem.ttf
# A sample size for the inline control inside the gate. Small on purpose: the
# control is about whether the comparator can fail, and that does not need the
# whole corpus to answer.
REFT_CTLN     ?= 200

# THE LINK MUST MATCH browser.aex's, or every number is of a browser that does
# not exist -- 12d33d6's lesson, and tests/wpt.mk's header says the same thing at
# more length. Two halves:
#
#   the ring-3 pipeline, exactly what browser.aex links, and
#   the KERNEL drawing code, which on the machine sits behind the five GUI
#   syscalls: fb.c, raster.c, text.c and c/lib/text. Those link on the host
#   unmodified, so this harness runs the REAL rasterizer, the REAL shaper and
#   the REAL text_measure rather than the `len * (px/2)` stub every other host
#   test in this tree substitutes. For a reftest -- a claim about where text
#   ended up -- that stub would make the whole text half of the corpus
#   meaningless.
#
# What is NOT shared is stated in full at the top of tests/unit/refhost/logit.h:
# wm.c's six-line syscall cases (transcribed, each quoted above the function
# that mirrors it), the window chrome, and which font file the loader opens.
REFT_PIPELINE := c/apps/browser/layout.c c/apps/browser/browser_paint.c \
                 c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                 c/apps/browser/css_extra.c $(HTML_PARSER_SRC)
REFT_KERNEL   := c/kernel/gui/fb.c c/kernel/gui/raster.c c/kernel/gui/text.c \
                 c/lib/text/ttf.c c/lib/text/cff.c c/lib/text/utf8.c \
                 c/lib/text/shape.c c/lib/text/script.c c/lib/text/bidi.c \
                 c/lib/text/otlayout.c
REFT_HARNESS  := tests/unit/reftest.c tests/unit/refhost/refrender.c \
                 tests/unit/refhost/refhost.c tests/unit/refhost/refmanifest.c
REFT_SRC      := $(REFT_HARNESS) $(REFT_PIPELINE) $(REFT_KERNEL) $(GFX_SRC) $(IMG_HOST_SRC)

# -Itests/unit/refhost FIRST: that is what makes `#include "logit.h"` resolve to
# the rasterizing shim instead of the app's int-0x80 wrappers. Same ordering trap
# CLAUDE.md documents for uonly/ -- put it after $(BTEST_INC) and the real header
# wins and the link fails on _sys.
REFT_INC := -Itests/unit/refhost -Iinclude/abi $(BTEST_INC) $(CSS_INC) \
            -Ic/kernel/gui -Ic/kernel/mm -Ic/kernel/core -Ic/fs \
            -Ic/drivers/virtio -Ic/kernel/cpu

$(REFT_BIN): $(REFT_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) \
             tests/unit/refhost/logit.h tests/unit/refhost/refhost.h \
             tests/unit/refhost/refmanifest.h tests/unit/refhost/refrender.h
	@mkdir -p $(BUILD)/reftest
	$(CC) -O2 -w $(REFT_INC) -o $@ $(REFT_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm -lpthread

# --- the Ahem question, answered in seconds -------------------------------
# Run this before anything else. A large class of layout reftests sets
# `font: 25px/1 Ahem` precisely so that font rendering cancels out of the
# comparison: Ahem's glyphs are exactly-specified filled boxes, one em wide,
# from 0.8em above the baseline to 0.2em below. If we cannot render Ahem, that
# whole class is unjudgeable no matter how good layout gets.
#
# THE ANSWER IS YES. Our own c/lib/text parses Ahem and rasterizes a 25px glyph
# as a PERFECT 25x25 box: 625 pixels at full coverage and ZERO antialiased edge
# pixels, so exact pixel comparison is viable rather than merely hoped for. The
# metrics agree too -- text_line_height(25) == 25, and text_measure is exact em
# multiples. This target re-asserts all of it, so a regression in ttf.c or
# raster.c that made Ahem fuzzy would be caught here rather than showing up as
# an unexplained drop in the rate.
test-reftest-ahem: $(BUILD)/reftest/ahem_test
	@if [ ! -f "$(REFT_AHEM)" ]; then \
	    echo "test-reftest-ahem: $(REFT_AHEM) not present -- run 'make reftest-ahem-fetch'"; \
	    echo "  (Ahem is NOT in third_party/wpt: our vendored subset omits fonts/.)"; \
	    exit 1; \
	 fi
	@$(BUILD)/reftest/ahem_test $(REFT_AHEM)

# Links only the text half -- no browser pipeline, no LibCSS -- so it builds and
# runs in seconds and can be the first thing anyone asks.
$(BUILD)/reftest/ahem_test: tests/unit/ahem_test.c tests/unit/refhost/refhost.c $(REFT_KERNEL)
	@mkdir -p $(BUILD)/reftest
	$(CC) -O2 -w -Itests/unit/refhost -Ic/kernel/gui -Ic/lib/text -Ic/kernel/mm \
	    -Ic/kernel/core -Ic/fs -Ic/drivers/virtio -Ic/lib/gfx -Ic/lib/image \
	    -o $@ tests/unit/ahem_test.c tests/unit/refhost/refhost.c $(REFT_KERNEL) -lm

# Ahem is not in the vendored WPT subset (third_party/wpt/fonts/ does not
# exist), so it is fetched to .cache/ and gitignored, the same shape as the
# other external corpora here.
reftest-ahem-fetch:
	@mkdir -p .cache/ahem
	@if [ -f .cache/ahem/Ahem.ttf ]; then echo "already present: .cache/ahem/Ahem.ttf"; else \
	    echo "fetching Ahem..."; \
	    curl -fsSL -o .cache/ahem/Ahem.ttf \
	      https://raw.githubusercontent.com/web-platform-tests/wpt/master/fonts/Ahem.ttf \
	    || { echo "fetch failed -- put Ahem.ttf at .cache/ahem/ by hand"; exit 1; }; \
	 fi
	@ls -l .cache/ahem/Ahem.ttf

# The manifest cache. Walking this checkout costs ~95 seconds of pure I/O
# against 0.6 seconds of CPU, so re-walking per invocation would make the
# single-test bisect form unusable.
reftest-manifest: $(REFT_BIN)
	@mkdir -p $(BUILD)/reftest
	@rm -f $(REFT_MANIFEST)
	@$(REFT_BIN) --manifest $(REFT_MANIFEST) --filter '$(REFT_FILTER)' --limit 1 >/dev/null
	@echo "manifest: $$(wc -l < $(REFT_MANIFEST)) candidate files -> $(REFT_MANIFEST)"

$(REFT_MANIFEST): $(REFT_BIN)
	@$(MAKE) --no-print-directory reftest-manifest

# --- THE GATE --------------------------------------------------------------
# Ratcheted against a committed expected-failure list, the same shape
# tests/wpt.mk uses and for the reason its header gives: a percentage is not a
# thing you can defend, but the individual tests behind it are. Green on a tree
# that changes nothing, red only when something that worked stops working.
#
# The always-equal control runs INLINE, before the measurement. tools/audit_tests.py
# reports 22 harnesses in this tree that print FAIL and exit 0, and the CI suite
# discovery deliberately excludes `test-*-negctl` targets -- so a control that
# lived only in a separate target would never run in CI at all. Putting it here
# means the gate cannot report a rate without first proving its own comparator
# is capable of saying no.
test-reftest: $(REFT_BIN) $(REFT_MANIFEST)
	@echo "--- control: a comparator that always reports equal must score 100% ---"
	@rate=$$($(REFT_BIN) --manifest $(REFT_MANIFEST) --limit $(REFT_CTLN) \
	          --always-equal 2>/dev/null | sed -n 's/^PASS RATE.*=  *\([0-9.]*\)%.*/\1/p'); \
	 if [ "$$rate" != "100.00" ]; then \
	    echo "CONTROL BROKEN: always-equal scored $$rate%, expected 100.00%"; \
	    echo "  The suite must be able to go green when the comparator cannot fail."; \
	    exit 1; \
	 fi; echo "  ok: always-equal scores $$rate% -- the comparator is load-bearing"
	@echo
	@rc=0; \
	 $(REFT_BIN) --manifest $(REFT_MANIFEST) --filter '$(REFT_FILTER)' \
	    --baseline $(REFT_BASELINE) --ahem $(REFT_AHEM) \
	    > $(BUILD)/reftest/gate.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/reftest/gate.log; \
	 if grep -q 'DISCRIMINATION CHECK: SUSPECT' $(BUILD)/reftest/gate.log; then \
	    echo; echo "CONTROL BROKEN: the discrimination check measured nothing."; \
	    echo "  It is negative control 2 folded into the ordinary run, and a"; \
	    echo "  check that never fires is a check that is not there."; \
	    exit 1; \
	 fi; exit $$rc

# The same measurement with no gate, for a tree that is already broken.
reftest: $(REFT_BIN) $(REFT_MANIFEST)
	@$(REFT_BIN) --manifest $(REFT_MANIFEST) --filter '$(REFT_FILTER)' \
	    --ahem $(REFT_AHEM) || true

# --- bisecting -------------------------------------------------------------
reftest-one: $(REFT_BIN)
	@if [ -z "$(TEST)" ]; then echo "usage: make reftest-one TEST=css/CSS2/....xht"; exit 2; fi
	@$(REFT_BIN) --one '$(TEST)' --ahem $(REFT_AHEM)

reftest-diff: $(REFT_BIN)
	@if [ -z "$(TEST)" ]; then echo "usage: make reftest-diff TEST=css/CSS2/....xht"; exit 2; fi
	@mkdir -p $(BUILD)/reftest/diff
	@$(REFT_BIN) --one '$(TEST)' --ahem $(REFT_AHEM) --diffdir $(BUILD)/reftest/diff
	@echo "  test / reference / amplified difference written to $(BUILD)/reftest/diff/"

# --- the work order --------------------------------------------------------
# Worth more than the pass rate, and said plainly in the brief this was built
# from: this table decides the order of the layout work.
reftest-rank: $(REFT_BIN) $(REFT_MANIFEST)
	@$(REFT_BIN) --manifest $(REFT_MANIFEST) --filter '$(REFT_FILTER)' \
	    --ahem $(REFT_AHEM) 2>/dev/null | sed -n '/ranked failure causes/,$$p'

reftest-baseline: $(REFT_BIN) $(REFT_MANIFEST)
	@$(REFT_BIN) --manifest $(REFT_MANIFEST) --filter '$(REFT_FILTER)' \
	    --ahem $(REFT_AHEM) --write-baseline $(REFT_BASELINE) >/dev/null || true
	@echo "baseline rewritten: $$(grep -vc '^#' $(REFT_BASELINE)) expected failures -> $(REFT_BASELINE)"

# --- the negative controls, full corpus ------------------------------------
# NEGATIVE CONTROL 1, and the one that matters. A comparator that cannot fail is
# the single most dangerous thing this line could have built: it would report a
# rising pass rate forever while layout got worse. The suite MUST go green when
# the comparator is stubbed to equality, and this target FAILS if it does not.
test-reftest-negctl: $(REFT_BIN) $(REFT_MANIFEST)
	@$(REFT_BIN) --manifest $(REFT_MANIFEST) --filter '$(REFT_FILTER)' \
	    --always-equal > $(BUILD)/reftest/negctl.log 2>&1 || true
	@rate=$$(sed -n 's/^PASS RATE.*=  *\([0-9.]*\)%.*/\1/p' $(BUILD)/reftest/negctl.log); \
	 echo "always-equal comparator: $$rate%"; \
	 if [ "$$rate" != "100.00" ]; then \
	    echo "NEGATIVE CONTROL FAILED: expected 100.00%, got $$rate%"; exit 1; \
	 else echo "negative control ok: the suite goes green when the comparator cannot fail"; fi

# NEGATIVE CONTROL 2, and read tests/unit/reftest.c's opt_nocss comment before
# changing it. Withholding CSS from BOTH sides does NOT collapse the rate -- it
# RAISES it, because two pages stripped of styling degrade to the same unstyled
# HTML. So the control withholds it from the TEST only, and even that scored
# 77% on the first 300, which is what turned the control into the per-test
# discrimination measurement the gate now prints. This target records the
# corpus-wide figure for both.
test-reftest-css-negctl: $(REFT_BIN) $(REFT_MANIFEST)
	@echo "--- CSS withheld from the TEST only ---"
	@$(REFT_BIN) --manifest $(REFT_MANIFEST) --filter '$(REFT_FILTER)' \
	    --no-css-test 2>/dev/null | grep -E 'PASS RATE|DISCRIMINATING' || true
	@echo "--- CSS withheld from BOTH sides (diagnostic, expected to be HIGH) ---"
	@$(REFT_BIN) --manifest $(REFT_MANIFEST) --filter '$(REFT_FILTER)' \
	    --no-css 2>/dev/null | grep -E 'PASS RATE|DISCRIMINATING' || true
