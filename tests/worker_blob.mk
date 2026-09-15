WORKER_BLOB_DIR = $(BUILD)/worker-blob
WORKER_BLOB_SRC = $(filter-out tests/unit/worker_test.c,$(WORKER_TEST_SRC)) tests/unit/worker_blob_test.c
WORKER_BLOB_DEPS = $(WORKER_BLOB_SRC) tests/unit/worker_test.c $(HTML_PARSER_SRC) c/apps/browser/js_webapi.h c/apps/browser/js_worker.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
WORKER_BLOB_CF = $(WORKER_CF) -DJS_WORKER_BLOB_ALLOC_AUDIT
.PHONY: test-worker-blob test-worker-blob-negctl test-worker-blob-asan
$(WORKER_BLOB_DIR)/current: $(WORKER_BLOB_DEPS)
	@mkdir -p $(WORKER_BLOB_DIR)
	@$(CC) -O2 -w $(WORKER_BLOB_CF) -o $@ $(WORKER_BLOB_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_BLOB_DIR)/old: $(WORKER_BLOB_DEPS)
	@mkdir -p $(WORKER_BLOB_DIR)
	@$(CC) -O2 -w -DJS_WORKER_NO_BLOB $(WORKER_BLOB_CF) -o $@ $(WORKER_BLOB_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-worker-blob-negctl: $(WORKER_BLOB_DIR)/old
	@rc=0; $(WORKER_BLOB_DIR)/old > $(WORKER_BLOB_DIR)/old.log 2>&1 || rc=$$?; cat $(WORKER_BLOB_DIR)/old.log; test $$rc -eq 1 && grep -q 'FAIL: Blob worker starts and echoes' $(WORKER_BLOB_DIR)/old.log
$(WORKER_BLOB_DIR)/oom: $(WORKER_BLOB_DEPS)
	@mkdir -p $(WORKER_BLOB_DIR)
	@$(CC) -O2 -w -DJS_WORKER_TEST_START_OOM $(WORKER_BLOB_CF) -o $@ $(WORKER_BLOB_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-worker-blob: test-worker-blob-negctl $(WORKER_BLOB_DIR)/current $(WORKER_BLOB_DIR)/oom
	@$(WORKER_BLOB_DIR)/current
	@$(WORKER_BLOB_DIR)/oom
test-worker-blob-asan: test-worker-blob
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(WORKER_BLOB_CF) -o $(WORKER_BLOB_DIR)/asan $(WORKER_BLOB_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(WORKER_BLOB_DIR)/asan
ci-host: test-worker-blob
