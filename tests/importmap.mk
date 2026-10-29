# Real loader source list; the negative build removes only map resolution.
IMPORTMAP_SRC = $(filter-out tests/unit/loader_test.c,$(LOADER_SRC)) tests/unit/importmap_test.c
IMPORTMAP_DEPS = $(IMPORTMAP_SRC) tests/unit/loader_test.c c/apps/gui/browser/js/runtime/importmap.inc c/apps/gui/browser/net/url.inc c/apps/gui/browser/storage/backend.c c/apps/gui/browser/app/page.c $(BROWSER_HEADERS) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
IMPORTMAP_CF = -O2 -w $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
$(BUILD)/importmap_test: $(IMPORTMAP_DEPS)
	$(CC) $(IMPORTMAP_CF) -o $@ $(IMPORTMAP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/importmap_negctl: $(IMPORTMAP_DEPS)
	$(CC) $(IMPORTMAP_CF) -DJS_MODULE_NO_IMPORTMAP -o $@ $(IMPORTMAP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
