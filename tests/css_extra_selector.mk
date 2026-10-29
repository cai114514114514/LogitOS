# Exact selectors for properties captured outside LibCSS. The negative control
# restores the original last-compound/pseudo-stripping implementation, and must
# fail the named ancestor and pseudo-target assertions with ordinary exit 1.
XSELECT_DIR := $(BUILD)/css-extra-selector
XSELECT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/css_extra_selector_test.c
XSELECT_DEPS = $(XSELECT_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/css/style.h $(BUILD)/libcss_host.a
$(XSELECT_DIR)/test: $(XSELECT_DEPS)
	@mkdir -p $(XSELECT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(XSELECT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(XSELECT_DIR)/negctl: $(XSELECT_DEPS)
	@mkdir -p $(XSELECT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_NEGCTL_APPROX_SELECTOR -o $@ $(XSELECT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
