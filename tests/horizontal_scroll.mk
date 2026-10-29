# Match the existing complete paint pipeline, changing only the test entrypoint.
HSCROLL_SRC = $(filter-out tests/unit/paint_gfx_test.c,$(PAINTGFX_SRC)) tests/unit/horizontal_scroll_test.c $(GFX_SRC) $(HTML_PARSER_SRC)
HSCROLL_DEPS = $(HSCROLL_SRC) c/apps/gui/browser/layout/block.h c/apps/gui/browser/css/style.h c/apps/gui/browser/paint/renderer.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/grid.c $(BUILD)/libcss_host.a
$(BUILD)/horizontal_scroll_test: $(HSCROLL_DEPS)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(HSCROLL_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/horizontal_scroll_negctl: $(HSCROLL_DEPS)
	$(CC) -O2 -w -DPAINT_NO_HORIZONTAL_SCROLL $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(HSCROLL_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/horizontal_extent_negctl: $(HSCROLL_DEPS)
	$(CC) -O2 -w -DPAINT_SCROLL_WIDTH_ITEMS $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(HSCROLL_SRC) $(BUILD)/libcss_host.a -lm
