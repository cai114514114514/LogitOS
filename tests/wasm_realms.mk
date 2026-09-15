# Production binding, two real runtimes; no page or network fixture needed.
WASM_REALMS_DIR = $(BUILD)/wasm-realms
WASM_REALMS_SRC = tests/unit/wasm_realms_test.c c/apps/browser/js_wasm.c
WASM_REALMS_DEP = $(WASM_REALMS_SRC) c/apps/browser/js_wasm.h c/apps/browser/js_wasm_prelude.inc tests/unit/wasm_js_modules.inc $(WASM_EXEC_SRC) $(QJS_SRC)
WASM_REALMS_TEST_FLAGS = -DWASM_REALM_TEST_COUNTS
WASM_REALMS_CF = -O1 -g -w -Ic/apps/browser $(JS_INC) -DCONFIG_VERSION='"host"' $(WASM_REALMS_TEST_FLAGS)
$(WASM_REALMS_DIR)/current: $(WASM_REALMS_DEP)
	@mkdir -p $(WASM_REALMS_DIR)
	$(CC) $(WASM_REALMS_CF) -o $@ $(WASM_REALMS_SRC) $(QJS_SRC) -lm
$(WASM_REALMS_DIR)/old-install: $(WASM_REALMS_DEP)
	@mkdir -p $(WASM_REALMS_DIR)
	$(CC) $(WASM_REALMS_CF) -DWASM_REALM_NEG_INSTALL -o $@ $(WASM_REALMS_SRC) $(QJS_SRC) -lm
$(WASM_REALMS_DIR)/old-reset: $(WASM_REALMS_DEP)
	@mkdir -p $(WASM_REALMS_DIR)
	$(CC) $(WASM_REALMS_CF) -DWASM_REALM_NEG_RESET -o $@ $(WASM_REALMS_SRC) $(QJS_SRC) -lm
$(WASM_REALMS_DIR)/san: $(WASM_REALMS_DEP)
	@mkdir -p $(WASM_REALMS_DIR)
	$(CC) $(WASM_REALMS_CF) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(WASM_REALMS_SRC) $(QJS_SRC) -lm
.PHONY: test-wasm-realms test-wasm-realms-negctl test-wasm-realms-san
test-wasm-realms-negctl: $(WASM_REALMS_DIR)/old-install $(WASM_REALMS_DIR)/old-reset
	@rc=0; $(WASM_REALMS_DIR)/old-install > $(WASM_REALMS_DIR)/old-install.log 2>&1 || rc=$$?; cat $(WASM_REALMS_DIR)/old-install.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL CASE:' $(WASM_REALMS_DIR)/old-install.log)" -eq 2 && grep -q '^FAIL CASE: install-cache exit=1 signal=0$$' $(WASM_REALMS_DIR)/old-install.log && grep -q '^FAIL CASE: install-defined-memory exit=1 signal=0$$' $(WASM_REALMS_DIR)/old-install.log
	@rc=0; $(WASM_REALMS_DIR)/old-reset > $(WASM_REALMS_DIR)/old-reset.log 2>&1 || rc=$$?; cat $(WASM_REALMS_DIR)/old-reset.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL CASE:' $(WASM_REALMS_DIR)/old-reset.log)" -eq 5 && grep -q '^FAIL CASE: reset-peer exit=1 signal=0$$' $(WASM_REALMS_DIR)/old-reset.log && grep -q '^FAIL CASE: reset-reverse exit=1 signal=0$$' $(WASM_REALMS_DIR)/old-reset.log && grep -q '^FAIL CASE: same-runtime-contexts exit=1 signal=0$$' $(WASM_REALMS_DIR)/old-reset.log && grep -q '^FAIL CASE: repeated-realms exit=1 signal=0$$' $(WASM_REALMS_DIR)/old-reset.log && grep -q '^FAIL CASE: imported-exceptions exit=1 signal=0$$' $(WASM_REALMS_DIR)/old-reset.log
test-wasm-realms: test-wasm-realms-negctl $(WASM_REALMS_DIR)/current
	@$(WASM_REALMS_DIR)/current
test-wasm-realms-san: test-wasm-realms $(WASM_REALMS_DIR)/san
	ASAN_OPTIONS=detect_leaks=0 $(WASM_REALMS_DIR)/san
test-wasm-js: test-wasm-realms
ci-host: test-wasm-realms

