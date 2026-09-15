# Exercise the actual app_main, not a second concatenation implementation.
CSS_BUFFER_SRC = $(filter-out tests/unit/dynamic_stylesheets_test.c,$(DYNAMIC_SHEETS_SRC)) tests/unit/css_buffer_test.c
CSS_BUFFER_DEPS = $(CSS_BUFFER_SRC) $(DYNAMIC_SHEETS_DEPS) tests/css_buffer.mk
$(BUILD)/css-buffer/current: $(CSS_BUFFER_DEPS)
	@mkdir -p $(BUILD)/css-buffer
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(CSS_BUFFER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/css-buffer/old: $(CSS_BUFFER_DEPS)
	@mkdir -p $(BUILD)/css-buffer
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_FIXED_CSS_BUFFERS -o $@ $(CSS_BUFFER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/css-buffer/limited: $(CSS_BUFFER_DEPS)
	@mkdir -p $(BUILD)/css-buffer
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_CSS_BYTES=4194304 -DCSS_BUFFER_LIMIT_TEST -o $@ $(CSS_BUFFER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/css-buffer/event-old: $(CSS_BUFFER_DEPS)
	@mkdir -p $(BUILD)/css-buffer
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_CSS_BYTES=4194304 -DCSS_BUFFER_LIMIT_TEST -DBROWSER_OMITTED_SHEET_LOAD -o $@ $(CSS_BUFFER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-css-buffer-event-negctl
test-css-buffer-event-negctl: $(BUILD)/css-buffer/event-old
	@$< > $(BUILD)/css-buffer/event-old.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: omitted dynamic stylesheet reports error instead of load' $(BUILD)/css-buffer/event-old.log
.PHONY: test-css-buffer test-css-buffer-negctl
test-css-buffer-negctl: $(BUILD)/css-buffer/old
	@$< > $(BUILD)/css-buffer/old.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: repeated sheet beyond 4 MiB preserves the last cascade occurrence' $(BUILD)/css-buffer/old.log
test-css-buffer: test-css-buffer-negctl test-css-buffer-event-negctl $(BUILD)/css-buffer/current $(BUILD)/css-buffer/limited
	@$(BUILD)/css-buffer/current > $(BUILD)/css-buffer/current.log 2>&1; rc=$$?; tail -16 $(BUILD)/css-buffer/current.log; exit $$rc
	@$(BUILD)/css-buffer/limited > $(BUILD)/css-buffer/limited.log 2>&1; rc=$$?; tail -16 $(BUILD)/css-buffer/limited.log; exit $$rc
ci-host: test-css-buffer
