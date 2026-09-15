# Finite same-run ink placement; kernel raster/API parity has its own gate.
CONTROL_INK_DIR = $(BUILD)/control-text-ink
CONTROL_INK_SRC = $(filter-out tests/unit/form_content_box_test.c,$(FORM_CONTENT_SRC)) tests/unit/control_text_ink_test.c
CONTROL_INK_DEPS = $(CONTROL_INK_SRC) $(FORM_CONTENT_DEPS) c/apps/browser/control_text_metrics.h include/abi/logit_abi.h tests/control_text_ink.mk
$(CONTROL_INK_DIR)/current: $(CONTROL_INK_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CONTROL_INK_DIR)
	$(CC) -O2 -w -DPAINTHOST_TEXT_METRICS $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(CONTROL_INK_SRC) $(BUILD)/libcss_host.a -lm
$(CONTROL_INK_DIR)/old: $(CONTROL_INK_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CONTROL_INK_DIR)
	$(CC) -O2 -w -DPAINTHOST_TEXT_METRICS -DFC_SINGLE_LINE_INK_LEGACY $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(CONTROL_INK_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-control-text-ink test-control-text-ink-negctl
test-control-text-ink-negctl: $(CONTROL_INK_DIR)/old
	@python3 tests/unit/control_text_ink_check.py $< $(CONTROL_INK_DIR)/old.log old
test-control-text-ink: test-control-text-ink-negctl $(CONTROL_INK_DIR)/current
	@python3 tests/unit/control_text_ink_check.py $(CONTROL_INK_DIR)/current $(CONTROL_INK_DIR)/current.log current
test-form-content-box: test-control-text-ink
