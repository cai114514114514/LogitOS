# Real loader source list; the negative build removes only map resolution.
IMPORTMAP_SRC = $(filter-out tests/unit/loader_test.c,$(LOADER_SRC)) tests/unit/importmap_test.c
IMPORTMAP_DEPS = $(IMPORTMAP_SRC) tests/unit/loader_test.c c/apps/browser/js_importmap.inc c/apps/browser/bfetch_url.inc c/apps/browser/storage_backend.c c/apps/browser/page_runtime.c $(wildcard c/apps/browser/*.h) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
IMPORTMAP_CF = -O2 -w $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.PHONY: test-importmap test-importmap-negctl
$(BUILD)/importmap_test: $(IMPORTMAP_DEPS)
	$(CC) $(IMPORTMAP_CF) -o $@ $(IMPORTMAP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/importmap_negctl: $(IMPORTMAP_DEPS)
	$(CC) $(IMPORTMAP_CF) -DJS_MODULE_NO_IMPORTMAP -o $@ $(IMPORTMAP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-importmap-negctl: $(BUILD)/importmap_negctl
	@$(BUILD)/importmap_negctl > $(BUILD)/importmap_negctl.log 2>&1; rc=$$?; test $$rc -eq 1 && grep -q 'FAIL: mapped graph executes through browser loader' $(BUILD)/importmap_negctl.log
	@echo 'importmap negative control: mapped graph and DOM mount fail without resolution'
test-importmap: test-importmap-negctl $(BUILD)/importmap_test
	$(BUILD)/importmap_test
