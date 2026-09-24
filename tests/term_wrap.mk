# tests/term_wrap.mk -- the Terminal wraps scrollback on CELLS, at a character
# boundary (c/apps/gui/terminal/term_line.h).
#
# The bug: terminal.c wrapped when a line's BYTES reached the window's width in
# CELLS, so a row of Chinese ended a third of the way across and the wrap could
# split one character between two rows. The gate drives the same header the
# Terminal compiles, byte by byte, and counts characters per row.
#
# The control compiles the old byte rule back into tl_fits and must fail; it
# runs first, so a green positive is never the only evidence.

.PHONY: test-term-wrap test-term-wrap-negctl

TERM_WRAP_INC := -Ic/apps/gui/terminal -Ic/apps/coreutils

test-term-wrap-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DTERM_WRAP_NEGCTL $(TERM_WRAP_INC) \
		-o $(BUILD)/term_wrap_negctl tests/unit/term_wrap_test.c
	@if ./$(BUILD)/term_wrap_negctl > $(BUILD)/term_wrap_negctl.log 2>&1; then \
		cat $(BUILD)/term_wrap_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: the suite passes with one column per byte"; exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/term_wrap_negctl.log) checks fail on the byte rule"; \
	fi

test-term-wrap: test-term-wrap-negctl
	@$(CC) -O2 -Wall -Wextra $(TERM_WRAP_INC) \
		-o $(BUILD)/term_wrap_test tests/unit/term_wrap_test.c
	@./$(BUILD)/term_wrap_test

ci-host: test-term-wrap
