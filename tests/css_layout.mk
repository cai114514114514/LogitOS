# Reuse the product DOM/CSS/layout source set. A copied numeric-only list would
# miss precisely the bridges this gate measures. Faults are positive-gate
# prerequisites, and each runs its own public geometry assertions to red.
CSS_LAYOUT_DIR := $(BUILD)/css-layout
CSS_LAYOUT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/css_layout_test.c
CSS_LAYOUT_DEPS = $(CSS_LAYOUT_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) \
 c/apps/gui/browser/layout/block.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/flex.h \
 c/apps/gui/browser/layout/ratio.h \
 c/apps/gui/browser/layout/grid.c c/apps/gui/browser/layout/grid.h c/apps/gui/browser/css/style.h \
 $(BROWSER_STYLE_MODULES) \
 $(BUILD)/libcss_host.a tests/css_layout.mk
.PHONY: test-css-layout test-css-layout-negctl test-css-layout-san

$(CSS_LAYOUT_DIR)/test: $(CSS_LAYOUT_DEPS)
	@mkdir -p $(CSS_LAYOUT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_LAYOUT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

$(CSS_LAYOUT_DIR)/no-ratio: CSS_LAYOUT_FAULT = CSS_LAYOUT_NO_RATIO
$(CSS_LAYOUT_DIR)/old-column: CSS_LAYOUT_FAULT = CSS_LAYOUT_COLUMN_FALLBACK
$(CSS_LAYOUT_DIR)/old-grid: CSS_LAYOUT_FAULT = CSS_LAYOUT_GRID_START_AS_NORMAL
$(CSS_LAYOUT_DIR)/no-grid-bounds: CSS_LAYOUT_FAULT = CSS_LAYOUT_GRID_IGNORE_BOUNDS
$(CSS_LAYOUT_DIR)/no-ratio $(CSS_LAYOUT_DIR)/old-column $(CSS_LAYOUT_DIR)/old-grid $(CSS_LAYOUT_DIR)/no-grid-bounds: $(CSS_LAYOUT_DEPS)
	@mkdir -p $(CSS_LAYOUT_DIR)
	@$(CC) -O2 -w -D$(CSS_LAYOUT_FAULT) $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_LAYOUT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

test-css-layout-negctl: $(CSS_LAYOUT_DIR)/no-ratio $(CSS_LAYOUT_DIR)/old-column $(CSS_LAYOUT_DIR)/old-grid $(CSS_LAYOUT_DIR)/no-grid-bounds
	@$(CSS_LAYOUT_DIR)/no-ratio ratio > $(CSS_LAYOUT_DIR)/no-ratio.log 2>&1; rc=$$?; cat $(CSS_LAYOUT_DIR)/no-ratio.log; test $$rc -eq 1 && grep -q 'FAIL: ratio: card aspect height' $(CSS_LAYOUT_DIR)/no-ratio.log
	@$(CSS_LAYOUT_DIR)/old-column column > $(CSS_LAYOUT_DIR)/old-column.log 2>&1; rc=$$?; cat $(CSS_LAYOUT_DIR)/old-column.log; test $$rc -eq 1 && grep -q 'FAIL: column: auto basis occupies main axis' $(CSS_LAYOUT_DIR)/old-column.log
	@$(CSS_LAYOUT_DIR)/old-grid grid > $(CSS_LAYOUT_DIR)/old-grid.log 2>&1; rc=$$?; cat $(CSS_LAYOUT_DIR)/old-grid.log; test $$rc -eq 1 && grep -q 'FAIL: grid: explicit start keeps auto track content width' $(CSS_LAYOUT_DIR)/old-grid.log
	@$(CSS_LAYOUT_DIR)/no-grid-bounds grid-bounds > $(CSS_LAYOUT_DIR)/no-grid-bounds.log 2>&1; rc=$$?; cat $(CSS_LAYOUT_DIR)/no-grid-bounds.log; test $$rc -eq 1 && grep -q 'FAIL: grid: auto track includes minimum-only empty item' $(CSS_LAYOUT_DIR)/no-grid-bounds.log

test-css-layout: test-css-layout-negctl $(CSS_LAYOUT_DIR)/test
	@$(CSS_LAYOUT_DIR)/test

$(CSS_LAYOUT_DIR)/san: $(CSS_LAYOUT_DEPS)
	@mkdir -p $(CSS_LAYOUT_DIR)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_LAYOUT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-css-layout-san: test-css-layout-negctl $(CSS_LAYOUT_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 $(CSS_LAYOUT_DIR)/san
# LibCSS's established host apparatus retains process-global sheets. This is
# address/undefined-behavior coverage, not a claim of global teardown/leak proof.
ci-host: test-css-layout
