# Ordinary Worker pure globals, sharing the existing production-linked harness.
WORKER_GLOBALS_DIR = $(BUILD)/worker-globals
# This gate measures the independent URL/Encoding installer. Keep fetch absent
# explicitly now that ordinary workers also install their own fetch owner;
# removing noFetch assertions would silently change what its old control tests.
WORKER_GLOBALS_CF = $(WORKER_CF) -DWORKER_NO_FETCH
WORKER_GLOBALS_SRC = tests/unit/worker_globals_test.c $(filter-out tests/unit/worker_test.c,$(WORKER_TEST_SRC))
WORKER_GLOBALS_DEP = $(WORKER_GLOBALS_SRC) tests/unit/worker_test.c $(wildcard c/apps/browser/js_*prelude.inc) c/apps/browser/js_encoding.inc c/apps/browser/js_url.h c/apps/browser/js_webapi.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(WORKER_GLOBALS_DIR)/current: $(WORKER_GLOBALS_DEP)
	@mkdir -p $(WORKER_GLOBALS_DIR)
	$(CC) -O2 -w $(WORKER_GLOBALS_CF) -o $@ $(WORKER_GLOBALS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_GLOBALS_DIR)/old-globals: $(WORKER_GLOBALS_DEP)
	@mkdir -p $(WORKER_GLOBALS_DIR)
	$(CC) -O2 -w $(WORKER_GLOBALS_CF) -DWORKER_NO_PURE_GLOBALS -o $@ $(WORKER_GLOBALS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_GLOBALS_DIR)/san: $(WORKER_GLOBALS_DEP)
	@mkdir -p $(WORKER_GLOBALS_DIR)
	$(CC) -O1 -g -w $(WORKER_GLOBALS_CF) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(WORKER_GLOBALS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-worker-globals test-worker-globals-negctl test-worker-globals-san
test-worker-globals-negctl: $(WORKER_GLOBALS_DIR)/old-globals
	@rc=0; $(WORKER_GLOBALS_DIR)/old-globals > $(WORKER_GLOBALS_DIR)/old-globals.log 2>&1 || rc=$$?; cat $(WORKER_GLOBALS_DIR)/old-globals.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(WORKER_GLOBALS_DIR)/old-globals.log)" -eq 13 && grep -q '^FAIL: workers expose URL$$' $(WORKER_GLOBALS_DIR)/old-globals.log && grep -q '^FAIL: workers expose TextDecoder$$' $(WORKER_GLOBALS_DIR)/old-globals.log
test-worker-globals: test-worker-globals-negctl $(WORKER_GLOBALS_DIR)/current
	@$(WORKER_GLOBALS_DIR)/current
test-worker-globals-san: test-worker-globals $(WORKER_GLOBALS_DIR)/san
	ASAN_OPTIONS=detect_leaks=0 $(WORKER_GLOBALS_DIR)/san
test-worker: test-worker-globals
ci-host: test-worker-globals
