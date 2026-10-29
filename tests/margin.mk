# Parsed CSS margin units/kinds through real DOM layout. The control restores
# the previous lossy conversion and MUST fail before the positive can run.
MARGIN_DIR := $(BUILD)/margin
MARGIN_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/margin_test.c
MARGIN_DEPS = $(MARGIN_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/grid.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/grid.c $(BUILD)/libcss_host.a
$(MARGIN_DIR)/test: $(MARGIN_DEPS)
	@mkdir -p $(MARGIN_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(MARGIN_DIR)/negctl: $(MARGIN_DEPS)
	@mkdir -p $(MARGIN_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_MARGIN_KIND -o $@ $(MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(MARGIN_DIR)/negctl-position: $(MARGIN_DEPS)
	@mkdir -p $(MARGIN_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_MARGIN_POSITION -o $@ $(MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
