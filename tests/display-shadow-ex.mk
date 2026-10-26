# Analytic rounded-rect shadow (ol_display_shadow_ex). The control clamps sigma
# to the legacy 72px mask budget; the sigma40 cases must then fail.
DSX_SRC := tests/unit/display_shadow_ex_test.c $(GFX_SRC)
DSX_FLAGS := -O2 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)
DSX_DEPS := $(DSX_SRC) $(GFX_HEADERS)

$(BUILD)/display_shadow_ex_test: $(DSX_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DSX_FLAGS) $(DSX_SRC) -lm -o $@

$(BUILD)/display_shadow_ex_negctl: $(DSX_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DSX_FLAGS) -DOL_SHADOW_EX_NEGCTL_CLAMP $(DSX_SRC) -lm -o $@

test-display-shadow-ex-negctl: $(BUILD)/display_shadow_ex_negctl
	@rc=0; $(BUILD)/display_shadow_ex_negctl > $(BUILD)/display-shadow-ex-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/display-shadow-ex-negctl.log; exit 1; }
	@grep -q '^FAIL sigma40' $(BUILD)/display-shadow-ex-negctl.log || { echo "negctl did not fail with 'FAIL sigma40'"; cat $(BUILD)/display-shadow-ex-negctl.log; exit 1; }
	@! grep '^FAIL' $(BUILD)/display-shadow-ex-negctl.log | grep -v '^FAIL sigma40' | grep -q . || { echo "negctl failed for another reason"; cat $(BUILD)/display-shadow-ex-negctl.log; exit 1; }
	@echo "display-shadow-ex control: sigma clamped to the 72px budget failed: $$(grep '^FAIL sigma40' $(BUILD)/display-shadow-ex-negctl.log | head -1)"

test-display-shadow-ex: test-display-shadow-ex-negctl $(BUILD)/display_shadow_ex_test
	$(BUILD)/display_shadow_ex_test

# Budget: shadow_ex (r=12, sigma=8, 400x300) <= 1.5x the legacy ol_display_shadow.
DSXB_SRC := tests/unit/display_shadow_ex_bench.c $(GFX_SRC)
DSXB_FLAGS := -O2 -g -Wall -Wextra -DGFX_NCTX=1 -Itests/unit $(GFX_INC)

$(BUILD)/display_shadow_ex_bench: $(DSXB_SRC) $(GFX_HEADERS) tests/unit/bench_budget.h
	@mkdir -p $(dir $@)
	$(CC) $(DSXB_FLAGS) $(DSXB_SRC) -lm -o $@

test-display-shadow-ex-budget: $(BUILD)/display_shadow_ex_bench
	$(BUILD)/display_shadow_ex_bench

test-gfx-budget: test-display-shadow-ex-budget
ci-host: test-display-shadow-ex
.PHONY: test-display-shadow-ex test-display-shadow-ex-negctl test-display-shadow-ex-budget
