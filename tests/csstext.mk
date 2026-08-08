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

.PHONY: test-csstext test-csstext-negctl regen-linebreak-tables

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
