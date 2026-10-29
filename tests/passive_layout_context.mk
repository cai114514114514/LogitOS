# Actual DOM/CSS/layout with two independently owned passive documents.
PASSIVE_CONTEXT_DIR := $(BUILD)/passive-layout-context
PASSIVE_CONTEXT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/passive_layout_context_test.c
PASSIVE_CONTEXT_DEPS = $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/block.h $(BUILD)/libcss_host.a
$(PASSIVE_CONTEXT_DIR)/test: $(PASSIVE_CONTEXT_DEPS)
	@mkdir -p $(PASSIVE_CONTEXT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(PASSIVE_CONTEXT_DIR)/old-layout: $(PASSIVE_CONTEXT_DEPS)
	@mkdir -p $(PASSIVE_CONTEXT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_CONTEXT_LEGACY_SHARED -o $@ $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(PASSIVE_CONTEXT_DIR)/old-css: $(PASSIVE_CONTEXT_DEPS)
	@mkdir -p $(PASSIVE_CONTEXT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_CONTEXT_LEGACY_SHARED -o $@ $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(PASSIVE_CONTEXT_DIR)/sanitize: $(PASSIVE_CONTEXT_DEPS)
	@mkdir -p $(PASSIVE_CONTEXT_DIR)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $@ $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
