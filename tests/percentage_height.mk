# Keep output private while sharing only the established host CSS archive.
PERCENT_HEIGHT_DIR := $(BUILD)/site-general/layout/percentage-height
PERCENT_HEIGHT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/percentage_height_test.c
PERCENT_HEIGHT_DEPS = $(PERCENT_HEIGHT_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a tests/percentage_height.mk
.PHONY: test-percentage-height test-percentage-height-negctl
$(PERCENT_HEIGHT_DIR)/test: $(PERCENT_HEIGHT_DEPS)
	@mkdir -p $(PERCENT_HEIGHT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(PERCENT_HEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(PERCENT_HEIGHT_DIR)/negctl: $(PERCENT_HEIGHT_DEPS)
	@mkdir -p $(PERCENT_HEIGHT_DIR)
	@$(CC) -O2 -w -DLAYOUT_PERCENT_HEIGHT_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(PERCENT_HEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-percentage-height-negctl: $(PERCENT_HEIGHT_DIR)/negctl
	@$(PERCENT_HEIGHT_DIR)/negctl > $(PERCENT_HEIGHT_DIR)/negctl.log 2>&1; rc=$$?; cat $(PERCENT_HEIGHT_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL: definite parent resolves percentage height' $(PERCENT_HEIGHT_DIR)/negctl.log && grep -q 'FAIL: block image percentage height' $(PERCENT_HEIGHT_DIR)/negctl.log
test-percentage-height: test-percentage-height-negctl $(PERCENT_HEIGHT_DIR)/test
	@$(PERCENT_HEIGHT_DIR)/test
