# Keep output private while sharing only the established host CSS archive.
GRID_PERCENTAGE_ITEM_DIR := $(BUILD)/site-general/layout/grid-percentage-item
GRID_PERCENTAGE_ITEM_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/grid_percentage_item_test.c
GRID_PERCENTAGE_ITEM_DEPS = $(GRID_PERCENTAGE_ITEM_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a tests/grid_percentage_item.mk
.PHONY: test-grid-percentage-item test-grid-percentage-item-negctl
$(GRID_PERCENTAGE_ITEM_DIR)/test: $(GRID_PERCENTAGE_ITEM_DEPS)
	@mkdir -p $(GRID_PERCENTAGE_ITEM_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(GRID_PERCENTAGE_ITEM_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(GRID_PERCENTAGE_ITEM_DIR)/negctl: $(GRID_PERCENTAGE_ITEM_DEPS)
	@mkdir -p $(GRID_PERCENTAGE_ITEM_DIR)
	@$(CC) -O2 -w -DLAYOUT_GRID_PERCENT_CONTAINER $(BTEST_INC) $(CSS_INC) -o $@ $(GRID_PERCENTAGE_ITEM_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-grid-percentage-item-negctl: $(GRID_PERCENTAGE_ITEM_DIR)/negctl
	@$(GRID_PERCENTAGE_ITEM_DIR)/negctl > $(GRID_PERCENTAGE_ITEM_DIR)/negctl.log 2>&1; rc=$$?; cat $(GRID_PERCENTAGE_ITEM_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL: percent item resolves against area not entire grid' $(GRID_PERCENTAGE_ITEM_DIR)/negctl.log
test-grid-percentage-item: test-grid-percentage-item-negctl $(GRID_PERCENTAGE_ITEM_DIR)/test
	@$(GRID_PERCENTAGE_ITEM_DIR)/test
