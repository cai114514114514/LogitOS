# The negative build restores the missing whitespace region. It must fail the
# SAME real pipeline test before the positive test can claim coverage.
# Keep the full painter pipeline (including css_extra/interp and SVG colour
# evaluator) in step with the existing paint gate; only its test main changes.
INLINE_HIT_SRC = $(filter-out tests/unit/paint_gfx_test.c,$(PAINTGFX_SRC)) tests/unit/inline_hit_test.c $(GFX_SRC) $(HTML_PARSER_SRC)
INLINE_HIT_DEPS = $(INLINE_HIT_SRC) c/apps/gui/browser/layout/block.h c/apps/gui/browser/css/style.h c/apps/gui/browser/paint/renderer.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/flex.h c/apps/gui/browser/layout/grid.c c/apps/gui/browser/layout/grid.h c/apps/gui/browser/layout/text.h $(BUILD)/libcss_host.a
$(BUILD)/inline_hit_test: $(INLINE_HIT_DEPS)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(INLINE_HIT_SRC) $(BUILD)/libcss_host.a -lm
