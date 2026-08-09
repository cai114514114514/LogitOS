# ============================================================================
# CSS value interpolation, `transform` as a value, and Element.prototype.animate
#   c/apps/browser/css_interp.{c,h}   the value math
#   c/apps/browser/js_anim.c          the binding that makes it observable
#
# THE ORACLE IS WPT, TRANSCRIBED. Every expected value in these two suites is
# copied out of a WPT file rather than worked out here: the matrices from
# css/css-transforms/transform-2d-getComputedStyle-001.html, the interpolation
# triples from css/css-transforms/animation/transform-interpolation-001.html,
# and createEasing() verbatim out of css/support/interpolation-testcommon.js.
# A suite whose expectations are derived from the same reasoning as the
# implementation agrees with the implementation and measures nothing, and this
# project has now watched that happen more than once.
#
#   make test-css-interp          the value math (seconds, no corpus needed)
#   make test-css-interp-asan     the same under ASan + UBSan
#   make test-css-interp-negctl   componentwise-always instead of matrix
#                                 decomposition; the suite MUST fail, and this
#                                 target succeeds when it does
#   make test-css-interp-accum-negctl
#                                 `accumulate` implemented as `add`; MUST fail
#   make test-css-anim            the animate() timing math (seconds)
#   make test-css-anim-negctl     a cubic-bezier clamped to [0,1]; MUST fail
#
# WHY THESE THREE NEGATIVE CONTROLS AND NOT "DELETE THE FEATURE". Any suite
# catches a deletion. All three are the implementation that LOOKS RIGHT:
#
#   css_interp: interpolate two transform lists componentwise even when their
#   function lists differ. Every animation between two translate()s stays
#   perfect, every page renders correctly, and rotate() against scale() gets a
#   smooth, finite, plausible, wrong answer -- matrix(-0.212958, 0.977061, ...)
#   where the truth is matrix(1.06066, 1.06066, ...). Nothing looks broken.
#   The control fails TWO checks on purpose, and they are different questions:
#   a counter says the decomposition path ran at all, a value says it ran
#   correctly. Either can pass while the other fails, because the two agree on
#   many pairs.
#
#   css_interp, composite: implement `accumulate` as `add`. For every scalar
#   type the two ARE the same operation -- a length, a percentage, a number and
#   a colour channel all sum either way -- so every one of the 20 scalar
#   composite checks still passes and so does most of the corpus. The
#   distinction lives entirely in list-valued types: add CONCATENATES,
#   accumulate combines componentwise, and a scale factor accumulates as
#   a + b - 1. `scaleX(2)` with `scaleX(3)` is `scaleX(2) scaleX(3)` under add
#   -- a factor of six -- and `scaleX(4)` under accumulate. Both render, both
#   are smooth, and nothing about the page looks broken. The control fails on
#   the transform checks and ONLY those, which is also how a reader can tell
#   which half of this suite is doing the work.
#
#   js_anim: clamp the timing function's output to [0, 1], which is what
#   "progress is a fraction" produces if you write it without thinking. The
#   corpus never advances a timeline -- duration 100s, currentTime 50s, so the
#   input progress is always exactly 0.5 -- and every distinct `at` in all 337
#   interpolation files comes out of cubic-bezier(0, b, 1, b) with
#   b = (8y-1)/6. Clamping collapses the two of every seven subtests whose
#   `at` is -0.3 or 1.5 onto an endpoint, and nothing else changes.
#
# tools/audit_tests.py classifies any `test-*` with a recipe as a host suite
# and `make ci` runs it, so neither needs further wiring -- and both are real
# gates: they exit non-zero when a check fails, so neither is one of the 22
# harnesses that print FAIL and exit 0.
# ============================================================================
.PHONY: test-css-interp test-css-interp-asan test-css-interp-negctl
.PHONY: test-css-interp-accum-negctl
.PHONY: test-css-anim test-css-anim-negctl

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

# `accumulate` as `add`. The one thing to watch for in the output: the failures
# must be the TRANSFORM checks and not the scalar ones. A control that also
# breaks `50px + 100px` would be a different bug from the one being modelled,
# and the grep below prints them so a reader can see which fired.
test-css-interp-accum-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(INTERP_CF) -DCI_NEGCTL_ACCUM_IS_ADD \
	    -o $(BUILD)/interp_accum_negctl tests/unit/interp_test.c $(INTERP_SRC) -lm
	@if $(BUILD)/interp_accum_negctl > $(BUILD)/interp_accum_negctl.log 2>&1; then \
	    echo "test-css-interp-accum-negctl: FAILED -- the suite passed with"; \
	    echo "  accumulate implemented as add. Every scalar composite behaves"; \
	    echo "  identically under that bug, so a suite that stays green here is"; \
	    echo "  testing lengths and numbers and is not testing accumulation at"; \
	    echo "  all -- which is precisely the shape of the mistake."; \
	    exit 1; \
	 else \
	    echo "test-css-interp-accum-negctl: ok -- the suite catches accumulate==add:"; \
	    grep -E 'FAIL|checks' $(BUILD)/interp_accum_negctl.log | head -8; \
	 fi

# -DCONFIG_BIGNUM is neither optional nor cargo-cult: libbf's decimal path
# (bfdec_normalize_and_round) is compiled out without it while quickjs.c
# references it anyway, so the link fails on an undefined symbol that has
# nothing to do with this test. CLAUDE.md carries the same note for the host
# LibCSS tests, which hit it first.
ANIM_CF  := -O1 -g -w -Ic/apps/browser -Ithird_party/quickjs -Ithird_party/libm \
            -DCONFIG_VERSION='"host"' -DCONFIG_BIGNUM
ANIM_SRC := c/apps/browser/js_anim.c c/apps/browser/css_interp.c $(QJS_SRC)

test-css-anim: $(BUILD)/anim_test
	@$(BUILD)/anim_test

$(BUILD)/anim_test: tests/unit/anim_test.c c/apps/browser/js_anim.c $(INTERP_SRC)
	@mkdir -p $(BUILD)
	@$(CC) $(ANIM_CF) -o $@ tests/unit/anim_test.c $(ANIM_SRC) -lm -lpthread

test-css-anim-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(ANIM_CF) -DJS_ANIM_NEGCTL_CLAMP -o $(BUILD)/anim_negctl \
	    tests/unit/anim_test.c $(ANIM_SRC) -lm -lpthread
	@if $(BUILD)/anim_negctl > $(BUILD)/anim_negctl.log 2>&1; then \
	    echo "test-css-anim-negctl: FAILED -- the suite passed with the timing"; \
	    echo "  function clamped to [0,1], so it is not measuring the progress"; \
	    echo "  values outside the unit interval -- which is two of every seven"; \
	    echo "  subtests in all 337 interpolation files."; \
	    exit 1; \
	 else \
	    echo "test-css-anim-negctl: ok -- the suite catches a clamped easing:"; \
	    grep -E 'FAIL|checks' $(BUILD)/anim_negctl.log | head -6; \
	 fi
