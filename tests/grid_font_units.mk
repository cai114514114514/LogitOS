# Keep output private while sharing only the established host CSS archive.
GRID_FONT_UNITS_DIR := $(BUILD)/site-general/layout/grid-font-units
GRID_FONT_UNITS_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/grid_font_units_test.c
GRID_FONT_UNITS_DEPS = $(GRID_FONT_UNITS_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a tests/grid_font_units.mk
.PHONY: test-grid-font-units test-grid-font-units-negctl
$(GRID_FONT_UNITS_DIR)/test: $(GRID_FONT_UNITS_DEPS)
	@mkdir -p $(GRID_FONT_UNITS_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(GRID_FONT_UNITS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(GRID_FONT_UNITS_DIR)/negctl: $(GRID_FONT_UNITS_DEPS)
	@mkdir -p $(GRID_FONT_UNITS_DIR)
	@$(CC) -O2 -w -DLAYOUT_GRID_REM_AS_EM $(BTEST_INC) $(CSS_INC) -o $@ $(GRID_FONT_UNITS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-grid-font-units-negctl: $(GRID_FONT_UNITS_DIR)/negctl
	@$(GRID_FONT_UNITS_DIR)/negctl > $(GRID_FONT_UNITS_DIR)/negctl.log 2>&1; rc=$$?; cat $(GRID_FONT_UNITS_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL: rem track uses actual root font' $(GRID_FONT_UNITS_DIR)/negctl.log
test-grid-font-units: test-grid-font-units-negctl $(GRID_FONT_UNITS_DIR)/test
	@$(GRID_FONT_UNITS_DIR)/test
