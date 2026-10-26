# tests/text-linebreak-lift.mk -- c/lib/text/linebreak.{h,c} (tl_break_utf8) and
# the shared UAX #14 core (linebreak_core.inc) the browser's layout/text.c
# includes.
#
#   make test-text-linebreak-lift         hand-written exact breaks (mandatory,
#                                         CJK, emoji ZWJ, combining marks, the
#                                         line-break / word-break flags, invalid
#                                         UTF-8) + a differential against the
#                                         browser's ltx_break_utf8 on random text
#   make test-text-linebreak-lift-negctl  same sources with
#                                         -DTL_LINEBREAK_NEGCTL (a break after
#                                         every code point); MUST fail
#   make test-text-linebreak-lift-corpus UCD=<Unicode 16.0.0 UCD>
#                                         the whole LineBreakTest.txt against
#                                         tl_break_utf8; refuses loudly without
#                                         the corpus and is NOT in ci-host
#                                         (like test-csstext)
#
# The browser is linked with text.c by dozens of host link lines, so the core is
# an .inc that both files include; nothing new is linked into the browser.

UCD ?= /usr/share/unicode

TLLB_SRC := tests/unit/tl_linebreak_test.c c/lib/text/linebreak.c \
            c/apps/gui/browser/layout/text.c
TLLB_INC := -Ic/lib/text $(BROWSER_INC)
TLLB_CFL := -O2 -Wall -Wextra -Wno-unused-function

.PHONY: test-text-linebreak-lift test-text-linebreak-lift-negctl
.PHONY: test-text-linebreak-lift-corpus test-text-linebreak-lift-corpus-negctl

# The control breaks after every code point. It must fail the exact-position
# table (the stated reason), not crash or fail to build.
test-text-linebreak-lift-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(TLLB_CFL) -DTL_LINEBREAK_NEGCTL -o $(BUILD)/tl_linebreak_test_negctl \
	    $(TLLB_SRC) $(TLLB_INC)
	@if $(BUILD)/tl_linebreak_test_negctl >$(BUILD)/tl_linebreak_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the suite passes a break-everywhere wrapper"; \
	    exit 1; \
	 elif ! grep -q '^FAIL expected-breaks' $(BUILD)/tl_linebreak_negctl.log; then \
	    cat $(BUILD)/tl_linebreak_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL expected-breaks'"; \
	    exit 1; \
	 else \
	    echo "negative control ok: $$(grep -c '^FAIL expected-breaks' $(BUILD)/tl_linebreak_negctl.log) expected-breaks cases fail with a break after every code point"; \
	    grep -m1 '^FAIL expected-breaks' $(BUILD)/tl_linebreak_negctl.log | sed 's/^/      /'; \
	 fi

test-text-linebreak-lift: test-text-linebreak-lift-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(TLLB_CFL) -o $(BUILD)/tl_linebreak_test $(TLLB_SRC) $(TLLB_INC)
	@$(BUILD)/tl_linebreak_test

test-text-linebreak-lift-corpus-negctl:
	@mkdir -p $(BUILD)
	@if [ ! -f "$(UCD)/auxiliary/LineBreakTest.txt" ]; then \
	    echo "test-text-linebreak-lift-corpus-negctl: no LineBreakTest.txt under UCD=$(UCD)"; \
	    echo "  (pass UCD=<a Unicode 16.0.0 UCD>; refusing to pass quietly)"; exit 1; fi
	@$(CC) $(TLLB_CFL) -DTL_LINEBREAK_NEGCTL -o $(BUILD)/tl_linebreak_corpus_negctl \
	    $(TLLB_SRC) $(TLLB_INC)
	@if $(BUILD)/tl_linebreak_corpus_negctl $(UCD) >$(BUILD)/tl_linebreak_corpus_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the corpus passes a break-everywhere wrapper"; \
	    exit 1; \
	 elif ! grep -q '^FAIL LineBreakTest: [0-9]* cases disagree' $(BUILD)/tl_linebreak_corpus_negctl.log; then \
	    cat $(BUILD)/tl_linebreak_corpus_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL LineBreakTest: N cases disagree'"; \
	    exit 1; \
	 else \
	    echo "negative control ok:"; \
	    grep -E '^(FAIL LineBreakTest|  LineBreakTest)' $(BUILD)/tl_linebreak_corpus_negctl.log | sed 's/^/      /'; \
	 fi

test-text-linebreak-lift-corpus: test-text-linebreak-lift-corpus-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(TLLB_CFL) -o $(BUILD)/tl_linebreak_corpus $(TLLB_SRC) $(TLLB_INC)
	@$(BUILD)/tl_linebreak_corpus $(UCD)

ci-host: test-text-linebreak-lift

# The corpus gate is NOT unconditionally on ci-host: like test-csstext it refuses
# without LineBreakTest.txt, and this host has no UCD. It is wired (so test-audit
# does not report an orphan) only where the corpus exists; elsewhere ci-host
# neither runs it nor pretends it passed.
ifneq ($(wildcard $(UCD)/auxiliary/LineBreakTest.txt),)
ci-host: test-text-linebreak-lift-corpus
endif
