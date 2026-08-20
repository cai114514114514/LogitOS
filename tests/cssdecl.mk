# tests/cssdecl.mk -- the declarations LibCSS drops on the floor.
#
#   make test-cssdecl          the gate: a declaration string in, values out
#   make test-cssdecl-negctl   TWO controls, both of which MUST fail
#
# Own fragment for the reason every other tests/*.mk gives, and this line paid
# for it inside one session: a concurrent session in this same worktree ran
# `git reset` while these edits were live and took the working tree back to
# HEAD. A fragment is the smallest thing that can be re-applied.
#
# WHAT IS BEING GATED. `transform`, `transform-origin`, `box-shadow` and every
# gradient value are absent from the vendored LibCSS property table -- not
# mis-parsed, absent -- so the cascade cannot carry them and css_extra.c's
# raw-declaration scan is their only producer. The suite has two halves that
# fail independently: the CAPTURE (does the span reach cstyle.xraw, from the
# longhand AND the shorthand AND the prefixed spelling AND an inline style=)
# and the PARSE (does the span become the right numbers, and are the values we
# cannot render REFUSED rather than approximated).
#
# THE LINK IS DELIBERATELY SMALL, and it is the design being protected rather
# than a convenience. Measured with the continuations JOINED (CLAUDE.md says
# why, and this number read "fourteen" until the joiner was fixed): css_extra.c
# is named by 18 host source lists -- 4 in the Makefile, 14 across nine
# tests/*.mk fragments -- and not one of them carries c/lib/gfx, svg.c or libm.
# So the parsers it exports are integer-only and reach for none of them, and
# this fragment adds exactly ONE source the others do not have -- css_interp.c,
# and only because the wiring half proves a captured transform span really does
# turn into a matrix. If a future edit here needs svg.c or -lm to compile
# css_extra.c itself, the constraint has been broken and those 18 targets are
# about to fail in files nobody touched.
.PHONY: test-cssdecl test-cssdecl-negctl

CSSDECL_DIR := $(BUILD)/cssdecl

CSSDECL_SRC := tests/unit/css_paintdecl_test.c \
               c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
               c/apps/browser/css_extra.c c/apps/browser/css_interp.c

# $1 = output binary, $2 = extra -D flags
define CSSDECL_BUILD
	@mkdir -p $(CSSDECL_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(2) -o $(1) \
	    $(CSSDECL_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
endef

$(CSSDECL_DIR)/cssdecl_test: $(CSSDECL_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call CSSDECL_BUILD,$@,)

$(CSSDECL_DIR)/cssdecl_nocapture: $(CSSDECL_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call CSSDECL_BUILD,$@,-DCSS_NEGCTL_NO_XCAPTURE)

$(CSSDECL_DIR)/cssdecl_pctaspx: $(CSSDECL_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call CSSDECL_BUILD,$@,-DCSS_NEGCTL_GRAD_PCT_AS_PX)

test-cssdecl: $(CSSDECL_DIR)/cssdecl_test test-cssdecl-negctl
	@$(CSSDECL_DIR)/cssdecl_test

# THE CONTROLS. Two, because the two halves of this line fail differently and
# one control only covers one of them. Each is required to redden an EXACT
# count, not merely to fail: a control that reddens everything proves the
# binary was rebuilt and nothing else.
#
#   NO_XCAPTURE   reverts the capture -- parse_xraw() becomes a no-op and
#                 cstyle.xraw[] stays NULL. The helpers stay compiled, so the
#                 control differs from the shipped build in one BEHAVIOUR and
#                 not in what the file contains. MEASURED: it reddens exactly
#                 14 rows -- the 11 capture rows that assert a span arrived,
#                 the 2 wiring rows, and the one rem row that reads a captured
#                 shadow -- and NOTHING else. All 67 parser rows keep passing,
#                 because the parsers are pure functions of their argument and
#                 the tables call them directly; so do the two capture rows
#                 that assert a span is ABSENT and the css_root_px() row, which
#                 measures css_engine.c rather than the scan. That asymmetry is
#                 the useful part: it says which rows measure the scan and
#                 which measure the grammar.
#
#                 The number moved 11 -> 13 -> 14 across one session as rows
#                 landed, and the control is what reported each step -- it
#                 refused the run and printed the new row names rather than
#                 passing on a changed suite. That is the count being an
#                 assertion and not a comment. Re-measure it, never adjust it
#                 to whatever the run printed: the two failures it is built to
#                 catch are "a parser row started depending on the capture"
#                 and "a capture row stopped measuring it", and both of those
#                 also just move the number.
#
#   GRAD_PCT_AS_PX is the PLAUSIBLE wrong implementation rather than the absent
#                 one, and it is a bug this tree has already paid for once:
#                 CLAUDE.md's M17 section records `padding-top:56.25%` stored
#                 as fifty-six pixels, found only by instrumenting what was
#                 PAINTED. Here a stop's percentage is stored as a pixel count.
#                 Nothing is zero, nothing overflows, every gradient still has
#                 its stops in the right order with the right colours, and only
#                 the rows whose stops carry a percentage move. MEASURED:
#                 exactly 4 of the 32 gradient rows.
test-cssdecl-negctl: $(CSSDECL_DIR)/cssdecl_nocapture $(CSSDECL_DIR)/cssdecl_pctaspx
	@if $(CSSDECL_DIR)/cssdecl_nocapture > $(CSSDECL_DIR)/nocapture.log 2>&1; then \
	   echo "test-cssdecl-negctl: FAILED -- the suite PASSED with the capture reverted,"; \
	   echo "  so nothing in it is measuring css_extra's scan."; exit 1; \
	 else \
	   n=`grep -c '^FAIL:' $(CSSDECL_DIR)/nocapture.log`; \
	   if [ "$$n" != "14" ]; then \
	     echo "test-cssdecl-negctl: FAILED -- NO_XCAPTURE reddened $$n rows, expected 14."; \
	     echo "  Either a parser row started depending on the capture, or a capture"; \
	     echo "  row stopped. Both are worth looking at; neither is a rebuild."; \
	     grep '^FAIL:' $(CSSDECL_DIR)/nocapture.log; exit 1; \
	   fi; \
	   echo "test-cssdecl-negctl: ok -- reverting the capture reddens exactly $$n rows:"; \
	   grep '^FAIL:' $(CSSDECL_DIR)/nocapture.log | head -4; \
	 fi
	@if $(CSSDECL_DIR)/cssdecl_pctaspx > $(CSSDECL_DIR)/pctaspx.log 2>&1; then \
	   echo "test-cssdecl-negctl: FAILED -- the suite PASSED with a gradient stop's"; \
	   echo "  percentage stored as a pixel count."; exit 1; \
	 else \
	   n=`grep -c '^FAIL:' $(CSSDECL_DIR)/pctaspx.log`; \
	   if [ "$$n" != "4" ]; then \
	     echo "test-cssdecl-negctl: FAILED -- GRAD_PCT_AS_PX reddened $$n rows, expected 4."; \
	     grep '^FAIL:' $(CSSDECL_DIR)/pctaspx.log; exit 1; \
	   fi; \
	   echo "test-cssdecl-negctl: ok -- a percentage read as pixels reddens exactly $$n rows:"; \
	   grep '^FAIL:' $(CSSDECL_DIR)/pctaspx.log; \
	 fi

# Named on the suite so it runs, and its control is a prerequisite of the
# positive above so the control runs too -- the two halves of not being in
# tests/audit-stranded.baseline. `ci-host:` accepts prerequisites from any
# fragment, so membership is this one line in the file that owns the target.
ci-host: test-cssdecl
