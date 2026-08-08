# ============================================================================
# CSS value interpolation and `transform` as a value -- c/apps/browser/css_interp.{c,h}
#
# THE ORACLE IS WPT, TRANSCRIBED. Every expected value in
# tests/unit/interp_test.c is copied out of a WPT file rather than worked out
# here: the matrices from css/css-transforms/transform-2d-getComputedStyle-001
# .html, the interpolation triples from
# css/css-transforms/animation/transform-interpolation-001.html. A suite whose
# expectations are derived from the same reasoning as the implementation
# agrees with the implementation and measures nothing, and this project has
# now watched that happen more than once.
#
#   make test-css-interp          the suite (seconds, no corpus needed)
#   make test-css-interp-asan     the same under ASan + UBSan
#   make test-css-interp-negctl   componentwise-always instead of matrix
#                                 decomposition; the suite MUST fail, and this
#                                 target succeeds when it does
#
# WHY THE NEGATIVE CONTROL IS THE ONE IT IS. "Remove the interpolation" is not
# a control -- any suite catches that. The bug this file exists to not have is
# the one that LOOKS RIGHT: interpolate two transform lists componentwise even
# when their function lists differ. Every animation between two translate()s
# stays perfect, every page renders correctly, and an animation from a
# rotate() to a scale() gets a smooth, finite, plausible, wrong answer --
# matrix(-0.212958, 0.977061, ...) where the truth is
# matrix(1.06066, 1.06066, ...). Nothing about the page looks broken. Only a
# test that knows the number can tell.
#
# The control fails TWO checks and they are deliberately different questions:
# a counter says the decomposition path ran at all, and a value says it ran
# correctly. Either one alone can pass while the other fails -- a suite that
# only checks the output cannot distinguish componentwise-always from correct
# on the many pairs where the two happen to agree.
#
# tools/audit_tests.py classifies any `test-*` with a recipe as a host suite
# and `make ci` runs it, so test-css-interp needs no further wiring -- and
# test-css-interp is a real gate: it exits non-zero when a check fails. It is
# not one of the 22 harnesses that print FAIL and exit 0.
# ============================================================================
.PHONY: test-css-interp test-css-interp-asan test-css-interp-negctl

INTERP_SRC := c/apps/browser/css_interp.c
INTERP_CF  := -O1 -g -Wall -Wextra -Ic/apps/browser

test-css-interp: $(BUILD)/interp_test
	@$(BUILD)/interp_test

$(BUILD)/interp_test: tests/unit/interp_test.c $(INTERP_SRC) c/apps/browser/css_interp.h
	@mkdir -p $(BUILD)
	@$(CC) $(INTERP_CF) -o $@ tests/unit/interp_test.c $(INTERP_SRC) -lm

# ASan matters more here than it looks: the parser walks caller-supplied byte
# spans with no NUL guarantee and the serialisers write into fixed buffers, so
# the interesting failures are one-past-the-end rather than wrong arithmetic.
test-css-interp-asan:
	@mkdir -p $(BUILD)
	@$(CC) $(INTERP_CF) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/interp_test_asan tests/unit/interp_test.c $(INTERP_SRC) -lm
	@$(BUILD)/interp_test_asan

test-css-interp-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(INTERP_CF) -DCI_NEGCTL_NO_DECOMPOSE \
	    -o $(BUILD)/interp_negctl tests/unit/interp_test.c $(INTERP_SRC) -lm
	@if $(BUILD)/interp_negctl > $(BUILD)/interp_negctl.log 2>&1; then \
	    echo "test-css-interp-negctl: FAILED -- the suite passed with transform"; \
	    echo "  lists interpolated componentwise regardless of whether their"; \
	    echo "  function lists match. It is therefore not measuring the"; \
	    echo "  decomposition, which is the only part of this file that is hard."; \
	    exit 1; \
	 else \
	    echo "test-css-interp-negctl: ok -- the suite catches componentwise-always:"; \
	    grep -E 'FAIL|checks' $(BUILD)/interp_negctl.log | head -6; \
	 fi
