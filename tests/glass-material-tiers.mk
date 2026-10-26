# tests/glass-material-tiers.mk -- glass blur tiers (THIN/REGULAR/THICK/ULTRA).
# The test is built with -DGFX_NCTX=2 and no fb.c; it defines gfx_ctx_hook and
# interleaves two displays on different tiers. The control collapses THICK to a
# single box pass, so the gaussian comparison has to fail.
GLASS_TIERS_TEST := tests/unit/glass_tiers_test.c
GLASS_TIERS_BENCH := tests/unit/glass_tiers_bench.c
GLASS_TIERS_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)

$(BUILD)/glass_tiers_test: $(GLASS_TIERS_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GLASS_TIERS_FLAGS) $< $(GFX_SRC) -lm -o $@

$(BUILD)/glass_tiers_single_pass: $(GLASS_TIERS_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GLASS_TIERS_FLAGS) -DGLASS_TIERS_NEGCTL_SINGLE_PASS $< $(GFX_SRC) -lm -o $@

$(BUILD)/glass_tiers_sanitize: $(GLASS_TIERS_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GLASS_TIERS_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $< $(GFX_SRC) -lm -o $@

$(BUILD)/glass_tiers_bench: $(GLASS_TIERS_BENCH) tests/unit/bench_budget.h $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wextra -DGFX_NCTX=2 -Itests/unit $(GFX_INC) $< $(GFX_SRC) -lm -o $@

test-glass-material-tiers-negctl: $(BUILD)/glass_tiers_single_pass
	@rc=0; $(BUILD)/glass_tiers_single_pass > $(BUILD)/glass-tiers-single-pass.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg '^FAIL gaussian THICK$$' $(BUILD)/glass-tiers-single-pass.log
	@echo 'Glass tier control: a single-pass THICK failed the gaussian comparison.'

test-glass-material-tiers: test-glass-material-tiers-negctl $(BUILD)/glass_tiers_test $(BUILD)/glass_tiers_sanitize
	$(BUILD)/glass_tiers_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/glass_tiers_sanitize > /dev/null

test-glass-material-tiers-budget: $(BUILD)/glass_tiers_bench
	$(BUILD)/glass_tiers_bench

test-gfx-budget: test-glass-material-tiers-budget
ci-host: test-glass-material-tiers
.PHONY: test-glass-material-tiers test-glass-material-tiers-negctl test-glass-material-tiers-budget
