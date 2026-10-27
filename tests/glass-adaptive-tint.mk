# tests/glass-adaptive-tint.mk -- OL_GLASS_ADAPT_SMOOTH / OL_GLASS_ADAPT_FG.
# Built with -DGFX_NCTX=2 and no fb.c; the test defines gfx_ctx_hook. The
# control builds with -DGLASS_ADAPT_HARD_THRESHOLD, which makes SMOOTH use the
# hard thresholds, so the tone-sweep continuity check must fail.
GAT_TEST := tests/unit/glass_adapt_test.c
GAT_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)

$(BUILD)/glass_adapt_test: $(GAT_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GAT_FLAGS) $< $(GFX_SRC) -lm -o $@

$(BUILD)/glass_adapt_hard: $(GAT_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GAT_FLAGS) -DGLASS_ADAPT_HARD_THRESHOLD $< $(GFX_SRC) -lm -o $@

# Exit 1 (checks failed, not a crash), the continuity failure present, and no
# other failure: parity, FG contrast and reduce transparency still pass.
test-glass-adaptive-tint-negctl: $(BUILD)/glass_adapt_hard
	@rc=0; $(BUILD)/glass_adapt_hard > $(BUILD)/glass-adapt-hard.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg '^FAIL continuity ' $(BUILD)/glass-adapt-hard.log
	@! rg '^FAIL ' $(BUILD)/glass-adapt-hard.log | rg -v '^FAIL continuity( |$$)'
	@echo 'Glass adaptive tint control: hard thresholds failed the tone-sweep continuity check.'

test-glass-adaptive-tint: test-glass-adaptive-tint-negctl $(BUILD)/glass_adapt_test
	$(BUILD)/glass_adapt_test

ci-host: test-glass-adaptive-tint
.PHONY: test-glass-adaptive-tint test-glass-adaptive-tint-negctl
