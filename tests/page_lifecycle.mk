PAGE_LIFECYCLE_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/page_lifecycle_test.c
PAGE_LIFECYCLE_DEPS = $(PAGE_LIFECYCLE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(BUILD)/page_lifecycle_test: $(PAGE_LIFECYCLE_DEPS)
	$(CC) -O1 -g -w $(DOMIFACE_CF) -o $@ $(PAGE_LIFECYCLE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/page_lifecycle_negctl: $(PAGE_LIFECYCLE_DEPS)
	$(CC) -O1 -g -w $(DOMIFACE_CF) -DJS_PAGE_STALE_WATCHDOG -o $@ $(PAGE_LIFECYCLE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
