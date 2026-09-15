# Same real parser/style/layout pipeline as layout-box; only the test main changes.
# Restore the old max(child)/empty-control measurement in a prerequisite build.
INTRINSIC_DIR := $(BUILD)/intrinsic
INTRINSIC_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/intrinsic_test.c
INTRINSIC_DEPS = $(INTRINSIC_SRC) $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a
.PHONY: test-intrinsic test-intrinsic-negctl
$(INTRINSIC_DIR)/test: $(INTRINSIC_DEPS)
	@mkdir -p $(INTRINSIC_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(INTRINSIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(INTRINSIC_DIR)/negctl: $(INTRINSIC_DEPS)
	@mkdir -p $(INTRINSIC_DIR)
	$(CC) -O2 -w -DLAYOUT_INTRINSIC_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(INTRINSIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-intrinsic-negctl: $(INTRINSIC_DIR)/negctl
	@$(INTRINSIC_DIR)/negctl > $(INTRINSIC_DIR)/negctl.log 2>&1; rc=$$?; cat $(INTRINSIC_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL: nested inline widths are summed' $(INTRINSIC_DIR)/negctl.log && grep -q 'FAIL: text input contributes its intrinsic box' $(INTRINSIC_DIR)/negctl.log
test-intrinsic: test-intrinsic-negctl $(INTRINSIC_DIR)/test
	@$(INTRINSIC_DIR)/test
