# Computed dimension inheritance and actual line advance, with the real engine.
LHEIGHT_DIR := $(BUILD)/line-height
LHEIGHT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/line_height_test.c
LHEIGHT_DEPS = $(LHEIGHT_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/css/style.h $(BUILD)/libcss_host.a
$(LHEIGHT_DIR)/test: $(LHEIGHT_DEPS)
	@mkdir -p $(LHEIGHT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(LHEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(LHEIGHT_DIR)/negctl: $(LHEIGHT_DEPS)
	@mkdir -p $(LHEIGHT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_NEGCTL_LINE_HEIGHT_DIMENSION -o $@ $(LHEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
