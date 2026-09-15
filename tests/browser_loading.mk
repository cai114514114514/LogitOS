# Real app_main, same source-derived loader/CSSOM composition as runtime-scroll.
# The test includes the existing fake transport to control only image readiness.
BROWSER_LOADING_SRC = $(filter-out tests/unit/runtime_scroll_test.c tests/unit/loader_fakebfetch.c,$(RUNTIME_SCROLL_SRC)) tests/unit/browser_loading_test.c
BROWSER_LOADING_DEP = $(BROWSER_LOADING_SRC) tests/unit/loader_fakebfetch.c tests/unit/loader_test.c $(RUNTIME_SCROLL_DEPS)
$(BUILD)/wiring/browser_loading_test: $(BROWSER_LOADING_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(BROWSER_LOADING_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring/browser_loading_negctl: $(BROWSER_LOADING_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_BLOCKING_IMAGES -o $@ $(BROWSER_LOADING_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring/browser_loading_duplicate_negctl: $(BROWSER_LOADING_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_DUPLICATE_IMAGE_IO -o $@ $(BROWSER_LOADING_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-browser-loading-duplicate-negctl
test-browser-loading-duplicate-negctl: $(BUILD)/wiring/browser_loading_duplicate_negctl
	@$< > $(BUILD)/wiring/browser_loading_duplicate_negctl.log 2>&1; rc=$$?; \
	  test $$rc -eq 1 && grep -F 'FAIL: load handler DOM mutation paints and discovers new images without user input' $(BUILD)/wiring/browser_loading_duplicate_negctl.log
.PHONY: test-browser-loading test-browser-loading-negctl
test-browser-loading-negctl: $(BUILD)/wiring/browser_loading_negctl
	@$(BUILD)/wiring/browser_loading_negctl > $(BUILD)/wiring/browser_loading_negctl.log 2>&1; rc=$$?; \
	  test $$rc -eq 1 && grep -F 'FAIL: script bootstrap does not wait for pending images' $(BUILD)/wiring/browser_loading_negctl.log && \
	  grep -F 'FAIL: native mouse click reaches page before image completion' $(BUILD)/wiring/browser_loading_negctl.log
test-browser-loading: test-browser-loading-negctl test-browser-loading-duplicate-negctl $(BUILD)/wiring/browser_loading_test
	$(BUILD)/wiring/browser_loading_test
