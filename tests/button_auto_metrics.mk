BUTTON_AUTO_DIR = $(BUILD)/button-auto-metrics
BUTTON_AUTO_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/button_auto_metrics_test.c
BUTTON_AUTO_DEPS = $(BUTTON_AUTO_SRC) $(HTML_PARSER_SRC) tests/unit/intrinsic_test.c $(wildcard c/apps/browser/css*.h c/apps/browser/css*.inc c/apps/browser/layout*.h c/apps/browser/layout*.c c/apps/browser/layout*.inc) $(BUILD)/libcss_host.a tests/button_auto_metrics.mk
$(BUTTON_AUTO_DIR)/test: $(BUTTON_AUTO_DEPS)
	@mkdir -p $(BUTTON_AUTO_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(BUTTON_AUTO_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(BUTTON_AUTO_DIR)/legacy: $(BUTTON_AUTO_DEPS)
	@mkdir -p $(BUTTON_AUTO_DIR)
	$(CC) -O2 -w -DLAYOUT_BUTTON_AUTO_METRICS_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(BUTTON_AUTO_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-button-auto-metrics test-button-auto-metrics-negctl
test-button-auto-metrics-negctl: $(BUTTON_AUTO_DIR)/legacy
	@python3 tests/unit/button_auto_metrics_check.py $< $(BUTTON_AUTO_DIR)/legacy.log legacy
test-button-auto-metrics: test-button-auto-metrics-negctl $(BUTTON_AUTO_DIR)/test
	@python3 tests/unit/button_auto_metrics_check.py $(BUTTON_AUTO_DIR)/test $(BUTTON_AUTO_DIR)/current.log current
ci-host: test-button-auto-metrics

include tests/button_inner_flex.mk
