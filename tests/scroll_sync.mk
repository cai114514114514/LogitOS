# Read/write/native event integration through the existing full CSSOM pipeline.
SCROLL_SYNC_SRC = $(filter-out tests/unit/cssom_test.c,$(CSSOM_TEST_SRC)) tests/unit/scroll_sync_test.c
SCROLL_SYNC_DEPS = $(SCROLL_SYNC_SRC) tests/unit/cssom_test.c $(HTML_PARSER_SRC) $(QJS_SRC) c/apps/browser/js_cssom.h c/apps/browser/js_dom.h c/apps/browser/css.h c/apps/browser/layout.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a
SCROLL_SYNC_FLAGS = -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.PHONY: test-scroll-sync test-scroll-sync-negctl
$(BUILD)/scroll_sync_test: $(SCROLL_SYNC_DEPS)
	$(CC) $(SCROLL_SYNC_FLAGS) -o $@ $(SCROLL_SYNC_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
test-scroll-sync: test-scroll-sync-negctl $(BUILD)/scroll_sync_test
	$(BUILD)/scroll_sync_test
test-scroll-sync-negctl: $(SCROLL_SYNC_DEPS)
	$(CC) $(SCROLL_SYNC_FLAGS) -DJS_CSSOM_NO_SCROLL_SYNC -o $(BUILD)/scroll_sync_negctl $(SCROLL_SYNC_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/scroll_sync_negctl > $(BUILD)/scroll_sync_negctl.log 2>&1; rc=$$?; cat $(BUILD)/scroll_sync_negctl.log; test $$rc -eq 1 && grep -q 'FAIL native offset reaches all window getters' $(BUILD)/scroll_sync_negctl.log
	$(CC) $(SCROLL_SYNC_FLAGS) -DJS_DOM_NO_PAGE_SCROLL -o $(BUILD)/scroll_page_negctl $(SCROLL_SYNC_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/scroll_page_negctl > $(BUILD)/scroll_page_negctl.log 2>&1; rc=$$?; cat $(BUILD)/scroll_page_negctl.log; test $$rc -eq 1 && grep -q 'FAIL native event page coordinates include scroll' $(BUILD)/scroll_page_negctl.log
