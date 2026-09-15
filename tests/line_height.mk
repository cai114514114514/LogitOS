# Computed dimension inheritance and actual line advance, with the real engine.
LHEIGHT_DIR := $(BUILD)/line-height
LHEIGHT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/line_height_test.c
LHEIGHT_DEPS = $(LHEIGHT_SRC) $(HTML_PARSER_SRC) c/apps/browser/css.h $(BUILD)/libcss_host.a
.PHONY: test-line-height test-line-height-negctl
$(LHEIGHT_DIR)/test: $(LHEIGHT_DEPS)
	@mkdir -p $(LHEIGHT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(LHEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(LHEIGHT_DIR)/negctl: $(LHEIGHT_DEPS)
	@mkdir -p $(LHEIGHT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_NEGCTL_LINE_HEIGHT_DIMENSION -o $@ $(LHEIGHT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-line-height-negctl: $(LHEIGHT_DIR)/negctl
	@set +e; $(LHEIGHT_DIR)/negctl > $(LHEIGHT_DIR)/negctl.log 2>&1; rc=$$?; set -e; cat $(LHEIGHT_DIR)/negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: 130% parent computes expected line height' $(LHEIGHT_DIR)/negctl.log && \
	 grep -q 'FAIL: 1.3em inheritance keeps length or ratio' $(LHEIGHT_DIR)/negctl.log || { echo 'line-height negative control did not fail as expected'; exit 1; }
test-line-height: test-line-height-negctl $(LHEIGHT_DIR)/test
	@$(LHEIGHT_DIR)/test
