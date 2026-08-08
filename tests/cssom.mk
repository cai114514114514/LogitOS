# tests/cssom.mk -- the CSSOM line's measurement.
#
# Own fragment for the reason every other tests/*.mk gives: several agents
# share this tree and a stale whole-file Makefile snapshot has silently deleted
# other people's targets more than once. tests/wpt.mk belongs to the WPT line
# and is not edited from here; this file builds its own runner variant instead.
#
#   make test-cssom            host unit tests for the CSSOM  (the gate)
#   make test-cssom-negctl     the negative control -- MUST fail without the fix
#   make wpt-cssom             the WPT run WITH js_cssom.c linked
#   make wpt-cssom-base        the same run WITHOUT it (the before column)
#   make wpt-cssom-rank        the ranked table of why css/ files die
#
# THE HEADLINE NUMBER IS NOT THE PASS RATE. 1,926 files under css/ never
# complete the harness, so every subtest inside them is outside the
# denominator entirely. A file that starts completing ADDS failing subtests to
# the count, so the raw pass rate can fall while the result is strictly
# better. `make wpt-cssom` prints both columns and the harness-death delta,
# and the delta is the number this line is judged on.

.PHONY: test-cssom test-cssom-negctl wpt-cssom wpt-cssom-base wpt-cssom-rank
.PHONY: cssom-compare

CSSOM_DIR  := $(BUILD)/cssom
CSSOM_SRC  := c/apps/browser/js_cssom.c

# --- the host unit tests ----------------------------------------------------
# Links layout.c so the GEOMETRY is real: a CSSOM-View test against a build
# with no layout would assert that zero equals zero, and would pass over an
# implementation that returns a constant. That is the whole reason this target
# exists separately from the WPT runner, which links no layout (see the note on
# wpt-cssom below). browser_paint.c is deliberately NOT linked: the display
# list is what the geometry reads, and PAINTING it would additionally need the
# recorder in tests/unit/painthost and a canvas this test has no use for.
CSSOM_TEST_SRC := tests/unit/cssom_test.c \
                  c/apps/browser/js_page.c c/apps/browser/js_dom.c \
                  c/apps/browser/js_cssom.c \
                  c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                  c/apps/browser/css_extra.c \
                  c/apps/browser/layout.c \
                  c/apps/browser/js_select.c \
                  c/apps/browser/js_tokenlist.c

