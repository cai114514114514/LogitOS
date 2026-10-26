# ol_display snapshot groups (c/lib/gfx/adapters/display_group.c): opacity
# groups and rounded-rect clips on opaque packed targets. The host test checks
# opacity (opacity-lerp), rounded corners incl. the analytic r > GFX_MASK_MAX
# path (rrect-corner), nesting, OL_LIMIT and a two-context interleave; it is
# built with -DGFX_NCTX=2 and defines gfx_ctx_hook (no fb.c).
# The control builds with -DOL_DISPLAY_GROUP_NO_RESTORE (end() keeps the drawn
# pixels) and must fail with 'FAIL opacity-lerp' and 'FAIL rrect-corner'.
DSG_SRC := tests/unit/display_group_test.c $(GFX_SRC)
DSG_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)
DSG_DEPS := $(DSG_SRC) $(GFX_HEADERS)

$(BUILD)/dsg/display_group_test: $(DSG_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DSG_FLAGS) $(DSG_SRC) -lm -o $@

$(BUILD)/dsg/display_group_test-negctl: $(DSG_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DSG_FLAGS) -DOL_DISPLAY_GROUP_NO_RESTORE $(DSG_SRC) -lm -o $@

test-display-snapshot-group-negctl: $(BUILD)/dsg/display_group_test-negctl
	@rc=0; $(BUILD)/dsg/display_group_test-negctl > $(BUILD)/dsg/negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/dsg/negctl.log; exit 1; }
	@grep -q '^FAIL opacity-lerp ' $(BUILD)/dsg/negctl.log || { echo "negctl did not fail with 'FAIL opacity-lerp'"; cat $(BUILD)/dsg/negctl.log; exit 1; }
	@grep -q '^FAIL rrect-corner ' $(BUILD)/dsg/negctl.log || { echo "negctl did not fail with 'FAIL rrect-corner'"; cat $(BUILD)/dsg/negctl.log; exit 1; }
	@echo "display-snapshot-group control: end() without restore failed as expected: $$(grep -m1 '^FAIL opacity-lerp ' $(BUILD)/dsg/negctl.log)"

test-display-snapshot-group: test-display-snapshot-group-negctl $(BUILD)/dsg/display_group_test
	$(BUILD)/dsg/display_group_test

# Budget: a corner-only group around a 1280x800 content blit <= 1.10x the
# plain blit, a full opacity group <= 3.0x (tests/unit/bench_budget.h).
DSG_BENCH_SRC := tests/unit/display_group_bench.c $(GFX_SRC)

$(BUILD)/dsg/display_group_bench: $(DSG_BENCH_SRC) $(GFX_HEADERS) tests/unit/bench_budget.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -Itests/unit $(GFX_INC) $(DSG_BENCH_SRC) -lm -o $@

test-display-snapshot-group-budget: $(BUILD)/dsg/display_group_bench
	$(BUILD)/dsg/display_group_bench

test-gfx-budget: test-display-snapshot-group-budget
ci-host: test-display-snapshot-group
.PHONY: test-display-snapshot-group test-display-snapshot-group-negctl test-display-snapshot-group-budget
