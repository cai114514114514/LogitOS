# This runs the actual JS bindings and native editing against one set of values.
# The raw-byte control preserves the old JS/native encoding mismatch only.
FORM_SELECTION_UTF16_SRC = $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) tests/unit/form_selection_utf16_test.c
FORM_SELECTION_UTF16_DIR = $(BUILD)/site-general/caret
FORM_SELECTION_UTF16_DEPS = tests/unit/select_state_test.c c/apps/browser/forms.h c/apps/browser/js_forms.c
.PHONY: test-form-selection-utf16 test-form-selection-utf16-negctl
$(FORM_SELECTION_UTF16_DIR)/selection_utf16: $(FORM_SELECTION_UTF16_SRC) $(FORM_SELECTION_UTF16_DEPS) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(FORM_SELECTION_UTF16_DIR)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(FORM_SELECTION_UTF16_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-form-selection-utf16-negctl: $(FORM_SELECTION_UTF16_SRC) $(FORM_SELECTION_UTF16_DEPS) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(FORM_SELECTION_UTF16_DIR)
	@$(CC) -O2 -w $(DOMIFACE_CF) -DFORM_SELECTION_RAW_BYTES -o $(FORM_SELECTION_UTF16_DIR)/selection_raw $(FORM_SELECTION_UTF16_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@rc=0; $(FORM_SELECTION_UTF16_DIR)/selection_raw > $(FORM_SELECTION_UTF16_DIR)/selection_raw.log 2>&1 || rc=$$?; cat $(FORM_SELECTION_UTF16_DIR)/selection_raw.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL CJK getter counts UTF16 units' $(FORM_SELECTION_UTF16_DIR)/selection_raw.log && \
	 grep -q '^FAIL UTF16 setter reaches correct native byte boundaries' $(FORM_SELECTION_UTF16_DIR)/selection_raw.log
test-form-selection-utf16: test-form-selection-utf16-negctl $(FORM_SELECTION_UTF16_DIR)/selection_utf16
	@rc=0; $(FORM_SELECTION_UTF16_DIR)/selection_utf16 > $(FORM_SELECTION_UTF16_DIR)/selection_utf16.log 2>&1 || rc=$$?; cat $(FORM_SELECTION_UTF16_DIR)/selection_utf16.log; exit $$rc
ci-host: test-form-selection-utf16
