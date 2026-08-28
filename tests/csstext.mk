# tests/csstext.mk -- the CSS Text line's measurement (inline layout and line
# breaking).
#
# Own fragment for the reason every other tests/*.mk gives: several agents share
# this tree, and a whole-file Makefile snapshot written from a stale working
# copy has silently deleted other people's targets more than once.
#
#   make test-csstext          the gate: UAX #14 conformance + the CSS layer
#   make test-csstext-negctl   the negative control -- MUST fail
#   make regen-linebreak-tables  rebuild linebreak_data.inc from the host UCD
#
# WHAT THE GATE ACTUALLY MEASURES, because "line breaking works" is not a
# claim anyone should accept on assertion. The bulk of it is the Unicode
# Consortium's own conformance corpus, LineBreakTest.txt, run whole: 16,672
# cases, not one of them written here, each stating the string AND every break
# opportunity in it. It is a differential against the specification's own test
# data, in the same shape as `make test-bidi` next door -- and it needs no
# font, no frame buffer and no browser, which is why this line had a scoreboard
# before the reftest harness existed. Reftests, when they land, ask a DIFFERENT
# question: whether the right pixels appear. Whether the breaks are in the
# right places is this file's question and it stays answered.
#
# The remaining ~80 checks are hand-written and they are ours: white-space
# collapsing and the segment-break transformation, text-transform, the
# letter-spacing/word-spacing/tab-stop arithmetic, text-align (including
# justify landing the last fragment exactly on the margin), text-indent, and
# the four tailorings. Each is stated as an exact position or an exact byte
# string. None of them is "it did not crash".

UCD ?= /usr/share/unicode

CSSTEXT_SRC := tests/unit/csstext_test.c c/apps/browser/layout_text.c
CSSTEXT_INC := -Ic/apps/browser

.PHONY: test-csstext test-csstext-negctl test-csstext-asan test-csstext-all
.PHONY: test-cjkwrap test-cjkwrap-negctl test-cjkwrap-ws-negctl test-csstext-wired
.PHONY: regen-linebreak-tables

# All of them. A gate without its control is half a measurement -- the gate
# says the suite passes, the control says the suite could have failed.
#
# THE THREE NEW ONES ARE A DIFFERENT QUESTION FROM THE OLD THREE, and the
# difference is the whole reason this line existed for months with a green
# scoreboard and an unreadable browser: csstext_test measures layout_text.c,
# and a gate on a module cannot see whether anything CONSUMES the module.
# Measured 2026-08-28, before the wiring: `nm build/browser.elf | grep -ci
# 'lbrk|linebreak|layout_text'` = 0, against 19 `grid_` symbols as the control.
# 103 checks passed and every Chinese paragraph on the machine was chopped at
# an arbitrary column.
#
#   test-csstext-wired   every link line naming layout.c also names
#                        layout_text.c -- the link, checked
#   test-cjkwrap         where layout_page() actually puts the line ends
#   test-cjkwrap-negctl  the same, with layout.c back on the any-boundary cut
#   test-cjkwrap-ws-negctl  the same, with the invented inter-run space back
test-csstext-all: test-csstext test-csstext-asan test-csstext-negctl \
                  test-csstext-wired test-cjkwrap test-cjkwrap-negctl \
                  test-cjkwrap-ws-negctl
	@echo "test-csstext-all: gates green, negative controls red (as required)"

# --- the link, checked ----------------------------------------------------
# layout.c calls ltx_break_utf8(), so 31 hand-written link lines across eleven
# files grew a dependency at once. CLAUDE.md's third failure shape is exactly
# this ("a source file grew a dependency and the link line did not follow") and
# it is credited with taking down six gates. Joins make's continuations first,
# which is rule 2, and refuses to pass on zero matches, which is rule 5.
test-csstext-wired:
	@python3 tests/unit/csstext_wired.py .

# --- does the BROWSER take the breaks? ------------------------------------
CJKW_SRC := tests/unit/cjkwrap_test.c c/apps/browser/layout.c \
            c/apps/browser/layout_text.c c/apps/browser/css_engine.c \
            c/apps/browser/css_vars.c c/apps/browser/css_extra.c

