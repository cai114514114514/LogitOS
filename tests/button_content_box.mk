BUTTON_CONTENT_BOX_DIR = $(BUILD)/button-content-box
BUTTON_CONTENT_BOX_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/button_content_box_test.c
BUTTON_CONTENT_BOX_DEPS = $(BUTTON_CONTENT_BOX_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/forms.h $(wildcard c/apps/browser/layout*.c c/apps/browser/layout*.h c/apps/browser/layout*.inc) $(BUILD)/libcss_host.a
.PHONY: test-button-content-box test-button-content-box-negctl
$(BUTTON_CONTENT_BOX_DIR)/test: $(BUTTON_CONTENT_BOX_DEPS)
	@mkdir -p $(BUTTON_CONTENT_BOX_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(BUTTON_CONTENT_BOX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(BUTTON_CONTENT_BOX_DIR)/legacy: $(BUTTON_CONTENT_BOX_DEPS)
	@mkdir -p $(BUTTON_CONTENT_BOX_DIR)
	@$(CC) -O2 -w -DLAYOUT_BUTTON_INSET_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(BUTTON_CONTENT_BOX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-button-content-box-negctl: $(BUTTON_CONTENT_BOX_DIR)/legacy
	@$(BUTTON_CONTENT_BOX_DIR)/legacy > $(BUTTON_CONTENT_BOX_DIR)/legacy.log 2>&1; rc=$$?; cat $(BUTTON_CONTENT_BOX_DIR)/legacy.log; test $$rc -eq 1 && grep -q 'FAIL: zero-padding button exposes full content width' $(BUTTON_CONTENT_BOX_DIR)/legacy.log
test-button-content-box: test-button-content-box-negctl $(BUTTON_CONTENT_BOX_DIR)/test
	@$(BUTTON_CONTENT_BOX_DIR)/test
ci-host: test-button-content-box
