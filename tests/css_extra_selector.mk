# Exact selectors for properties captured outside LibCSS. The negative control
# restores the original last-compound/pseudo-stripping implementation, and must
# fail the named ancestor and pseudo-target assertions with ordinary exit 1.
XSELECT_DIR := $(BUILD)/css-extra-selector
XSELECT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/css_extra_selector_test.c
XSELECT_DEPS = $(XSELECT_SRC) $(HTML_PARSER_SRC) c/apps/browser/css.h $(BUILD)/libcss_host.a
.PHONY: test-css-extra-selector test-css-extra-selector-negctl
$(XSELECT_DIR)/test: $(XSELECT_DEPS)
	@mkdir -p $(XSELECT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(XSELECT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(XSELECT_DIR)/negctl: $(XSELECT_DEPS)
	@mkdir -p $(XSELECT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_NEGCTL_APPROX_SELECTOR -o $@ $(XSELECT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-css-extra-selector-negctl: $(XSELECT_DIR)/negctl
	@set +e; $(XSELECT_DIR)/negctl > $(XSELECT_DIR)/negctl.log 2>&1; rc=$$?; set -e; cat $(XSELECT_DIR)/negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: ancestor attribute must match' $(XSELECT_DIR)/negctl.log && \
	 grep -q 'FAIL: pseudo element never styles origin' $(XSELECT_DIR)/negctl.log || { echo 'selector negative control did not fail as expected'; exit 1; }
test-css-extra-selector: test-css-extra-selector-negctl $(XSELECT_DIR)/test
	@$(XSELECT_DIR)/test
