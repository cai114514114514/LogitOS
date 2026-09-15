FLEX_COLUMN_BRIDGE_DIR := $(BUILD)/site-general/continue/flex-column
FLEX_COLUMN_BRIDGE_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/flex_column_bridge_test.c
FLEX_COLUMN_BRIDGE_DEPS = $(FLEX_COLUMN_BRIDGE_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a tests/flex_column_bridge.mk
.PHONY: test-flex-column-bridge test-flex-column-bridge-negctl
$(FLEX_COLUMN_BRIDGE_DIR)/test: $(FLEX_COLUMN_BRIDGE_DEPS)
	@mkdir -p $(FLEX_COLUMN_BRIDGE_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(FLEX_COLUMN_BRIDGE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(FLEX_COLUMN_BRIDGE_DIR)/legacy: $(FLEX_COLUMN_BRIDGE_DEPS)
	@mkdir -p $(FLEX_COLUMN_BRIDGE_DIR)
	@$(CC) -O2 -w -DLAYOUT_FLEX_COLUMN_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(FLEX_COLUMN_BRIDGE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-flex-column-bridge-negctl: $(FLEX_COLUMN_BRIDGE_DIR)/legacy
	@$(FLEX_COLUMN_BRIDGE_DIR)/legacy > $(FLEX_COLUMN_BRIDGE_DIR)/legacy.log 2>&1; rc=$$?; cat $(FLEX_COLUMN_BRIDGE_DIR)/legacy.log; test $$rc -eq 1 && grep -q 'FAIL: column scrollport consumes remaining main size' $(FLEX_COLUMN_BRIDGE_DIR)/legacy.log
test-flex-column-bridge: test-flex-column-bridge-negctl $(FLEX_COLUMN_BRIDGE_DIR)/test
	@$(FLEX_COLUMN_BRIDGE_DIR)/test
