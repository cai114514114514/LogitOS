BUTTON_INNER_DIR = $(BUILD)/button-inner-flex
BUTTON_INNER_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) $(IMG_HOST_SRC) tests/unit/button_inner_flex_test.c
BUTTON_INNER_DEPS = $(BUTTON_INNER_SRC) $(HTML_PARSER_SRC) tests/unit/intrinsic_test.c $(wildcard c/apps/browser/css*.h c/apps/browser/css*.inc c/apps/browser/layout*.h c/apps/browser/layout*.c c/apps/browser/layout*.inc c/apps/browser/svg*.inc c/apps/browser/svg*.h) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/button_inner_flex.mk
$(BUTTON_INNER_DIR)/test: $(BUTTON_INNER_DEPS)
	@mkdir -p $(BUTTON_INNER_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(KMM_INC) -o $@ $(sort $(BUTTON_INNER_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUTTON_INNER_DIR)/legacy: $(BUTTON_INNER_DEPS)
	@mkdir -p $(BUTTON_INNER_DIR)
	$(CC) -O2 -w -DLAYOUT_BUTTON_INNER_FLEX_LEGACY $(BTEST_INC) $(CSS_INC) $(KMM_INC) -o $@ $(sort $(BUTTON_INNER_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-button-inner-flex test-button-inner-flex-negctl
test-button-inner-flex-negctl: $(BUTTON_INNER_DIR)/legacy
	@python3 tests/unit/button_inner_flex_check.py $< $(BUTTON_INNER_DIR)/legacy.log legacy
test-button-inner-flex: test-button-inner-flex-negctl $(BUTTON_INNER_DIR)/test
	@python3 tests/unit/button_inner_flex_check.py $(BUTTON_INNER_DIR)/test $(BUTTON_INNER_DIR)/current.log current
ci-host: test-button-inner-flex
