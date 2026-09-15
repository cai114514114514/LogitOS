FLEX_USED_HEIGHT_DIR = $(BUILD)/flex-used-height
FLEX_USED_HEIGHT_SRC = $(filter-out tests/unit/modal_paint_test.c,$(MODAL_PAINT_SRC)) tests/unit/flex_used_height_test.c
FLEX_USED_HEIGHT_DEPS = $(FLEX_USED_HEIGHT_SRC) tests/unit/modal_paint_test.c c/apps/browser/forms.h $(wildcard c/apps/browser/layout*.h c/apps/browser/layout*.c c/apps/browser/layout*.inc) $(BUILD)/libcss_host.a
.PHONY: test-flex-used-height test-flex-used-height-negctl
$(FLEX_USED_HEIGHT_DIR)/test: $(FLEX_USED_HEIGHT_DEPS)
	@mkdir -p $(FLEX_USED_HEIGHT_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(FLEX_USED_HEIGHT_SRC) $(BUILD)/libcss_host.a -lm
$(FLEX_USED_HEIGHT_DIR)/legacy: $(FLEX_USED_HEIGHT_DEPS)
	@mkdir -p $(FLEX_USED_HEIGHT_DIR)
	@$(CC) -O2 -w -DLAYOUT_FLEX_ROW_STRETCH_LEGACY $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(FLEX_USED_HEIGHT_SRC) $(BUILD)/libcss_host.a -lm
test-flex-used-height-negctl: $(FLEX_USED_HEIGHT_DIR)/legacy
	@$(FLEX_USED_HEIGHT_DIR)/legacy > $(FLEX_USED_HEIGHT_DIR)/legacy.log 2>&1; rc=$$?; cat $(FLEX_USED_HEIGHT_DIR)/legacy.log; test $$rc -eq 1 && grep -q 'FAIL: stretched clipped welcome is actually painted' $(FLEX_USED_HEIGHT_DIR)/legacy.log && grep -q 'FAIL: sidebar scrollport receives remaining height' $(FLEX_USED_HEIGHT_DIR)/legacy.log
test-flex-used-height: test-flex-used-height-negctl $(FLEX_USED_HEIGHT_DIR)/test
	@$(FLEX_USED_HEIGHT_DIR)/test
ci-host: test-flex-used-height
