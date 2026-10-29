# Read/write/native event integration through the existing full CSSOM pipeline.
SCROLL_SYNC_SRC = $(filter-out tests/unit/cssom_test.c,$(CSSOM_TEST_SRC)) tests/unit/scroll_sync_test.c
SCROLL_SYNC_DEPS = $(SCROLL_SYNC_SRC) tests/unit/cssom_test.c $(HTML_PARSER_SRC) $(QJS_SRC) c/apps/gui/browser/js/style/cssom.h c/apps/gui/browser/js/dom/dom.h c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/block.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/grid.c $(BUILD)/libcss_host.a
SCROLL_SYNC_FLAGS = -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
$(BUILD)/scroll_sync_test: $(SCROLL_SYNC_DEPS)
	$(CC) $(SCROLL_SYNC_FLAGS) -o $@ $(SCROLL_SYNC_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