$(CSSOM_DIR)/cssom_test: $(CSSOM_TEST_SRC) $(HTML_PARSER_SRC) \
                         $(BUILD)/libcss_host.a
	@mkdir -p $(CSSOM_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' -o $@ $(CSSOM_TEST_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm

test-cssom: $(CSSOM_DIR)/cssom_test
	@$(CSSOM_DIR)/cssom_test

# --- the negative control ---------------------------------------------------
# An assertion nobody has watched fail is not an assertion. Two independent
# sabotages, because the two halves of this line fail in different ways and one
# control would only cover one of them:
#
#   -DCSSOM_NEGCTL_SERIALIZE   the colour serialiser emits `#0000ff` instead
#                              of `rgb(0, 0, 255)` and lengths lose their
#                              `px`. This is the plausible-looking wrong
#                              CSSOM: every property is present, every call
#                              returns a string, nothing throws, and only the
#                              BYTES are wrong -- which is all WPT compares.
#   -DCSSOM_NEGCTL_NOGEOM      the geometry accessors are installed but never
#                              flush layout, so they answer 0 -- which is
#                              exactly what a build with layout unlinked
#                              answers, i.e. the state the WPT runner is in.
#
# The target succeeds when the tests FAIL against both.
CSSOM_NEGS := SERIALIZE NOGEOM

test-cssom-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(CSSOM_DIR)
	@bad=0; \
	 for n in $(CSSOM_NEGS); do \
	   $(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	       -DCONFIG_VERSION='"host"' -DCSSOM_NEGCTL_$$n \
	       -o $(CSSOM_DIR)/negctl_$$n $(CSSOM_TEST_SRC) \
	       $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm || exit 1; \
	   if $(CSSOM_DIR)/negctl_$$n > $(CSSOM_DIR)/negctl_$$n.log 2>&1; then \
	     echo "test-cssom-negctl: FAILED -- the suite PASSED with $$n sabotaged,"; \
	     echo "  so its assertions are not measuring that half."; bad=1; \
	   else \
	     echo "test-cssom-negctl: ok -- $$n sabotaged, suite fails:"; \
	     grep -m3 '^  FAIL' $(CSSOM_DIR)/negctl_$$n.log || true; \
	   fi; \
	 done; \
	 exit $$bad

# --- the WPT runner, with js_cssom.c ----------------------------------------
# The same source list tests/wpt.mk builds, plus this line's file. Built here
# rather than by editing wpt.mk because that file belongs to the WPT line.
#
# IT DOES NOT LINK layout.c, deliberately, and this is the limit on every
# number below. layout.c needs the font backend and a canvas, and the WPT
# runner has neither; with it unlinked every box is 0x0, so a revived
# check-layout file COMPLETES and then FAILS. That is strictly better than
# dying -- a failing subtest is in the denominator and a dead file is not --
# and it is not the same as passing. `make test-cssom` is where the geometry
# is actually measured.
# Deferred (`=`, not `:=`): WPT_TEST_SRC and WPT_CF come from tests/wpt.mk,
# and make reads the fragments in whatever order the Makefile lists them. An
# immediate assignment here captured them EMPTY when wpt.mk had not been read
# yet, and the failure was a compile with no include path at all -- which
# reads as a missing header rather than as a missing variable.
#
# DEDUPED, and the `filter-out` is not defensive tidying. tests/wpt.mk globs
# c/apps/browser/js_*.c into WPT_TEST_SRC, so js_cssom.c is in it now and
# listing it again put the same TU on the link line twice: five "multiple
# definition of js_cssom_install" errors and no runner. The `after` column and
# `make test-wpt` are therefore now the SAME binary's numbers -- which is the
# right outcome and worth saying, because this fragment was written when they
# were not.
CSSOM_WPT_SRC = $(WPT_TEST_SRC) $(filter-out $(WPT_TEST_SRC),$(CSSOM_SRC))
CSSOM_WPT_CF  = $(WPT_CF)

$(CSSOM_DIR)/wpt_cssom: $(CSSOM_WPT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(CSSOM_DIR)
	@$(CC) -O2 -w $(CSSOM_WPT_CF) -o $@ $(CSSOM_WPT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

CSSOM_ONLY ?=
CSSOM_ARGS  = --root $(WPT_ROOT) -b /dev/null --subset css \
              $(if $(CSSOM_ONLY),--only $(CSSOM_ONLY),)

# The BEFORE column: tests/wpt.mk's own runner, unchanged. Its baseline is
# written to a path of ours -- tests/unit/wpt_expected_fail.txt is the WPT
# line's file and has a known defect they are fixing.
wpt-cssom-base: $(BUILD)/wpt_test
	@mkdir -p $(CSSOM_DIR)
	@$(BUILD)/wpt_test $(CSSOM_ARGS) --report $(CSSOM_DIR)/before.tsv \
	    > $(CSSOM_DIR)/before.log 2>&1 || true
	@tail -3 $(CSSOM_DIR)/before.log | head -1

wpt-cssom: $(CSSOM_DIR)/wpt_cssom
	@mkdir -p $(CSSOM_DIR)
	@$(CSSOM_DIR)/wpt_cssom $(CSSOM_ARGS) --report $(CSSOM_DIR)/after.tsv \
	    > $(CSSOM_DIR)/after.log 2>&1 || true
	@tail -3 $(CSSOM_DIR)/after.log | head -1
	@if [ -f $(CSSOM_DIR)/before.tsv ]; then \
	    python3 tools/cssom_compare.py $(CSSOM_DIR)/before.tsv $(CSSOM_DIR)/after.tsv; \
	 else echo "(run 'make wpt-cssom-base' first for the before column)"; fi

# --- the ranked cause table -------------------------------------------------
# The thing this line was asked for first and the thing that stays useful
# whether or not the code lands: WHY the 1,926 files die, with counts, grouped
# by cause rather than by file.
wpt-cssom-rank: $(CSSOM_DIR)/wpt_cssom
	@mkdir -p $(CSSOM_DIR)
	@test -f $(CSSOM_DIR)/before.tsv || $(MAKE) --no-print-directory wpt-cssom-base
	@python3 tools/cssom_rank.py $(CSSOM_DIR)/before.tsv
