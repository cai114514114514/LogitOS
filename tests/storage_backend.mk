# One real consumer: derive from the WebAPI gate rather than inventing another
# QuickJS/HTTP/Rust source list. backend.c is textually owned by js_webapi.c,
# matching browser_rt/http_cache; list both files as rebuild dependencies.
STORAGE_BACKEND_SRC = $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC)) tests/unit/storage_backend_test.c
STORAGE_BACKEND_DEPS = $(STORAGE_BACKEND_SRC) tests/unit/webapi_test.c c/apps/browser/storage_backend.c c/apps/browser/storage_backend.h c/apps/browser/storage_persistence.inc c/apps/browser/js_webapi.h $(QJS_SRC) $(RUST_LIB_HOST)
STORAGE_BACKEND_FLAGS = -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -DSTORAGE_BACKEND_TEST
.PHONY: test-storage-backend test-storage-backend-negctl
$(BUILD)/storage_backend_test: $(STORAGE_BACKEND_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(STORAGE_BACKEND_FLAGS) -o $@ $(STORAGE_BACKEND_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
test-storage-backend: test-storage-backend-negctl test-storage-platform $(BUILD)/storage_backend_test
	$(BUILD)/storage_backend_test
test-storage-backend-negctl: $(STORAGE_BACKEND_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(STORAGE_BACKEND_FLAGS) -DSTORAGE_NO_SESSION_PARTITION -o $(BUILD)/storage_backend_negctl $(STORAGE_BACKEND_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@$(BUILD)/storage_backend_negctl > $(BUILD)/storage_backend_negctl.log 2>&1; rc=$$?; cat $(BUILD)/storage_backend_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: different tab has independent session storage' $(BUILD)/storage_backend_negctl.log

STORAGE_PLATFORM_SRC = $(filter-out tests/unit/webapi_platform_test.c,$(PLATFORM_TEST_SRC)) tests/unit/storage_platform_test.c
STORAGE_PLATFORM_DEPS = $(STORAGE_PLATFORM_SRC) $(PLATFORM_MOD) tests/unit/webapi_platform_test.c c/apps/browser/storage_backend.c c/apps/browser/storage_backend.h c/apps/browser/storage_persistence.inc c/apps/browser/js_webapi.h $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-storage-platform test-storage-platform-negctl
$(BUILD)/storage_platform_test: $(STORAGE_PLATFORM_DEPS)
	$(CC) -O2 -w $(PLATFORM_CF) -o $@ $(STORAGE_PLATFORM_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-storage-platform: test-storage-platform-negctl $(BUILD)/storage_platform_test
	$(BUILD)/storage_platform_test
test-storage-platform-negctl: $(STORAGE_PLATFORM_DEPS)
	$(CC) -O2 -w $(PLATFORM_CF) -DSTORAGE_QUOTA_WRONG_CLASS -o $(BUILD)/storage_platform_negctl $(STORAGE_PLATFORM_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/storage_platform_negctl > $(BUILD)/storage_platform_negctl.log 2>&1; rc=$$?; cat $(BUILD)/storage_platform_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: storage quota is a real DOMException' $(BUILD)/storage_platform_negctl.log
