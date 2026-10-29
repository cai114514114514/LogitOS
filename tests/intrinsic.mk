# Same real parser/style/layout pipeline as layout-box; only the test main changes.
# Restore the old max(child)/empty-control measurement in a prerequisite build.
INTRINSIC_DIR := $(BUILD)/intrinsic
INTRINSIC_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/intrinsic_test.c
INTRINSIC_DEPS = $(INTRINSIC_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/layout/block.h c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/grid.c $(BUILD)/libcss_host.a
$(INTRINSIC_DIR)/test: $(INTRINSIC_DEPS)
	@mkdir -p $(INTRINSIC_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(INTRINSIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(INTRINSIC_DIR)/negctl: $(INTRINSIC_DEPS)
	@mkdir -p $(INTRINSIC_DIR)
	$(CC) -O2 -w -DLAYOUT_INTRINSIC_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(INTRINSIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
