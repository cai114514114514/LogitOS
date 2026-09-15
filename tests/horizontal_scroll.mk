# Match the existing complete paint pipeline, changing only the test entrypoint.
HSCROLL_SRC = $(filter-out tests/unit/paint_gfx_test.c,$(PAINTGFX_SRC)) tests/unit/horizontal_scroll_test.c $(GFX_SRC) $(HTML_PARSER_SRC)
HSCROLL_DEPS = $(HSCROLL_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/browser_paint.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a
.PHONY: test-horizontal-scroll test-horizontal-scroll-negctl
$(BUILD)/horizontal_scroll_test: $(HSCROLL_DEPS)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(HSCROLL_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/horizontal_scroll_negctl: $(HSCROLL_DEPS)
	$(CC) -O2 -w -DPAINT_NO_HORIZONTAL_SCROLL $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(HSCROLL_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/horizontal_extent_negctl: $(HSCROLL_DEPS)
	$(CC) -O2 -w -DPAINT_SCROLL_WIDTH_ITEMS $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(HSCROLL_SRC) $(BUILD)/libcss_host.a -lm
test-horizontal-scroll-negctl: $(BUILD)/horizontal_scroll_negctl $(BUILD)/horizontal_extent_negctl
	@$(BUILD)/horizontal_scroll_negctl > $(BUILD)/horizontal_scroll_negctl.log 2>&1; rc=$$?; cat $(BUILD)/horizontal_scroll_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: horizontal scroll translates actual text' $(BUILD)/horizontal_scroll_negctl.log
	@$(BUILD)/horizontal_extent_negctl > $(BUILD)/horizontal_extent_negctl.log 2>&1; rc=$$?; cat $(BUILD)/horizontal_extent_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: empty boxes contribute overflow without paint items' $(BUILD)/horizontal_extent_negctl.log && grep -q 'FAIL: transformed content contributes reachable overflow' $(BUILD)/horizontal_extent_negctl.log
test-horizontal-scroll: test-horizontal-scroll-negctl $(BUILD)/horizontal_scroll_test
	$(BUILD)/horizontal_scroll_test
