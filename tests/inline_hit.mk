# The negative build restores the missing whitespace region. It must fail the
# SAME real pipeline test before the positive test can claim coverage.
# Keep the full painter pipeline (including css_extra/interp and SVG colour
# evaluator) in step with the existing paint gate; only its test main changes.
INLINE_HIT_SRC = $(filter-out tests/unit/paint_gfx_test.c,$(PAINTGFX_SRC)) tests/unit/inline_hit_test.c $(GFX_SRC) $(HTML_PARSER_SRC)
INLINE_HIT_DEPS = $(INLINE_HIT_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/browser_paint.h c/apps/browser/layout_flex.c c/apps/browser/layout_flex.h c/apps/browser/layout_grid.c c/apps/browser/layout_grid.h c/apps/browser/layout_text.h $(BUILD)/libcss_host.a
.PHONY: test-inline-hit test-inline-hit-negctl
test-inline-hit: test-inline-hit-negctl $(BUILD)/inline_hit_test
	$(BUILD)/inline_hit_test
$(BUILD)/inline_hit_test: $(INLINE_HIT_DEPS)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(INLINE_HIT_SRC) $(BUILD)/libcss_host.a -lm
test-inline-hit-negctl: $(BUILD)/libcss_host.a
	$(CC) -O2 -w -DLAYOUT_NO_INLINE_HIT $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $(BUILD)/inline_hit_negctl $(INLINE_HIT_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/inline_hit_negctl > $(BUILD)/inline_hit_negctl.log 2>&1; rc=$$?; cat $(BUILD)/inline_hit_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: plain collapsed space navigates' $(BUILD)/inline_hit_negctl.log
