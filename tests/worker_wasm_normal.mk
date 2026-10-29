# Independent normal acceptance. Deliberately has no fault-injection, GC,
# realm-reset reproduction, or broader test-wasm-js/worker suite dependency.
WORKER_WASM_NORMAL_DIR = $(BUILD)/worker-wasm-normal
WORKER_WASM_NORMAL_SRC = tests/unit/worker_wasm_normal_test.c $(filter-out tests/unit/worker_test.c,$(WORKER_TEST_SRC))
WORKER_WASM_NORMAL_DEP = $(WORKER_WASM_NORMAL_SRC) tests/unit/worker_test.c tests/unit/wasm_js_modules.inc $(BROWSER_JS_PRELUDES) c/apps/gui/browser/js/wasm/wasm.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/worker_wasm_normal.mk
$(WORKER_WASM_NORMAL_DIR)/current: $(WORKER_WASM_NORMAL_DEP)
	@mkdir -p $(WORKER_WASM_NORMAL_DIR)
	$(CC) -O1 -g -w $(WORKER_CF) -o $@ $(WORKER_WASM_NORMAL_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_WASM_NORMAL_DIR)/diagnostics: $(WORKER_WASM_NORMAL_DEP)
	@mkdir -p $(WORKER_WASM_NORMAL_DIR)
	$(CC) -O1 -g -w $(WORKER_CF) -DJS_RUNTIME_DIAGNOSTICS -o $@ $(WORKER_WASM_NORMAL_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-worker-wasm-normal
test-worker-wasm-normal: $(WORKER_WASM_NORMAL_DIR)/current
	@$(WORKER_WASM_NORMAL_DIR)/current
ci-host: test-worker-wasm-normal
