# Measure decoder entries through real nested flex trials; no stopwatch proxy.
SVG_LAYOUT_DIR := $(BUILD)/wiring/svg-layout
SVG_LAYOUT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/svg_layout_cache_test.c
SVG_LAYOUT_FLAGS ?= -O2
SVG_LAYOUT_DEPS = $(SVG_LAYOUT_SRC) $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/svg_dom_paint.inc c/apps/browser/svg_style_props.h $(BUILD)/libcss_host.a
.PHONY: test-svg-layout-cache test-svg-layout-cache-negctl
$(SVG_LAYOUT_DIR)/test: $(SVG_LAYOUT_DEPS)
	@mkdir -p $(SVG_LAYOUT_DIR)
	@$(CC) $(SVG_LAYOUT_FLAGS) -w $(BTEST_INC) $(CSS_INC) -o $@ $(SVG_LAYOUT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(SVG_LAYOUT_DIR)/negctl: $(SVG_LAYOUT_DEPS)
	@mkdir -p $(SVG_LAYOUT_DIR)
	@$(CC) $(SVG_LAYOUT_FLAGS) -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_SVG_REPEAT_DECODE -o $@ $(SVG_LAYOUT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-svg-layout-cache-negctl: $(SVG_LAYOUT_DIR)/negctl
	@rc=0; $< > $(SVG_LAYOUT_DIR)/negctl.log 2>&1 || rc=$$?; cat $(SVG_LAYOUT_DIR)/negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: one SVG decode across nested flex measurement trials' $(SVG_LAYOUT_DIR)/negctl.log
test-svg-layout-cache: test-svg-layout-cache-negctl $(SVG_LAYOUT_DIR)/test
	@$(SVG_LAYOUT_DIR)/test
