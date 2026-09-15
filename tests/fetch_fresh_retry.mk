FETCH_RETRY_SRC = $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC)) tests/unit/fetch_fresh_retry_test.c
FETCH_RETRY_CF = -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.PHONY: test-fetch-fresh-retry test-fetch-fresh-retry-negctl
test-fetch-fresh-retry-negctl: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) $(FETCH_RETRY_CF) -DWEBAPI_NO_FRESH_RETRY -o $(BUILD)/fetch_retry_old $(FETCH_RETRY_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@$(BUILD)/fetch_retry_old >$(BUILD)/fetch_retry_old.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: unanswered read recovers with one replacement request' $(BUILD)/fetch_retry_old.log
test-fetch-fresh-retry: test-fetch-fresh-retry-negctl
	@$(CC) $(FETCH_RETRY_CF) -o $(BUILD)/fetch_retry $(FETCH_RETRY_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@$(BUILD)/fetch_retry
ci-host: test-fetch-fresh-retry
