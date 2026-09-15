# Parsed CSS margin units/kinds through real DOM layout. The control restores
# the previous lossy conversion and MUST fail before the positive can run.
MARGIN_DIR := $(BUILD)/margin
MARGIN_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/margin_test.c
MARGIN_DEPS = $(MARGIN_SRC) $(HTML_PARSER_SRC) c/apps/browser/css.h c/apps/browser/layout_grid.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a
.PHONY: test-margin test-margin-negctl
$(MARGIN_DIR)/test: $(MARGIN_DEPS)
	@mkdir -p $(MARGIN_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(MARGIN_DIR)/negctl: $(MARGIN_DEPS)
	@mkdir -p $(MARGIN_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_MARGIN_KIND -o $@ $(MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(MARGIN_DIR)/negctl-position: $(MARGIN_DEPS)
	@mkdir -p $(MARGIN_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_MARGIN_POSITION -o $@ $(MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-margin-negctl: $(MARGIN_DIR)/negctl $(MARGIN_DIR)/negctl-position
	@if $(MARGIN_DIR)/negctl > $(MARGIN_DIR)/negctl.log 2>&1; then echo 'margin: FAILED: old unit/sentinel conversion escaped'; exit 1; else cat $(MARGIN_DIR)/negctl.log; grep -q 'FAIL:' $(MARGIN_DIR)/negctl.log || exit 1; fi
	@if $(MARGIN_DIR)/negctl-position > $(MARGIN_DIR)/negctl-position.log 2>&1; then echo 'margin: FAILED: old negative/auto placement escaped'; exit 1; else cat $(MARGIN_DIR)/negctl-position.log; grep -q 'FAIL:' $(MARGIN_DIR)/negctl-position.log || exit 1; fi
test-margin: test-margin-negctl $(MARGIN_DIR)/test
	@$(MARGIN_DIR)/test
