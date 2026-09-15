# Independent normal acceptance. Deliberately has no fault-injection, GC,
# realm-reset reproduction, or broader test-wasm-js/worker suite dependency.
WORKER_WASM_NORMAL_DIR = $(BUILD)/worker-wasm-normal
WORKER_WASM_NORMAL_SRC = tests/unit/worker_wasm_normal_test.c $(filter-out tests/unit/worker_test.c,$(WORKER_TEST_SRC))
WORKER_WASM_NORMAL_DEP = $(WORKER_WASM_NORMAL_SRC) tests/unit/worker_test.c tests/unit/wasm_js_modules.inc $(wildcard c/apps/browser/js_*prelude.inc) c/apps/browser/js_wasm.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/worker_wasm_normal.mk
$(WORKER_WASM_NORMAL_DIR)/current: $(WORKER_WASM_NORMAL_DEP)
	@mkdir -p $(WORKER_WASM_NORMAL_DIR)
	$(CC) -O1 -g -w $(WORKER_CF) -o $@ $(WORKER_WASM_NORMAL_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_WASM_NORMAL_DIR)/diagnostics: $(WORKER_WASM_NORMAL_DEP)
	@mkdir -p $(WORKER_WASM_NORMAL_DIR)
	$(CC) -O1 -g -w $(WORKER_CF) -DJS_RUNTIME_DIAGNOSTICS -o $@ $(WORKER_WASM_NORMAL_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-worker-wasm-normal
test-worker-wasm-normal: $(WORKER_WASM_NORMAL_DIR)/current
	@$(WORKER_WASM_NORMAL_DIR)/current
.PHONY: test-worker-wasm-diagnostics
test-worker-wasm-diagnostics: $(WORKER_WASM_NORMAL_DIR)/current $(WORKER_WASM_NORMAL_DIR)/diagnostics
	@$(WORKER_WASM_NORMAL_DIR)/current > $(WORKER_WASM_NORMAL_DIR)/diagnostics-off.log 2>&1
	@$(WORKER_WASM_NORMAL_DIR)/diagnostics > $(WORKER_WASM_NORMAL_DIR)/diagnostics-on.log 2>&1
	@python3 tests/unit/worker_wasm_diagnostics_check.py $(WORKER_WASM_NORMAL_DIR)/diagnostics-off.log $(WORKER_WASM_NORMAL_DIR)/diagnostics-on.log
ci-host: test-worker-wasm-normal
