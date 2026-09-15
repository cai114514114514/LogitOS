# SPDX-License-Identifier: MIT
MODULE_RETRY_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/module_retry_test.c
MODULE_RETRY_DEPS = tests/module_retry.mk $(MODULE_RETRY_SRC) tests/unit/module_budget_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
MODULE_RETRY_DIR = $(BUILD)/module-retry
$(MODULE_RETRY_DIR)/current: $(MODULE_RETRY_DEPS)
	@mkdir -p $(MODULE_RETRY_DIR)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(MODULE_RETRY_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(MODULE_RETRY_DIR)/forget-error: $(MODULE_RETRY_DEPS)
	@mkdir -p $(MODULE_RETRY_DIR)
	$(CC) -O1 -g -fsanitize=address -fno-omit-frame-pointer -w $(DOMIFACE_CF) -DJS_MODULE_FORGET_RESOLUTION_ERROR -o $@ $(MODULE_RETRY_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(MODULE_RETRY_DIR)/eager: $(MODULE_RETRY_DEPS)
	@mkdir -p $(MODULE_RETRY_DIR)
	$(CC) -O1 -g -fsanitize=address -fno-omit-frame-pointer -w $(DOMIFACE_CF) -DJS_MODULE_NO_PREFETCH -o $@ $(MODULE_RETRY_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-module-retry test-module-retry-negctl
test-module-retry-negctl: $(MODULE_RETRY_DIR)/forget-error
	@rc=0; ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=86 $(MODULE_RETRY_DIR)/forget-error > $(MODULE_RETRY_DIR)/forget-error.log 2>&1 || rc=$$?; \
	 grep -E 'MODULE-RETRY|AddressSanitizer:|js_create_module_function' $(MODULE_RETRY_DIR)/forget-error.log; \
	 test $$rc -eq 86 && grep -q '^PASS first transitive missing dependency rejected$$' $(MODULE_RETRY_DIR)/forget-error.log && \
	 grep -q 'AddressSanitizer: SEGV' $(MODULE_RETRY_DIR)/forget-error.log && \
	 grep -q 'js_create_module_function' $(MODULE_RETRY_DIR)/forget-error.log
.PHONY: test-module-retry-asan
test-module-retry-asan: $(MODULE_RETRY_DEPS)
	@mkdir -p $(MODULE_RETRY_DIR)
	$(CC) -O1 -g -fsanitize=address -fno-omit-frame-pointer -w $(DOMIFACE_CF) -o $(MODULE_RETRY_DIR)/asan $(MODULE_RETRY_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	ASAN_OPTIONS=detect_leaks=0 $(MODULE_RETRY_DIR)/asan
test-module-retry: test-module-retry-negctl $(MODULE_RETRY_DIR)/current $(MODULE_RETRY_DIR)/eager
	@$(MODULE_RETRY_DIR)/current
	@ASAN_OPTIONS=detect_leaks=0 $(MODULE_RETRY_DIR)/eager
test-module-budget: test-module-retry
ci-host: test-module-retry

$(MODULE_RETRY_DIR)/eager-free: $(MODULE_RETRY_DEPS)
	@mkdir -p $(MODULE_RETRY_DIR)
	$(CC) -O1 -g -fsanitize=address -fno-omit-frame-pointer -w $(DOMIFACE_CF) -DJS_MODULE_NO_PREFETCH -DJS_MODULE_FREE_FAILED_PARSE -o $@ $(MODULE_RETRY_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-module-retry-eager-negctl
test-module-retry-eager-negctl: $(MODULE_RETRY_DIR)/eager-free
	@rc=0; ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=86 $< > $(MODULE_RETRY_DIR)/eager-free.log 2>&1 || rc=$$?; \
	 grep -E 'AddressSanitizer:|js_create_module_function|PASS failed cyclic' $(MODULE_RETRY_DIR)/eager-free.log; \
	 test $$rc -eq 86 && grep -q '^PASS failed cyclic graph rejected$$' $(MODULE_RETRY_DIR)/eager-free.log && \
	 grep -q 'AddressSanitizer: heap-use-after-free' $(MODULE_RETRY_DIR)/eager-free.log && \
	 grep -q 'js_create_module_function' $(MODULE_RETRY_DIR)/eager-free.log
test-module-retry: test-module-retry-eager-negctl
