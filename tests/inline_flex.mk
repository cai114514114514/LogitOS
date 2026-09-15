# Preserve flex's inside layout while keeping inline-flex atomic in its parent's line.
IFLEX_DIR := $(BUILD)/inline-flex
IFLEX_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/inline_flex_test.c
IFLEX_DEPS = $(IFLEX_SRC) $(HTML_PARSER_SRC) c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_flex.h $(BUILD)/libcss_host.a
.PHONY: test-inline-flex test-inline-flex-negctl
$(IFLEX_DIR)/test: $(IFLEX_DEPS)
	@mkdir -p $(IFLEX_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(IFLEX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(IFLEX_DIR)/negctl: $(IFLEX_DEPS)
	@mkdir -p $(IFLEX_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_NEGCTL_INLINE_FLEX_BLOCK -o $@ $(IFLEX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-inline-flex-negctl: $(IFLEX_DIR)/negctl
	@set +e; $(IFLEX_DIR)/negctl > $(IFLEX_DIR)/negctl.log 2>&1; rc=$$?; set -e; cat $(IFLEX_DIR)/negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: inline flex stays beside preceding text' $(IFLEX_DIR)/negctl.log && \
	 grep -q 'FAIL: auto inline flex shrink wraps children' $(IFLEX_DIR)/negctl.log || { echo 'inline-flex negative control did not fail as expected'; exit 1; }
test-inline-flex: test-inline-flex-negctl $(IFLEX_DIR)/test
	@$(IFLEX_DIR)/test
