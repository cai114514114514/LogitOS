# tests/display-rrect-fast.mk -- ol_display_round_rect fast path (corner-only coverage).
# The harness compares the production functions with verbatim copies of the old
# per-pixel loops over 2000 random rect/radius/clip/colour combinations. The
# control compiles openlogit_display.c with -DOL_RRECT_FAST_NEGCTL (corners
# written solid), which must fail "FAIL identity". Same sources/flags as the
# positive; only the -D differs.
DRRF_SRC := tests/unit/display_rrect_test.c c/lib/gfx/adapters/openlogit_display.c
DRRF_INC := -Itests/unit $(GFX_INC)
DRRF_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror

.PHONY: test-display-rrect-fast test-display-rrect-fast-negctl test-display-rrect-fast-budget

$(BUILD)/display_rrect_test: $(DRRF_SRC) tests/unit/bench_budget.h tests/display-rrect-fast.mk
	@mkdir -p $(BUILD)
	@$(CC) $(DRRF_CFLAGS) $(DRRF_INC) $(DRRF_SRC) -o $@

$(BUILD)/display_rrect_test_negctl: $(DRRF_SRC) tests/unit/bench_budget.h tests/display-rrect-fast.mk
	@mkdir -p $(BUILD)
	@$(CC) $(DRRF_CFLAGS) -DOL_RRECT_FAST_NEGCTL $(DRRF_INC) $(DRRF_SRC) -o $@

test-display-rrect-fast-negctl: $(BUILD)/display_rrect_test_negctl
	@rc=0; $< > $(BUILD)/display_rrect_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -q '^FAIL identity round_rect' $(BUILD)/display_rrect_negctl.log && \
	 echo "negctl ok: $$(grep '^FAIL identity' $(BUILD)/display_rrect_negctl.log | head -1)" || \
	 { cat $(BUILD)/display_rrect_negctl.log; echo "NEGCTL did not fail with FAIL identity (rc=$$rc)"; exit 1; }

test-display-rrect-fast: test-display-rrect-fast-negctl $(BUILD)/display_rrect_test
	@$(BUILD)/display_rrect_test

test-display-rrect-fast-budget: $(BUILD)/display_rrect_test
	@$(BUILD)/display_rrect_test budget

test-gfx-budget: test-display-rrect-fast-budget
ci-host: test-display-rrect-fast