$(BUILD)/cjkwrap_test: $(CJKW_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a \
                       c/apps/browser/layout.h c/apps/browser/layout_text.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(CJKW_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

# THE CONTROLS ARE PREREQUISITES, which is CLAUDE.md rule 5's worked fix and
# not the one that only looks like it: a control named on a `ci-host:` line
# satisfies the UNWIRED audit and still runs never. Both of these exit 0 when
# they correctly fail, so this ordering is "prove the suite can fail, then run
# it".
test-cjkwrap: test-cjkwrap-negctl test-cjkwrap-ws-negctl $(BUILD)/cjkwrap_test
	@$(BUILD)/cjkwrap_test

# THE CONTROL, and note what it is NOT: it does not delete layout_text.c from
# the link. -DLAYOUT_NO_UAX14 makes layout.c's lb_opps() return NULL, which
# puts the file back on exactly the cut it shipped with -- the largest prefix
# that fits, at whatever UTF-8 boundary that lands on. That build is what the
# owner has been reading Chinese pages on. This target requires it to FAIL.
$(BUILD)/cjkwrap_test_negctl: $(CJKW_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DLAYOUT_NO_UAX14 $(BTEST_INC) $(CSS_INC) -o $@ $(CJKW_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

test-cjkwrap-negctl: $(BUILD)/cjkwrap_test_negctl
	@if $(BUILD)/cjkwrap_test_negctl >$(BUILD)/cjkwrap_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the suite passes with UAX #14 switched off"; \
	    exit 1; \
	 else \
	    echo "negative control ok: with the any-boundary cut the suite reports"; \
	    grep -E '^  (FAIL|cjkwrap_test:)' $(BUILD)/cjkwrap_negctl.log \
	        | head -8 | sed 's/^/      /'; \
	 fi

# THE SECOND CONTROL, for the second half of the change. -DLAYOUT_NO_WS_COLLAPSE
# restores `if (f->line_started) f->x += spacew` -- the shipped behaviour, in
# which a space appears between any two runs on a started line whatever the
# source said. Two controls rather than one because the two defects are
# independent: line breaking can be right while the spacing is wrong, and a
# single control that switched both off could not tell which half a regression
# had landed in.
$(BUILD)/cjkwrap_test_wsnegctl: $(CJKW_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DLAYOUT_NO_WS_COLLAPSE $(BTEST_INC) $(CSS_INC) -o $@ $(CJKW_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

test-cjkwrap-ws-negctl: $(BUILD)/cjkwrap_test_wsnegctl
	@if $(BUILD)/cjkwrap_test_wsnegctl >$(BUILD)/cjkwrap_wsnegctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the suite passes with the invented space"; \
	    exit 1; \
	 else \
	    echo "negative control ok: with a space between every pair of runs"; \
	    grep -E '^  (FAIL|cjkwrap_test:)' $(BUILD)/cjkwrap_wsnegctl.log \
	        | head -8 | sed 's/^/      /'; \
	 fi

# The same suite under the sanitisers, which is what makes the fuzz sweep at
# the end of it worth running. Every string this module sees came off the
# network: truncated UTF-8, lone continuation bytes, a megabyte of soft
# hyphens. The conformance corpus is all WELL-FORMED by construction and
# therefore asks none of those questions. Without a sanitiser "it did not
# crash" is a weak claim about 4,000 random inputs; with one it is a check on
# every access those inputs caused.
test-csstext-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Wall -Wextra -Wno-unused-function -o $(BUILD)/csstext_test_asan \
	    $(CSSTEXT_SRC) $(CSSTEXT_INC)
	@$(BUILD)/csstext_test_asan $(UCD) 20000

test-csstext:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -Wno-unused-function -o $(BUILD)/csstext_test \
	    $(CSSTEXT_SRC) $(CSSTEXT_INC)
	@$(BUILD)/csstext_test $(UCD)

# THE NEGATIVE CONTROL, and note what it is NOT: it does not delete line
# breaking. Deleting it would fail everything and prove nothing -- of course a
# build with no algorithm fails a test of the algorithm.
#
# What it substitutes instead is the PLAUSIBLE WRONG ANSWER: break after
# U+0020, honour the hard line-break characters, never break inside a
# multi-byte character. That is a competent implementation. It is what
# c/apps/browser/layout.c does today, and it is what almost every hand-rolled
# inline layout does, because on English prose it is indistinguishable from
# correct -- an English-only test suite passes it outright.
#
# It has no idea that Chinese exists. This target requires the suite to notice.
test-csstext-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DCSSTEXT_BREAK_ON_SPACE_ONLY -o $(BUILD)/csstext_test_negctl \
	    $(CSSTEXT_SRC) $(CSSTEXT_INC)
	@if $(BUILD)/csstext_test_negctl $(UCD) >$(BUILD)/csstext_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the suite passes a space-only line breaker"; \
	    exit 1; \
	 else \
	    echo "negative control ok: with breaks only at U+0020 the suite reports"; \
	    grep -E '^  (LineBreakTest|FAIL|[0-9]+ checks)' $(BUILD)/csstext_negctl.log \
	        | head -8 | sed 's/^/      /'; \
	 fi

# Rebuild the Unicode tables from the host UCD. Not part of a normal build: the
# .inc is committed, exactly like c/crypto/trust/roots_bundle.inc and
# c/lib/text/bidi_data.inc. Re-run it when the UCD moves to a new version, and
# expect `make test-csstext` to move with it -- the corpus is versioned too, and
# the table and the corpus must come from the SAME UCD or the disagreements are
# the version gap rather than the code.
regen-linebreak-tables:
	@python3 tools/linebreak_gen.py --ucd $(UCD) --out c/apps/browser

# Both new gates are host-only, need no corpus and no QEMU, and run in about a
# second each. test-csstext itself is deliberately NOT here: it refuses without
# the Unicode corpus, which this host does not have, and weakening that refusal
# would trade a loud skip for a quiet pass.
ci-host: test-cjkwrap test-csstext-wired
