STORAGE_PERSIST_SRC = tests/unit/storage_persistence_test.c c/apps/browser/storage_backend.c
STORAGE_PERSIST_DEPS = $(STORAGE_PERSIST_SRC) c/apps/browser/storage_backend.h c/apps/browser/storage_persistence.inc c/apps/browser/tabs.h
.PHONY: test-storage-persistence test-storage-persistence-negctl
$(BUILD)/storage_persistence_test: $(STORAGE_PERSIST_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DSTORAGE_BACKEND_TEST -Ic/apps/browser -o $@ $(STORAGE_PERSIST_SRC)
$(BUILD)/storage_persistence_negctl: $(STORAGE_PERSIST_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DSTORAGE_BACKEND_TEST -DSTORAGE_PERSIST_NO_WRITE -Ic/apps/browser -o $@ $(STORAGE_PERSIST_SRC)
test-storage-persistence-negctl: $(BUILD)/storage_persistence_negctl
	@d=$$(mktemp -d); rc=0; $< write "$$d" > $(BUILD)/storage_persistence_negctl.log 2>&1 && $< read "$$d" >> $(BUILD)/storage_persistence_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/storage_persistence_negctl.log; rm -rf "$$d"; test $$rc -eq 1 && grep -q '^FAIL: new process restores exact local bytes' $(BUILD)/storage_persistence_negctl.log
test-storage-persistence: test-storage-persistence-negctl $(BUILD)/storage_persistence_test
	@$(BUILD)/storage_persistence_test
	@d=$$(mktemp -d); rc=0; $(BUILD)/storage_persistence_test write "$$d" && $(BUILD)/storage_persistence_test read "$$d" || rc=$$?; rm -rf "$$d"; exit $$rc

STORAGE_PERSIST_JS_SRC = $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC)) tests/unit/storage_persistence_js_test.c
STORAGE_PERSIST_JS_DEPS = $(STORAGE_PERSIST_JS_SRC) tests/unit/webapi_test.c c/apps/browser/storage_backend.c c/apps/browser/storage_backend.h c/apps/browser/storage_persistence.inc c/apps/browser/js_webapi.h $(QJS_SRC) $(RUST_LIB_HOST)
.PHONY: test-storage-persistence-js
$(BUILD)/storage_persistence_js_test: $(STORAGE_PERSIST_JS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -o $@ $(STORAGE_PERSIST_JS_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
test-storage-persistence-js: test-storage-persistence-negctl $(BUILD)/storage_persistence_js_test
	@$(BUILD)/storage_persistence_js_test
test-storage-persistence: test-storage-persistence-js

# LogitFS read_file refuses a too-small buffer; a host adapter that truncates
# reads masks the exact bug that first failed a real guest browser restart.
.PHONY: test-storage-persistence-prefix-negctl
$(BUILD)/storage_persistence_prefix_negctl: $(STORAGE_PERSIST_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DSTORAGE_BACKEND_TEST -DSTORAGE_PERSIST_PREFIX_READ -Ic/apps/browser -o $@ $(STORAGE_PERSIST_SRC)
test-storage-persistence-prefix-negctl: $(BUILD)/storage_persistence_prefix_negctl
	@d=$$(mktemp -d); rc=0; $< write "$$d" > $(BUILD)/storage_persistence_prefix_negctl.log 2>&1 && $< read "$$d" >> $(BUILD)/storage_persistence_prefix_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/storage_persistence_prefix_negctl.log; rm -rf "$$d"; test $$rc -eq 1 && grep -q '^FAIL: new process restores exact local bytes' $(BUILD)/storage_persistence_prefix_negctl.log
test-storage-persistence test-storage-persistence-js: test-storage-persistence-prefix-negctl
