# Computed spacing values consumed by the shipping flex/grid and box layout.
SPACING_MATH_DIR = $(BUILD)/css-spacing-math
SPACING_MATH_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/css_spacing_math_test.c
SPACING_MATH_DEPS = $(SPACING_MATH_SRC) $(HTML_PARSER_SRC) tests/unit/intrinsic_test.c $(wildcard c/apps/browser/css*.h c/apps/browser/css*.inc c/apps/browser/layout*.h c/apps/browser/layout*.c c/apps/browser/layout*.inc) $(BUILD)/libcss_host.a tests/css_spacing_math.mk
$(SPACING_MATH_DIR)/test: $(SPACING_MATH_DEPS)
	@mkdir -p $(SPACING_MATH_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(SPACING_MATH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(SPACING_MATH_DIR)/legacy: $(SPACING_MATH_DEPS)
	@mkdir -p $(SPACING_MATH_DIR)
	$(CC) -O2 -w -DCSS_SPACING_MATH_LEGACY -DLAYOUT_PADDING_OWN_WIDTH_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(SPACING_MATH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(SPACING_MATH_DIR)/own-width: $(SPACING_MATH_DEPS)
	@mkdir -p $(SPACING_MATH_DIR)
	$(CC) -O2 -w -DLAYOUT_PADDING_OWN_WIDTH_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(SPACING_MATH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-css-spacing-math test-css-spacing-math-negctl test-css-spacing-math-own-width-negctl
test-css-spacing-math-negctl: $(SPACING_MATH_DIR)/legacy
	@python3 tests/unit/css_spacing_math_check.py $< $(SPACING_MATH_DIR)/legacy.log legacy
test-css-spacing-math-own-width-negctl: $(SPACING_MATH_DIR)/own-width
	@python3 tests/unit/css_spacing_math_check.py $< $(SPACING_MATH_DIR)/own-width.log own-width
test-css-spacing-math: test-css-spacing-math-negctl test-css-spacing-math-own-width-negctl $(SPACING_MATH_DIR)/test
	@python3 tests/unit/css_spacing_math_check.py $(SPACING_MATH_DIR)/test $(SPACING_MATH_DIR)/current.log current
ci-host: test-css-spacing-math
