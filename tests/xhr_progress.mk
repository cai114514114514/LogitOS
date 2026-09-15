XHR_PROGRESS_DIR = $(BUILD)/xhr-progress
XHR_PROGRESS_SRC = tests/unit/xhr_progress_test.c $(STREAM_TEST_SRC) $(QJS_SRC)
XHR_PROGRESS_DEP = $(XHR_PROGRESS_SRC) tests/unit/stream_net.h $(wildcard c/apps/browser/js_*prelude.inc) c/apps/browser/cookie_persistence.c c/apps/browser/cookie_persistence.h tests/xhr_progress.mk $(RUST_LIB_HOST)
XHR_PROGRESS_CF = -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
$(XHR_PROGRESS_DIR)/current: $(XHR_PROGRESS_DEP)
	@mkdir -p $(XHR_PROGRESS_DIR)
	$(CC) $(XHR_PROGRESS_CF) -o $@ $(XHR_PROGRESS_SRC) $(RUST_LIB_HOST) -lm
$(XHR_PROGRESS_DIR)/old-callback: $(XHR_PROGRESS_DEP)
	@mkdir -p $(XHR_PROGRESS_DIR)
	$(CC) $(XHR_PROGRESS_CF) -DXHR_CALLBACKS_PROPAGATE -o $@ $(XHR_PROGRESS_SRC) $(RUST_LIB_HOST) -lm
$(XHR_PROGRESS_DIR)/old-final: $(XHR_PROGRESS_DEP)
	@mkdir -p $(XHR_PROGRESS_DIR)
	$(CC) $(XHR_PROGRESS_CF) -DXHR_NO_FINAL_PROGRESS -o $@ $(XHR_PROGRESS_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-xhr-progress test-xhr-progress-negctl test-xhr-progress-san
test-xhr-progress-negctl: $(XHR_PROGRESS_DIR)/old-callback $(XHR_PROGRESS_DIR)/old-final
	@rc=0; $(XHR_PROGRESS_DIR)/old-callback > $(XHR_PROGRESS_DIR)/old-callback.log 2>&1 || rc=$$?; cat $(XHR_PROGRESS_DIR)/old-callback.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(XHR_PROGRESS_DIR)/old-callback.log)" -eq 6 && grep -q '^FAIL: header callback error cannot become network failure' $(XHR_PROGRESS_DIR)/old-callback.log && grep -q '^FAIL: progress callback errors cannot strand request completion' $(XHR_PROGRESS_DIR)/old-callback.log && grep -q '^xhr-progress: 21 checks, 6 failures$$' $(XHR_PROGRESS_DIR)/old-callback.log
	@rc=0; $(XHR_PROGRESS_DIR)/old-final > $(XHR_PROGRESS_DIR)/old-final.log 2>&1 || rc=$$?; cat $(XHR_PROGRESS_DIR)/old-final.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(XHR_PROGRESS_DIR)/old-final.log)" -eq 3 && grep -q '^FAIL: terminal progress exposes decoder final character' $(XHR_PROGRESS_DIR)/old-final.log && grep -q '^FAIL: empty response has terminal progress before load and loadend' $(XHR_PROGRESS_DIR)/old-final.log && grep -q '^FAIL: abort in terminal progress prevents load completion' $(XHR_PROGRESS_DIR)/old-final.log && grep -q '^xhr-progress: 21 checks, 3 failures$$' $(XHR_PROGRESS_DIR)/old-final.log
test-xhr-progress: test-xhr-progress-negctl $(XHR_PROGRESS_DIR)/current
	@$(XHR_PROGRESS_DIR)/current
$(XHR_PROGRESS_DIR)/san: $(XHR_PROGRESS_DEP)
	@mkdir -p $(XHR_PROGRESS_DIR)
	$(CC) $(XHR_PROGRESS_CF) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(XHR_PROGRESS_SRC) $(RUST_LIB_HOST) -lm
test-xhr-progress-san: test-xhr-progress $(XHR_PROGRESS_DIR)/san
	ASAN_OPTIONS=detect_leaks=0 $(XHR_PROGRESS_DIR)/san
ci-host: test-xhr-progress
include tests/xhr_response_state.mk
