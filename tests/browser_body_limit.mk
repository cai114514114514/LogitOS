BODY_LIMIT_SRC = $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC)) tests/unit/browser_body_limit_test.c
BODY_LIMIT_CF = -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.PHONY: test-browser-body-limit test-browser-body-limit-negctl
test-browser-body-limit-negctl: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) $(BODY_LIMIT_CF) -DBROWSER_BUFFERED_BODY_MAX=8388608 -o $(BUILD)/body_limit_old $(BODY_LIMIT_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm -lz
	@$(BUILD)/body_limit_old >$(BUILD)/body_limit_old.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: compressed body beyond 8 MiB reaches fetch arrayBuffer intact' $(BUILD)/body_limit_old.log
test-browser-body-limit: test-browser-body-limit-negctl
	@$(CC) $(BODY_LIMIT_CF) -o $(BUILD)/body_limit $(BODY_LIMIT_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm -lz
	@$(BUILD)/body_limit
ci-host: test-browser-body-limit
