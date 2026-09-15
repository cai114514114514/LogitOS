# Actual DOM/CSS/layout with two independently owned passive documents.
PASSIVE_CONTEXT_DIR := $(BUILD)/passive-layout-context
PASSIVE_CONTEXT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/passive_layout_context_test.c
PASSIVE_CONTEXT_DEPS = $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) c/apps/browser/css.h c/apps/browser/layout.h $(BUILD)/libcss_host.a
.PHONY: test-passive-layout-context test-passive-layout-context-negctl test-passive-layout-context-sanitize
$(PASSIVE_CONTEXT_DIR)/test: $(PASSIVE_CONTEXT_DEPS)
	@mkdir -p $(PASSIVE_CONTEXT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(PASSIVE_CONTEXT_DIR)/old-layout: $(PASSIVE_CONTEXT_DEPS)
	@mkdir -p $(PASSIVE_CONTEXT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_CONTEXT_LEGACY_SHARED -o $@ $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(PASSIVE_CONTEXT_DIR)/old-css: $(PASSIVE_CONTEXT_DEPS)
	@mkdir -p $(PASSIVE_CONTEXT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_CONTEXT_LEGACY_SHARED -o $@ $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-passive-layout-context-negctl: $(PASSIVE_CONTEXT_DIR)/old-layout $(PASSIVE_CONTEXT_DIR)/old-css
	@$(PASSIVE_CONTEXT_DIR)/old-layout >$(PASSIVE_CONTEXT_DIR)/old-layout.log 2>&1; rc=$$?; test $$rc -eq 1 && grep -q 'FAIL: parent box table restored after children' $(PASSIVE_CONTEXT_DIR)/old-layout.log && grep -q 'FAIL: same relative src retains document-specific pixels' $(PASSIVE_CONTEXT_DIR)/old-layout.log || { cat $(PASSIVE_CONTEXT_DIR)/old-layout.log; exit 1; }
	@$(PASSIVE_CONTEXT_DIR)/old-css >$(PASSIVE_CONTEXT_DIR)/old-css.log 2>&1; rc=$$?; test $$rc -eq 1 && grep -q 'FAIL: custom-property arena restored' $(PASSIVE_CONTEXT_DIR)/old-css.log && grep -q 'FAIL: parent unchanged stylesheet reuses its own parses' $(PASSIVE_CONTEXT_DIR)/old-css.log || { cat $(PASSIVE_CONTEXT_DIR)/old-css.log; exit 1; }
	@echo 'passive-layout-context: both prior singleton controls fail as expected'
$(PASSIVE_CONTEXT_DIR)/sanitize: $(PASSIVE_CONTEXT_DEPS)
	@mkdir -p $(PASSIVE_CONTEXT_DIR)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $@ $(PASSIVE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-passive-layout-context-sanitize: $(PASSIVE_CONTEXT_DIR)/sanitize
	@$(PASSIVE_CONTEXT_DIR)/sanitize
test-passive-layout-context: test-passive-layout-context-negctl test-passive-layout-context-sanitize $(PASSIVE_CONTEXT_DIR)/test
	@$(PASSIVE_CONTEXT_DIR)/test
