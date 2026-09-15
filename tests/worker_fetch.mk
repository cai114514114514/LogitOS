# Real Worker scheduler + production fetch/HTTP/Cookie/CORS over local memory
# sockets. Only worker STARTUP scripts use loader_fakebfetch; fetch does not.
WORKER_FETCH_DIR = $(BUILD)/worker-fetch
WORKER_FETCH_SRC = tests/unit/worker_fetch_test.c tests/unit/worker_fetch_net.c $(filter-out tests/unit/worker_test.c,$(WORKER_TEST_SRC))
WORKER_FETCH_DEP = $(WORKER_FETCH_SRC) tests/unit/worker_test.c tests/unit/stream_net.h tests/unit/worker_fetch_net.h c/apps/browser/js_webapi.h $(wildcard c/apps/browser/js_*prelude.inc) c/apps/browser/js_worker_promise_diagnostics.inc $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/worker_fetch.mk
WORKER_FETCH_CF = $(WORKER_CF) -DWEBAPI_FETCH_TEST_HOOKS
$(WORKER_FETCH_DIR)/current: $(WORKER_FETCH_DEP)
	@mkdir -p $(WORKER_FETCH_DIR)
	$(CC) -O2 -w $(WORKER_FETCH_CF) -o $@ $(WORKER_FETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_FETCH_DIR)/missing: $(WORKER_FETCH_DEP)
	@mkdir -p $(WORKER_FETCH_DIR)
	$(CC) -O2 -w $(WORKER_FETCH_CF) -DWORKER_NO_FETCH -o $@ $(WORKER_FETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_FETCH_DIR)/foreign-abort: $(WORKER_FETCH_DEP)
	@mkdir -p $(WORKER_FETCH_DIR)
	$(CC) -O2 -w $(WORKER_FETCH_CF) -DWEBAPI_FETCH_NO_OWNER -o $@ $(WORKER_FETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_FETCH_DIR)/unpolled: $(WORKER_FETCH_DEP)
	@mkdir -p $(WORKER_FETCH_DIR)
	$(CC) -O2 -w $(WORKER_FETCH_CF) -DWORKER_FETCH_NO_PUMP -DWFT_LIVENESS_ONLY -o $@ $(WORKER_FETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_FETCH_DIR)/san: $(WORKER_FETCH_DEP)
	@mkdir -p $(WORKER_FETCH_DIR)
	$(CC) -O1 -g -w $(WORKER_FETCH_CF) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(WORKER_FETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-worker-fetch test-worker-fetch-negctl-missing test-worker-fetch-negctl-owner test-worker-fetch-negctl-pump test-worker-fetch-san
test-worker-fetch-negctl-missing: $(WORKER_FETCH_DIR)/missing
	@rc=0; $(WORKER_FETCH_DIR)/missing > $(WORKER_FETCH_DIR)/missing.log 2>&1 || rc=$$?; cat $(WORKER_FETCH_DIR)/missing.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(WORKER_FETCH_DIR)/missing.log)" -eq 1 && grep -q '^FAIL: both dedicated Workers expose fetch$$' $(WORKER_FETCH_DIR)/missing.log
test-worker-fetch-negctl-owner: $(WORKER_FETCH_DIR)/foreign-abort
	@rc=0; $(WORKER_FETCH_DIR)/foreign-abort > $(WORKER_FETCH_DIR)/foreign-abort.log 2>&1 || rc=$$?; cat $(WORKER_FETCH_DIR)/foreign-abort.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(WORKER_FETCH_DIR)/foreign-abort.log)" -eq 2 && grep -q '^FAIL: page owner cannot abort a worker request handle$$' $(WORKER_FETCH_DIR)/foreign-abort.log && grep -q '^FAIL: peer body survives foreign-owner abort attempt$$' $(WORKER_FETCH_DIR)/foreign-abort.log
test-worker-fetch-negctl-pump: $(WORKER_FETCH_DIR)/unpolled
	@rc=0; $(WORKER_FETCH_DIR)/unpolled > $(WORKER_FETCH_DIR)/unpolled.log 2>&1 || rc=$$?; cat $(WORKER_FETCH_DIR)/unpolled.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(WORKER_FETCH_DIR)/unpolled.log)" -eq 3 && grep -q '^FAIL: worker text resolves relative to script URL$$' $(WORKER_FETCH_DIR)/unpolled.log
test-worker-fetch: test-worker-fetch-negctl-missing test-worker-fetch-negctl-owner test-worker-fetch-negctl-pump $(WORKER_FETCH_DIR)/current
	@$(WORKER_FETCH_DIR)/current
test-worker-fetch-san: test-worker-fetch $(WORKER_FETCH_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 $(WORKER_FETCH_DIR)/san
ci-host: test-worker-fetch
