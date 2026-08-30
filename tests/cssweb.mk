# --- the CSS engine's measurement + fidelity targets -----------------------
#
# In its own .mk rather than in the Makefile because several lines share this
# tree and a stale Makefile snapshot has silently deleted other people's targets
# more than once today (see the "re-add ... lost to a stale Makefile snapshot"
# commits). A separate file cannot be clobbered by a whole-file overwrite.

# --- audit-css: what REAL pages need from the engine, measured -------------
# The work order for the CSS engine, produced rather than guessed. Runs the
# real pipeline over the committed corpus in tests/fixtures/cssweb (15 pages:
# news, docs, search results, product, Chinese portals, a GitHub-style app UI)
# with a reporter installed inside LibCSS's parseProperty(), and ranks every
# declaration the cascade throws away plus every layout mode the pages need,
# weighted by how much of the box tree each mode GOVERNS.
# See the header of tests/unit/css_audit.c for why it is weighted that way.
#   make audit-css                 # the whole corpus
#   make audit-css AUDIT_TOP=60
#   make audit-css AUDIT_DIRS=tests/fixtures/cssweb/github
AUDIT_DIRS ?= $(sort $(dir $(wildcard tests/fixtures/cssweb/*/index.html)))
AUDIT_TOP  ?= 30
NEGDIR := $(BUILD)/cssnegctl
$(BUILD)/css_audit: tests/unit/css_audit.c $(BUILD)/libcss_host.a \
                    c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                    c/apps/browser/css_extra.c c/apps/browser/layout.c c/apps/browser/layout_text.c \
                    c/apps/browser/browser_paint.c $(HTML_PARSER_SRC)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ tests/unit/css_audit.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c \
	    c/apps/browser/layout.c c/apps/browser/layout_text.c c/apps/browser/browser_paint.c $(GFX_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a

audit-css: $(BUILD)/css_audit
	@$(BUILD)/css_audit --top=$(AUDIT_TOP) $(AUDIT_DIRS)

# --- audit-css-before: the same corpus with this work DISABLED -------------
# The before half of the before/after, built from the SAME sources with only
# the three changes turned off -- the css_vars whole-value invariant, the
# conditional group rules, and the nested-at-rule dispatch. Building it this
# way rather than from an old checkout is what makes the two columns
# comparable: same corpus, same viewport, same binary shape, one variable.
#   make audit-css-before | head -22   # then diff against `make audit-css`
audit-css-before: $(BUILD)/libcss_host.a $(NEGDIR)/css_audit_before
	@$(NEGDIR)/css_audit_before --top=$(AUDIT_TOP) $(AUDIT_DIRS)

$(NEGDIR)/css_audit_before: tests/unit/css_audit.c c/apps/browser/css_vars.c \
                            third_party/css/libcss/src/parse/language.c \
                            third_party/css/libcss/src/parse/parse.c \
                            $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)/libcss/parse
	@sed -e 's/#define VVAL_MAX 8192/#define VVAL_MAX 192/' \
	     -e 's|char \*dst = (vlen < VVAL_MAX \&\& text_balanced(val, vlen))|if (vlen >= VVAL_MAX) vlen = VVAL_MAX - 1;\n    char *dst = (1)|' \
	     -e 's/^static int text_balanced(const char \*s, int n)$$/static int text_balanced(const char *s, int n) { (void) s; (void) n; return 1; }\nstatic int text_balanced_disabled(const char *s, int n)/' \
	     c/apps/browser/css_vars.c > $(NEGDIR)/css_vars.c
	@sed -e 's/^static bool at_rule_is_group(const css_token \*kw)$$/static bool at_rule_is_group(const css_token *kw) { (void) kw; return false; }\nstatic bool at_rule_is_group_disabled(const css_token *kw)/' \
	     third_party/css/libcss/src/parse/language.c > $(NEGDIR)/libcss/parse/language.c
	@sed -e 's/^static const bool css__nested_atrules = true;$$/static const bool css__nested_atrules = false;/' \
	     third_party/css/libcss/src/parse/parse.c > $(NEGDIR)/libcss/parse/parse.c
	@for f in css_vars.c libcss/parse/language.c libcss/parse/parse.c; do \
	    case $$f in css_vars.c) o=c/apps/browser/css_vars.c;; \
	                *) o=third_party/css/libcss/src/parse/$${f##*/};; esac; \
	    cmp -s $$o $(NEGDIR)/$$f && \
	      { echo "FAIL: audit-css-before patched nothing in $$o"; exit 1; } || true; \
	 done
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/language.c -o $(NEGDIR)/language_before.o
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/parse.c -o $(NEGDIR)/parse_before.o
	@cp $(BUILD)/libcss_host.a $(NEGDIR)/libcss_before.a
	@ar d $(NEGDIR)/libcss_before.a language.o parse.o
	@ar r $(NEGDIR)/libcss_before.a $(NEGDIR)/language_before.o $(NEGDIR)/parse_before.o
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ tests/unit/css_audit.c \
	    c/apps/browser/css_engine.c $(NEGDIR)/css_vars.c c/apps/browser/css_extra.c \
	    c/apps/browser/layout.c c/apps/browser/layout_text.c c/apps/browser/browser_paint.c $(GFX_SRC) \
	    $(HTML_PARSER_SRC) $(NEGDIR)/libcss_before.a

# --- test-css-modern: the constructs a 2020s stylesheet is written in -------
# @layer / @supports / @container (each of which used to take its whole block
# with it) and the CSS logical properties. See the header of
# tests/unit/css_modern_test.c for the measured reason each one is here.
CSSMODERN_SRC := tests/unit/css_modern_test.c tests/unit/css_hostmm.c c/apps/browser/css_engine.c \
                 c/apps/browser/css_vars.c c/apps/browser/css_extra.c
# test-css-web-negctl runs BOTH halves (VVAL_MAX truncation restored; the
# libcss group-rule branch disabled); anchoring it on this gate covers the
# css_modern half AND runs the css_vars half beside it. The css_vars half's
# own positive (css_vars_test) is defined in the root Makefile, which this
# package does not edit -- the wiring line for it is in testdebt's report.
# tools/audit_tests.py's NOT_CI drops every test-*-negctl from what CI
# runs on the assumption the positive runs it; this prerequisite line is
# what makes that assumption true. The control sat stranded in
# tests/audit-stranded.baseline from the day it landed -- excluded from
# CI and invoked by nobody, which reads exactly like a covered control.
test-css-modern: test-css-web-negctl $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_modern_test \
	    $(CSSMODERN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_modern_test

# --- test-css-selector-inert: an unknown pseudo costs its OWN selector -----
# parsePseudo()'s lut stopped being a gate and became a type/arity table: a
# pseudo-class/element it does not know now parses to a detail that matches
# NOTHING, instead of taking its rule -- and every sibling selector in the same
# comma list -- with it. Measured with `make css-selector-recovery-probe` over
# tests/fixtures/cssweb: 407 rules / 795 declarations refused for a selector
# reason before, 218 / 490 after.
#
# The gate asserts on `struct cstyle` AFTER css_apply()+css_extra_apply() on a
# real DOM, not on whether the parser returned OK: this line's own scar is a
# WPT runner that linked layout.c and never called it, reading 531/11152 with
# and without an entire grid implementation. See the header of
# tests/unit/css_selector_inert_test.c for the three properties it holds --
# recovered, matches nothing, and never matches too much.
#
# Its control is a PREREQUISITE of the target, not a line on ci-host:. 61
# controls in this tree are stranded exactly that way.
CSSINERT_SRC := tests/unit/css_selector_inert_test.c tests/unit/css_hostmm.c \
                c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                c/apps/browser/css_extra.c
test-css-selector-inert: test-css-selector-inert-negctl test-css-selector-all-inert-negctl $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_selector_inert_test \
	    $(CSSINERT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_selector_inert_test

# --- test-css-selector-inert-negctl: put the refusal back, it must FAIL -----
# The one line that changed, restored: an unrecognised pseudo returns
# CSS_INVALID again and takes its whole selector list with it. Built from a
# SED'd copy rather than from an #ifdef in the shipping file, for the same
# reason the two controls below are -- a knob that lives in the real source is
# a knob that can be left on. The sed is checked for having matched, because a
# control that silently patches nothing is the failure this whole category of
# gate exists to prevent.
test-css-selector-inert-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)/libcss/parse
	@sed -e 's|if (lut_idx == N_ELEMENTS(pseudo_lut)) {|if (lut_idx == N_ELEMENTS(pseudo_lut)) return CSS_INVALID; if (0) {|' \
	     third_party/css/libcss/src/parse/language.c > $(NEGDIR)/libcss/parse/language_inert.c
	@cmp -s third_party/css/libcss/src/parse/language.c $(NEGDIR)/libcss/parse/language_inert.c && \
	    { echo "FAIL: the inert-pseudo negative control patched nothing --"; \
	      echo "      the sed no longer matches parsePseudo(), so it proves nothing."; exit 1; } || true
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/language_inert.c -o $(NEGDIR)/language_inert.o
	@cp $(BUILD)/libcss_host.a $(NEGDIR)/libcss_inert.a
	@ar d $(NEGDIR)/libcss_inert.a language.o
	@ar r $(NEGDIR)/libcss_inert.a $(NEGDIR)/language_inert.o
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_selector_inert_neg \
	    $(CSSINERT_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_inert.a -lm
	@if $(NEGDIR)/css_selector_inert_neg > $(NEGDIR)/inert.txt 2>&1; then \
	    echo "FAIL: with the unknown-pseudo refusal restored the gate still PASSED --"; \
	    echo "      it is not testing the selector recovery it claims to."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): refusal restored -> test-css-selector-inert fails, as required"; \
	    grep FAIL $(NEGDIR)/inert.txt | sed 's/^/       /'; \
	 fi

.PHONY: test-css-selector-inert test-css-selector-inert-negctl

# The opposite control: restore the rejected broad recovery rule by making
# the standards boundary conditional false. The ordinary-unknown assertions
# must then fail while the webkit compatibility assertions still pass.
test-css-selector-all-inert-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)/libcss-all-inert/parse
	@sed -e '/if (require_element == false ||/,/return CSS_INVALID;/s/return CSS_INVALID;/inert = inert;/' \
	     third_party/css/libcss/src/parse/language.c > $(NEGDIR)/libcss-all-inert/parse/language.c
	@cmp -s third_party/css/libcss/src/parse/language.c $(NEGDIR)/libcss-all-inert/parse/language.c && \
	    { echo "FAIL: the all-inert negative control patched nothing"; exit 1; } || true
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss-all-inert/parse/language.c -o $(NEGDIR)/libcss-all-inert/language.o
	@cp $(BUILD)/libcss_host.a $(NEGDIR)/libcss_all_inert.a
	@ar d $(NEGDIR)/libcss_all_inert.a language.o
	@ar r $(NEGDIR)/libcss_all_inert.a $(NEGDIR)/libcss-all-inert/language.o
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_selector_all_inert_neg \
	    $(CSSINERT_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_all_inert.a -lm
	@if $(NEGDIR)/css_selector_all_inert_neg > $(NEGDIR)/all_inert.txt 2>&1; then \
	    echo "FAIL: the rejected all-unknown-inert parser passed the boundary gate"; exit 1; \
	 else \
	    grep -q 'FAIL unknown pseudo-class invalidates' $(NEGDIR)/all_inert.txt || \
	      { echo "FAIL: all-inert control failed somewhere else:"; cat $(NEGDIR)/all_inert.txt; exit 1; }; \
	    echo "PASS (negative control): all-unknown-inert fails on ordinary pseudo-classes"; \
	 fi

.PHONY: test-css-selector-all-inert-negctl

# --- test-css-selstatic: the four STATIC pseudo-classes + the @supports pair
# css_engine.c handed LibCSS h_false for :checked, :disabled, :enabled and
# :target, so every rule behind them was selected away -- 578 uses across the
# 15-site corpus. They are answerable with no re-style machinery at all (an
# attribute, a form control's stored state, the URL fragment), which is why
# :hover/:active/:focus are deliberately NOT in this gate.
#
# The same gate carries the @supports half, because the two halves fail the
# same way: an engine that answers a question about itself wrongly. Each
# @supports case asserts the TRUE branch was taken AND that the property really
# reaches cstyle.xraw[] -- a test of the boolean alone would be testing
# logit_css_extra_supports_name() against itself. See the header of
# tests/unit/css_selstatic_test.c.
#
# Both controls are PREREQUISITES of the target rather than lines on ci-host:,
# which is what actually runs them; 61 controls in this tree are stranded the
# other way.
CSSSELSTATIC_SRC := tests/unit/css_selstatic_test.c tests/unit/css_hostmm.c \
                    c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                    c/apps/browser/css_extra.c
test-css-selstatic: test-css-selstatic-negctl test-css-selstatic-supports-negctl \
                    $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_selstatic_test \
	    $(CSSSELSTATIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_selstatic_test

# --- test-css-selstatic-negctl: the four handlers back to h_false ----------
# The exact prior state, and only that: the four entries of g_handler are
# reverted, so :checked/:disabled/:enabled/:target select nothing again while
# every other handler, the whole cascade and css_extra.c stay as shipped.
#
# SED'd from the shipping file rather than switched by an #ifdef inside it, for
# the reason the inert-pseudo control above gives: a knob that lives in the real
# source is a knob that can be left on. The sed is checked for having matched,
# because a control that patches nothing passes for the wrong reason -- which is
# the failure this whole category of gate exists to prevent.
test-css-selstatic-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)
	@sed -e 's|    h_node_is_enabled, h_node_is_disabled, h_node_is_checked,|    h_false /*enabled*/, h_false /*disabled*/, h_false /*checked*/,|' \
	     -e 's|    h_node_is_target, h_node_is_lang,|    h_false /*target*/, h_node_is_lang,|' \
	     c/apps/browser/css_engine.c > $(NEGDIR)/css_engine_hfalse.c
	@cmp -s c/apps/browser/css_engine.c $(NEGDIR)/css_engine_hfalse.c && \
	    { echo "FAIL: the static-pseudo negative control patched nothing --"; \
	      echo "      the four handlers are no longer where the sed expects them."; exit 1; } || true
	@grep -q "h_false /\*checked\*/" $(NEGDIR)/css_engine_hfalse.c || \
	    { echo "FAIL: the control did not restore h_false for :checked"; exit 1; }
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_selstatic_neg \
	    tests/unit/css_selstatic_test.c tests/unit/css_hostmm.c \
	    $(NEGDIR)/css_engine_hfalse.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@if $(NEGDIR)/css_selstatic_neg > $(NEGDIR)/selstatic.txt 2>&1; then \
	    echo "FAIL: with the four handlers back on h_false the gate still PASSED --"; \
	    echo "      it is not testing the selectors it claims to."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): handlers reverted -> test-css-selstatic fails, as required"; \
	    grep FAIL $(NEGDIR)/selstatic.txt | head -6 | sed 's/^/       /'; \
	 fi

# --- test-css-selstatic-supports-negctl: the PRODUCER reverted -------------
# Not the @supports list -- the thing it is derived FROM. Under
# CSS_NEGCTL_NO_XCAPTURE, parse_xraw() is a no-op, so nothing produces
# transform/transform-origin/box-shadow AND xr_names() answers nothing, which
# is the property the derivation buys: the capability and the claim of the
# capability go off together. A hand-copied list would still affirm transform
# here, and that pass would be the says-YES-cannot-do-it lie.
#
# The gate must go red for BOTH reasons at once, and its output is printed so
# the two are visible: the TRUE branch is no longer taken, and the value no
# longer reaches cstyle.xraw[].
test-css-selstatic-supports-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)
	@$(CC) -O2 -w -DCSS_NEGCTL_NO_XCAPTURE $(BTEST_INC) $(CSS_INC) \
	    -o $(NEGDIR)/css_selstatic_supneg \
	    $(CSSSELSTATIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@if $(NEGDIR)/css_selstatic_supneg > $(NEGDIR)/selstatic_sup.txt 2>&1; then \
	    echo "FAIL: with the xraw producer reverted the gate still PASSED --"; \
	    echo "      @supports is answering from a list, not from the producer."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): producer reverted -> test-css-selstatic fails, as required"; \
	    grep FAIL $(NEGDIR)/selstatic_sup.txt | head -6 | sed 's/^/       /'; \
	 fi

.PHONY: test-css-selstatic test-css-selstatic-negctl test-css-selstatic-supports-negctl

# --- css-selstatic-census: what the four handlers are worth on real bytes ---
# A MEASUREMENT, not a gate -- `test-` is deliberately not the prefix, for the
# reason css-drop-probe's comment above gives (tools/audit_tests.py counts any
# `test-*` with a recipe as a suite CI runs, and a target that reports rather
# than asserts must not be counted as one).
#
# It runs the corpus through the shipped engine and through the SAME engine
# with only the four handlers reverted to h_false, digesting every element's
# computed style over every property LibCSS knows, and diffs. The differing
# lines are the elements whose style the slice changed -- measured, never
# inferred from how often ":checked" appears in a sheet.
#
# THE NUMBER IS REACH, NOT CORRECTNESS, and the order matters: a handler that
# matched too much would make this number BIGGER. test-css-selstatic is what
# says the matching is right; this only says how far it goes. Read it second.
#
#   make css-selstatic-census
#   make css-selstatic-census CENSUS_FRAG=main   # :target with a fragment
CENSUS_SRC  := tests/unit/css_selstatic_census.c \
               c/apps/browser/css_vars.c c/apps/browser/css_extra.c \
               c/apps/browser/layout.c c/apps/browser/layout_text.c \
               c/apps/browser/browser_paint.c
CENSUS_FRAG ?=
css-selstatic-census: test-css-selstatic-negctl $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_census_after \
	    $(CENSUS_SRC) c/apps/browser/css_engine.c $(GFX_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_census_before \
	    $(CENSUS_SRC) $(NEGDIR)/css_engine_hfalse.c $(GFX_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(NEGDIR)/css_census_before $(if $(CENSUS_FRAG),--frag=$(CENSUS_FRAG),) \
	    $(AUDIT_DIRS) > $(BUILD)/census_before.txt 2> $(BUILD)/census_before.match
	@$(BUILD)/css_census_after  $(if $(CENSUS_FRAG),--frag=$(CENSUS_FRAG),) \
	    $(AUDIT_DIRS) > $(BUILD)/census_after.txt 2> $(BUILD)/census_after.match
	@python3 tests/unit/css_selstatic_census.py \
	    $(BUILD)/census_before.txt $(BUILD)/census_after.txt \
	    $(BUILD)/census_before.match $(BUILD)/census_after.match

.PHONY: css-selstatic-census

# --- test-css-selstatic-os: the same slice, ON THE MACHINE -----------------
# test-css-selstatic asserts on `struct cstyle` in a host process. This line's
# own scar is why that is not the end of it: `make test-wpt ONLY=css/css-grid`
# read 531/11152 with and WITHOUT the grid implementation, because the runner
# linked layout.c and never called it. Linking a TU is not running it.
#
# So this boots LogitOS and measures the SCREENDUMP: `input:checked + label`
# must paint the checked box's label and NOT the unchecked one's, and the
# @supports TRUE branch must not merely be taken but must MOVE the box -- a
# feature test that only checks the boolean is testing the table, not the
# engine. See the docstring in tests/qmp/qmp_css_selstatic.py.
test-css-selstatic-os: $(ISO) $(DISK)
	python3 tests/qmp/qmp_css_selstatic.py $(ISO) $(DISK)

.PHONY: test-css-selstatic-os

# --- test-css-border-radius: border-radius as a REAL cascaded property ------
# The first property moved OUT of css_extra.c's raw-text post-pass and INTO
# LibCSS's table -- five names (the shorthand + four corner longhands), four
# opcodes, a hand-written parser for the `<lp>{1,4} [ / <lp>{1,4} ]?` grammar
# the generator cannot express, and four generated computed-style slots.
#
# Measured with `make audit-css AUDIT_DIRS=tests/fixtures/cssweb/bing/`:
# border-radius 26 + border-bottom-right-radius 2 + border-bottom-left-radius 2
# = 30 declarations move from the DROP column to the accept column, and the
# page's `reach` goes 83% -> 86%. Over the whole corpus it is 1,832.
#
# The assertions that matter are NOT "is the box rounded" -- it was rounded
# before, by the wrong producer. They are specificity, !important, the exact
# selector, the four separate corners and em/rem lengths, none of which the
# deleted raw-text scan could do. See the header of
# tests/unit/css_border_radius_test.c.
#
# Its control is a PREREQUISITE of the target, not a line on ci-host:.
CSSRADIUS_SRC := tests/unit/css_border_radius_test.c tests/unit/css_hostmm.c \
                 c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                 c/apps/browser/css_extra.c
test-css-border-radius: test-css-border-radius-negctl $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_border_radius_test \
	    $(CSSRADIUS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_border_radius_test

# --- test-css-border-radius-negctl: take the five names back out -----------
# The property block's five SMAP() entries are replaced with names no
# stylesheet uses, so parseProperty()'s linear scan cannot find `border-radius`
# and the declaration is dropped again -- exactly the state before this change,
# with the handler table, the opcodes and the computed style all still present.
# That is the point: it isolates the TABLE ENTRY, so a pass here would mean the
# gate is measuring something the property table does not control.
#
# It also proves the gate is not passing on css_extra.c's old producer, which
# was deleted in the same commit: if that scan were still there, this control
# would still round the boxes and the gate would still pass.
test-css-border-radius-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)/libcss/parse
	@sed -e 's|SMAP("border-radius")|SMAP("logitos-no-radius-0")|' \
	     -e 's|SMAP("border-top-left-radius")|SMAP("logitos-no-radius-1")|' \
	     -e 's|SMAP("border-top-right-radius")|SMAP("logitos-no-radius-2")|' \
	     -e 's|SMAP("border-bottom-right-radius")|SMAP("logitos-no-radius-3")|' \
	     -e 's|SMAP("border-bottom-left-radius")|SMAP("logitos-no-radius-4")|' \
	     third_party/css/libcss/src/parse/propstrings.c > $(NEGDIR)/libcss/parse/propstrings_norad.c
	@cmp -s third_party/css/libcss/src/parse/propstrings.c $(NEGDIR)/libcss/parse/propstrings_norad.c && \
	    { echo "FAIL: the border-radius negative control patched nothing --"; \
	      echo "      the five SMAP() names are not where the sed expects them."; exit 1; } || true
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/propstrings_norad.c -o $(NEGDIR)/propstrings_norad.o
	@cp $(BUILD)/libcss_host.a $(NEGDIR)/libcss_norad.a
	@ar d $(NEGDIR)/libcss_norad.a propstrings.o
	@ar r $(NEGDIR)/libcss_norad.a $(NEGDIR)/propstrings_norad.o
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_border_radius_neg \
	    $(CSSRADIUS_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_norad.a -lm
	@if $(NEGDIR)/css_border_radius_neg > $(NEGDIR)/radius.txt 2>&1; then \
	    echo "FAIL: with border-radius removed from the property table the gate still PASSED --"; \
	    echo "      it is not testing the property it claims to."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): name removed -> test-css-border-radius fails, as required"; \
	    grep FAIL $(NEGDIR)/radius.txt | head -4 | sed 's/^/       /'; \
	 fi

.PHONY: test-css-border-radius test-css-border-radius-negctl

# --- test-css-proptables: the three positional tables must agree -----------
# The one silent failure this slice's architecture introduces. propstrings.h's
# enum, propstrings.c's stringmap[] and parse/properties/properties.c's
# property_handlers[] are indexed by the same integer with nothing asserting
# that they line up, and a shear calls the WRONG PROPERTY'S PARSER under a
# right name -- which ACCEPTS, so it is a success in every drop counter.
# See the header of tests/unit/css_proptable_test.c.
CSSPROPTAB_SRC := tests/unit/css_proptable_test.c tests/unit/css_hostmm.c \
                  c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                  c/apps/browser/css_extra.c
test-css-proptables: test-css-proptables-negctl $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_proptable_test \
	    $(CSSPROPTAB_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_proptable_test

# --- test-css-proptables-negctl: shear the handler table by hand -----------
# Two ADJACENT entries of property_handlers[] are swapped -- the smallest edit
# that produces the failure, and the one a careless insertion produces at its
# boundary. The gate must go red. Watched failing, not asserted to.
test-css-proptables-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)/libcss/parse/properties
	@python3 -c 'import sys; \
	 src="third_party/css/libcss/src/parse/properties/properties.c"; \
	 dst=sys.argv[1]; L=open(src).read().split("\n"); \
	 a=L.index("\tcss__parse_border_collapse,"); \
	 assert L[a+1]=="\tcss__parse_border_color,", "not adjacent any more"; \
	 L[a],L[a+1]=L[a+1],L[a]; open(dst,"w").write("\n".join(L))' \
	 $(NEGDIR)/libcss/parse/properties/properties_swap.c
	@cmp -s third_party/css/libcss/src/parse/properties/properties.c \
	        $(NEGDIR)/libcss/parse/properties/properties_swap.c && \
	    { echo "FAIL: the property-table negative control patched nothing --"; \
	      echo "      border-collapse/border-color are no longer adjacent in the table."; exit 1; } || true
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/properties/properties_swap.c -o $(NEGDIR)/properties_swap.o
	@cp $(BUILD)/libcss_host.a $(NEGDIR)/libcss_swap.a
	@ar d $(NEGDIR)/libcss_swap.a properties.o
	@ar r $(NEGDIR)/libcss_swap.a $(NEGDIR)/properties_swap.o
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_proptable_neg \
	    $(CSSPROPTAB_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_swap.a -lm
	@if $(NEGDIR)/css_proptable_neg > $(NEGDIR)/proptab.txt 2>&1; then \
	    echo "FAIL: with two adjacent handlers swapped the table gate still PASSED --"; \
	    echo "      it cannot see the shear it exists for."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): handlers swapped -> test-css-proptables fails, as required"; \
	    grep FAIL $(NEGDIR)/proptab.txt | head -4 | sed 's/^/       /'; \
	 fi

.PHONY: test-css-proptables test-css-proptables-negctl

# --- check-cssgen: LibCSS's two code generators must be FIXPOINTS ----------
# select_generator.py emits autogenerated_computed.h / propget.h / propset.h /
# destroy.inc from select_config.py, and gen_parser emits
# src/parse/properties/autogen/*.c from properties.gen. Both reproduce the
# checked-in tree byte for byte today -- verified before this target was
# written, which is what makes it a gate that starts green and can only be
# reddened by a real mistake.
#
# The failure it exists for: someone hand-edits inside a generated header and
# the next regeneration silently reverts it. Same shape as
# `tools/gen_as_opcodes.py --check` and its marker-delimited region.
#
# It runs the generators into a COPY of the tree, never in place, so a red gate
# cannot also be a modified working tree.
CSSGEN_DIR := $(BUILD)/cssgen
check-cssgen:
	@rm -rf $(CSSGEN_DIR)
	@mkdir -p $(CSSGEN_DIR)
	@cp -R third_party/css/libcss $(CSSGEN_DIR)/libcss
	@cd $(CSSGEN_DIR)/libcss/src/select && python3 select_generator.py > /dev/null
	@ok=1; for f in autogenerated_computed.h autogenerated_propget.h \
	                autogenerated_propset.h autogenerated_destroy.inc; do \
	    cmp -s third_party/css/libcss/src/select/$$f \
	           $(CSSGEN_DIR)/libcss/src/select/$$f || \
	      { echo "FAIL: $$f is not what select_generator.py emits"; ok=0; }; \
	 done; \
	 [ $$ok = 1 ] && echo "PASS: select_generator.py reproduces all four checked-in headers byte for byte" || exit 1

.PHONY: check-cssgen

# --- audit-css-radius-before: the corpus with border-radius NOT in the table
# The before column for the border-radius slice, built the way
# audit-css-before and audit-css-inert-before are: same corpus, same viewport,
# same binary shape, ONE variable -- the five SMAP() names. It reuses the
# library test-css-border-radius-negctl already builds, so there is one sed in
# the tree for this change rather than two that can drift apart.
#
#   make audit-css-radius-before AUDIT_TOP=8 > /tmp/before
#   make audit-css              AUDIT_TOP=8 > /tmp/after
audit-css-radius-before: test-css-border-radius-negctl
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_audit_norad \
	    tests/unit/css_audit.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    c/apps/browser/css_extra.c c/apps/browser/layout.c c/apps/browser/layout_text.c \
	    c/apps/browser/browser_paint.c $(GFX_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_norad.a
	@$(NEGDIR)/css_audit_norad --top=$(AUDIT_TOP) $(AUDIT_DIRS)

.PHONY: audit-css-radius-before

# --- test-css-web-negctl: both new gates, disabled, must FAIL ---------------
# Two assertions in this tree would otherwise be unfalsifiable, so each is run
# again against a deliberately broken build of the file it guards:
#
#  1. css_vars.c's "a value that cannot be stored WHOLE is not stored in part".
#     The negative control restores the old fixed 192-byte field and its silent
#     truncation. css_vars_test's balance checks must then fail -- if they pass,
#     they are not testing the invariant, and the apple.com bug (its cascade saw
#     4% of its own declarations) could come back unnoticed.
#
#  2. language.c's conditional-group-rule branch. The negative control disables
#     it so @layer/@supports/@container fall back to `return CSS_INVALID` and
#     take their blocks with them. css_modern_test must then fail.
#
# Both are built from SED'd copies rather than from an #ifdef in the shipping
# file: a knob that lives in the real source is a knob that can be left on.
test-css-web-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)
	@sed -e 's/#define VVAL_MAX 8192/#define VVAL_MAX 192/' \
	     -e 's|char \*dst = (vlen < VVAL_MAX \&\& text_balanced(val, vlen))|if (vlen >= VVAL_MAX) vlen = VVAL_MAX - 1;\n    char *dst = (1)|' \
	     c/apps/browser/css_vars.c > $(NEGDIR)/css_vars.c
	@cmp -s c/apps/browser/css_vars.c $(NEGDIR)/css_vars.c && \
	    { echo "FAIL: the css_vars negative control patched nothing --"; \
	      echo "      the sed no longer matches the source, so it proves nothing."; exit 1; } || true
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_vars_neg \
	    tests/unit/css_vars_test.c tests/unit/css_hostmm.c $(NEGDIR)/css_vars.c \
	    c/apps/browser/css_engine.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@if $(NEGDIR)/css_vars_neg > $(NEGDIR)/vars.txt 2>&1; then \
	    echo "FAIL: with value truncation restored css_vars_test still PASSED --"; \
	    echo "      the balance assertions cannot fail, so they prove nothing."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): truncation restored -> css_vars_test fails, as required"; \
	    grep FAIL $(NEGDIR)/vars.txt | sed 's/^/       /'; \
	 fi
	@mkdir -p $(NEGDIR)/libcss/parse
	@sed -e 's/^static bool at_rule_is_group(const css_token \*kw)$$/static bool at_rule_is_group(const css_token *kw) { (void) kw; return false; }\nstatic bool at_rule_is_group_disabled(const css_token *kw)/' \
	     third_party/css/libcss/src/parse/language.c > $(NEGDIR)/libcss/parse/language.c
	@cmp -s third_party/css/libcss/src/parse/language.c $(NEGDIR)/libcss/parse/language.c && \
	    { echo "FAIL: the at-rule negative control patched nothing."; exit 1; } || true
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/language.c -o $(NEGDIR)/language_neg.o
	@cp $(BUILD)/libcss_host.a $(NEGDIR)/libcss_neg.a
	@ar d $(NEGDIR)/libcss_neg.a language.o
	@ar r $(NEGDIR)/libcss_neg.a $(NEGDIR)/language_neg.o
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_modern_neg \
	    $(CSSMODERN_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_neg.a -lm
	@if $(NEGDIR)/css_modern_neg > $(NEGDIR)/modern.txt 2>&1; then \
	    echo "FAIL: with the group-rule branch disabled css_modern_test still PASSED --"; \
	    echo "      @layer/@supports/@container are not actually being tested."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): group rules disabled -> css_modern_test fails, as required"; \
	    grep FAIL $(NEGDIR)/modern.txt | sed 's/^/       /'; \
	 fi

# --- css-drop-probe: what does the engine SILENTLY drop? -------------------
# A MEASUREMENT, not a gate, and named without a `test-` prefix on purpose:
# tools/audit_tests.py classifies any `test-*` with a recipe as a suite CI
# runs, and a target that reports rather than asserts must not be counted as a
# test (that is exactly what test-audit looks for).
#
# It answers the question audit-css cannot. audit-css ranks what a corpus of
# real pages loses; this asks, construct by construct, WHICH OF THE FOUR WAYS
# it was lost -- unknown at-rule (takes its block), unparseable selector (takes
# its rule, and every other selector in the same list), dropped declaration, or
# parsed-and-never-read -- because those need four different repairs and look
# identical from the outside. It also diffs @supports's answer against what the
# engine can really do, in both directions.
#
#   make css-drop-probe
# See the header of tests/unit/css_drop_probe.c.
CSSDROP_SRC := tests/unit/css_drop_probe.c tests/unit/css_hostmm.c \
               c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
               c/apps/browser/css_extra.c
css-drop-probe: $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_drop_probe \
	    $(CSSDROP_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_drop_probe

# --- css-selector-recovery-probe: how much of the pseudo_lut gap got closed?
# A MEASUREMENT, not a gate (see css-drop-probe's comment above for why
# `test-` is deliberately not this target's prefix). Answers, on the real
# tests/fixtures/cssweb corpus rather than a fixture this session wrote: of
# the rules THIS build's selector parser refuses outright (parsePseudo()'s
# pseudo_lut in third_party/css/libcss/src/parse/language.c -- one unknown
# pseudo-class/element takes the WHOLE rule, and every sibling selector in
# the same comma list, with it), how many rules and how many declarations?
#
# Two independent instruments cross-checked against each other -- see the
# header of tests/unit/css_selector_recovery_probe.c for why an isolated
# per-selector re-parse was tried first and produces false positives (it
# loses any @namespace earlier in the same real sheet). If the printed
# self-check count is non-zero, the printed loss numbers are a floor, not an
# exact count -- said so by the tool itself, not left for the reader to
# notice.
#   make css-selector-recovery-probe
css-selector-recovery-probe: $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(CSS_INC) -o $(BUILD)/css_sel_recovery \
	    tests/unit/css_selector_recovery_probe.c $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_sel_recovery tests/fixtures/cssweb

.PHONY: audit-css audit-css-before test-css-modern test-css-web-negctl css-drop-probe css-selector-recovery-probe

# --- test-css-modern-os: the same constructs, ON THE MACHINE ---------------
# test-css-modern asserts on `struct cstyle` and audit-css on the display list.
# This boots LogitOS, serves a fixture whose whole stylesheet is inside @layer
# (with a @supports nested in it) and a second one carrying a 208-byte custom
# property, and asserts on the SCREENDUMP -- three boxes with three different
# left edges and one shared top, which a block fallback cannot produce.
# See the docstring in tests/qmp/qmp_css_modern.py.
test-css-modern-os: $(ISO) $(DISK)
	python3 tests/qmp/qmp_css_modern.py $(ISO) $(DISK)

.PHONY: test-css-modern-os

# --- css-drop-probe-before: the same probe with the inert-pseudo change OFF
# Scratch comparison target for the selector-recovery slice: builds
# css_drop_probe against the SAME library the negative control builds (the
# unknown-pseudo refusal restored), so `make css-drop-probe-before` and
# `make css-drop-probe` differ in exactly one line of parsePseudo().
css-drop-probe-before: test-css-selector-inert-negctl
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_drop_probe_before \
	    $(CSSDROP_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_inert.a -lm
	@$(NEGDIR)/css_drop_probe_before

.PHONY: css-drop-probe-before

# --- audit-css-inert-before: the corpus with the inert-pseudo change OFF ----
# The before column for the selector-recovery slice, built the way
# audit-css-before is: same corpus, same viewport, same binary shape, ONE
# variable -- parsePseudo() refusing an unrecognised pseudo again. Reuses the
# library the negative control already builds, so there is one sed in the tree
# for this change rather than two that can drift apart.
audit-css-inert-before: test-css-selector-inert-negctl
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_audit_inert \
	    tests/unit/css_audit.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
	    c/apps/browser/css_extra.c c/apps/browser/layout.c c/apps/browser/layout_text.c \
	    c/apps/browser/browser_paint.c $(GFX_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_inert.a
	@$(NEGDIR)/css_audit_inert --top=$(AUDIT_TOP) $(AUDIT_DIRS)

.PHONY: audit-css-inert-before

# --- CI ---------------------------------------------------------------------
# The two new gates and the generator fixpoint, named on ci-host so the audit
# can see them. The CONTROLS are deliberately NOT on this line: each is already
# a prerequisite of the target it guards (test-css-border-radius:
# test-css-border-radius-negctl, test-css-proptables: test-css-proptables-negctl),
# which is what actually runs them. Naming a control here instead satisfies
# tools/audit_tests.py and runs it never, and 56 controls in this tree are
# stranded exactly that way.
ci-host: test-css-border-radius test-css-proptables check-cssgen \
         test-css-selstatic

# test-css-selstatic-os boots QEMU, so it belongs on the boot suite and not
# above. Its host twin is on ci-host and asserts the same four selectors
# against `struct cstyle`; this one asserts the SCREEN, because linking a
# translation unit is not running it -- the scar this line carries is a WPT
# runner that read 531/11152 with and without an entire grid implementation.
#
# test-css-modern-os joins it for the same reason with no host twin to lean
# on: @layer and 208-byte custom properties are only ever asserted on the
# screendump, and it sat unwired (reachable by nobody) since it landed.
ci-boot: test-css-selstatic-os test-css-modern-os

# --- css-selector-census: WHICH selectors lose, ranked by what they carry ---
# A MEASUREMENT, not a gate -- `test-` is deliberately not the prefix, for the
# reason css-drop-probe's comment above gives.
#
# css-selector-recovery-probe answers "how many rules does the PARSER refuse".
# This answers the much larger question underneath it: of the rules that parse
# perfectly, how many apply to NOTHING, and WHY -- with the reasons ranked by
# DECLARATIONS CARRIED rather than by rule count, and with the rules that match
# nothing because the page simply has no such element separated out and never
# ranked. See the header of tests/unit/css_selcensus.c, especially the
# "cheapest reason wins" rule and the CLASSIFIER CONTRADICTED control.
#
#   make css-selector-census
#   make css-selector-census CENSUS_TOP=40
CENSUS_TOP ?= 24
CSSCENSUS_SRC := tests/unit/css_selcensus.c tests/unit/css_hostmm.c \
                 c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                 c/apps/browser/css_extra.c
css-selector-census: $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_selcensus \
	    $(CSSCENSUS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_selcensus --top=$(CENSUS_TOP) $(AUDIT_DIRS)

.PHONY: css-selector-census

# test-css-selector-inert joins the ci-host line above it: same fragment,
# same shape (its own two negctls already sit on its prerequisite line), and
# unwired since it landed -- reachable by nobody, reading like coverage.
ci-host: test-css-selector-inert
