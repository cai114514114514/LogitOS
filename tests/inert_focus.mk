# Derive the complete form/DOM binding link from its existing gate. Including
# the old test with renamed main reuses its host boundary, not its assertions.
INERT_FOCUS_SRC = $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) tests/unit/inert_focus_test.c
INERT_FOCUS_DEPS = $(INERT_FOCUS_SRC) tests/unit/select_state_test.c c/apps/browser/focus.h c/apps/browser/forms.h c/apps/browser/js_dom.h $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-inert-focus test-inert-focus-negctl
$(BUILD)/inert_focus_test: $(INERT_FOCUS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(INERT_FOCUS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-inert-focus: test-inert-focus-negctl $(BUILD)/inert_focus_test
	$(BUILD)/inert_focus_test
test-inert-focus-negctl: $(INERT_FOCUS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -DFOCUS_NO_INERT -o $(BUILD)/inert_focus_negctl $(INERT_FOCUS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/inert_focus_negctl > $(BUILD)/inert_focus_negctl.log 2>&1; rc=$$?; cat $(BUILD)/inert_focus_negctl.log; test $$rc -eq 1 && grep -q '^FAIL program focus cannot enter inert subtree' $(BUILD)/inert_focus_negctl.log && grep -q '^FAIL Tab excludes inert' $(BUILD)/inert_focus_negctl.log
