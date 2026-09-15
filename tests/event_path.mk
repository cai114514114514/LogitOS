# Derive the installed page surface; do not invent a second list of DOM files.
EVENT_PATH_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/event_path_test.c
EVENT_PATH_DEPS = $(EVENT_PATH_SRC) tests/unit/dom_iface_test.c $(wildcard c/apps/browser/*.h c/apps/browser/*.inc) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-event-path test-event-path-negctl
$(BUILD)/event_path_test: $(EVENT_PATH_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(EVENT_PATH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/event_path_negctl: $(EVENT_PATH_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -DJS_EVENT_PATH_NEGCTL -o $@ $(EVENT_PATH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-event-path-negctl: $(BUILD)/event_path_negctl
	@rc=0; $< > $(BUILD)/event_path_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/event_path_negctl.log; test $$rc -eq 1 && grep -q '^FAIL path frozen across reparent' $(BUILD)/event_path_negctl.log
test-event-path: test-event-path-negctl $(BUILD)/event_path_test
	@$(BUILD)/event_path_test
