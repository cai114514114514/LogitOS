PARSER_SCRIPT_SRC = $(filter-out tests/unit/script_resource_events_test.c,$(SCRIPT_RESOURCE_SRC)) tests/unit/parser_script_events_test.c
PARSER_SCRIPT_DIR = $(BUILD)/site-general/continue/parser-scripts
PARSER_SCRIPT_DEPS = $(PARSER_SCRIPT_SRC) $(RUNTIME_SCROLL_DEPS)
$(PARSER_SCRIPT_DIR)/current: $(PARSER_SCRIPT_DEPS)
	@mkdir -p $(PARSER_SCRIPT_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(PARSER_SCRIPT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(PARSER_SCRIPT_DIR)/old: $(PARSER_SCRIPT_DEPS)
	@mkdir -p $(PARSER_SCRIPT_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_NO_PARSER_SCRIPT_EVENTS -o $@ $(PARSER_SCRIPT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-parser-script-events test-parser-script-events-negctl
test-parser-script-events-negctl: $(PARSER_SCRIPT_DIR)/old
	@rc=0; $< > $(PARSER_SCRIPT_DIR)/old.log 2>&1 || rc=$$?; cat $(PARSER_SCRIPT_DIR)/old.log; \
	 test $$rc -eq 1 && grep -q '^FAIL: parser external success delivers exact nonbubbling load' $(PARSER_SCRIPT_DIR)/old.log && \
	 grep -q '^FAIL: parser load handler starts dynamic continuation' $(PARSER_SCRIPT_DIR)/old.log
test-parser-script-events: test-parser-script-events-negctl $(PARSER_SCRIPT_DIR)/current
	@$(PARSER_SCRIPT_DIR)/current
ci-host: test-parser-script-events
