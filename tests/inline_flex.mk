# Preserve flex's inside layout while keeping inline-flex atomic in its parent's line.
IFLEX_DIR := $(BUILD)/inline-flex
IFLEX_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/inline_flex_test.c
IFLEX_DEPS = $(IFLEX_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/flex.h $(BUILD)/libcss_host.a
$(IFLEX_DIR)/test: $(IFLEX_DEPS)
	@mkdir -p $(IFLEX_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(IFLEX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(IFLEX_DIR)/negctl: $(IFLEX_DEPS)
	@mkdir -p $(IFLEX_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_NEGCTL_INLINE_FLEX_BLOCK -o $@ $(IFLEX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
