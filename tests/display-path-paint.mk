# ol_display_fill_mask/fill_path/stroke_path (adapters/display_path.c): packed-target
# paint fills checked against gfx_fill on straight RGBA. The control builds with
# -DOL_DISPLAY_PATH_IGNORE_PAINT (every paint becomes solid) so the gradient cases
# must fail ('FAIL linear').
DPP_SRC := tests/unit/display_path_test.c $(GFX_SRC)
DPP_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)
DPP_DEPS := $(DPP_SRC) $(GFX_HEADERS)

$(BUILD)/display_path_test: $(DPP_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DPP_FLAGS) $(DPP_SRC) -lm -o $@

$(BUILD)/display_path_negctl: $(DPP_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DPP_FLAGS) -DOL_DISPLAY_PATH_IGNORE_PAINT $(DPP_SRC) -lm -o $@

test-display-path-paint-negctl: $(BUILD)/display_path_negctl
	@rc=0; $(BUILD)/display_path_negctl > $(BUILD)/display-path-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/display-path-negctl.log; exit 1; }
	@grep -q '^FAIL linear$$' $(BUILD)/display-path-negctl.log || { echo "negctl did not fail with 'FAIL linear'"; cat $(BUILD)/display-path-negctl.log; exit 1; }
	@echo 'display-path-paint control: solid-only paint failed FAIL linear.'

test-display-path-paint: test-display-path-paint-negctl $(BUILD)/display_path_test
	$(BUILD)/display_path_test

ci-host: test-display-path-paint

# Budget: fill_path of rrect paths (40x24 r=8 and others) at 100% <= 2.0x gfx_fill into a
# same-format gfx surface (display adapter overhead). blend_round_rect ratio is info only.
DPPB_SRC := tests/unit/display_path_bench.c $(GFX_SRC)
DPPB_FLAGS := -O2 -g -Wall -Wextra -Itests/unit $(GFX_INC)

$(BUILD)/display_path_bench: $(DPPB_SRC) $(GFX_HEADERS) tests/unit/bench_budget.h
	@mkdir -p $(dir $@)
	$(CC) $(DPPB_FLAGS) $(DPPB_SRC) -lm -o $@

test-display-path-paint-budget: $(BUILD)/display_path_bench
	$(BUILD)/display_path_bench

test-gfx-budget: test-display-path-paint-budget

.PHONY: test-display-path-paint test-display-path-paint-negctl test-display-path-paint-budget
