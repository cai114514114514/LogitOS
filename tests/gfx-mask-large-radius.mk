# tests/gfx-mask-large-radius.mk -- corner masks up to GFX_MASK_BIG (512) from a
# per-context byte arena. Built with -DGFX_NCTX=2 and no fb.c; the test defines
# gfx_ctx_hook and interleaves two contexts. The control builds with
# -DGFX_MASK_LEGACY_CAP (refuse past GFX_MASK_MAX) and must fail 'FAIL r180'.
GMLR_TEST  := tests/unit/gfx_mask_big_test.c
GMLR_FLAGS := -O2 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)

$(BUILD)/gfx_mask_big_test: $(GMLR_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GMLR_FLAGS) $< $(GFX_SRC) -lm -o $@

$(BUILD)/gfx_mask_big_legacy: $(GMLR_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GMLR_FLAGS) -DGFX_MASK_LEGACY_CAP $< $(GFX_SRC) -lm -o $@

test-gfx-mask-large-radius-negctl: $(BUILD)/gfx_mask_big_legacy
	@rc=0; $(BUILD)/gfx_mask_big_legacy > $(BUILD)/gfx-mask-big-legacy.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/gfx-mask-big-legacy.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc (expected a verdict, exit 1)"; exit 1; fi; \
	if ! grep -q '^FAIL r180$$' $(BUILD)/gfx-mask-big-legacy.log; then cat $(BUILD)/gfx-mask-big-legacy.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL r180'"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL r180$$' $(BUILD)/gfx-mask-big-legacy.log)"

test-gfx-mask-large-radius: test-gfx-mask-large-radius-negctl $(BUILD)/gfx_mask_big_test
	$(BUILD)/gfx_mask_big_test

ci-host: test-gfx-mask-large-radius
.PHONY: test-gfx-mask-large-radius test-gfx-mask-large-radius-negctl
