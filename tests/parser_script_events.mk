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

# Same full loader link as the resource-event gate. The legacy-order control
# must fail the callback and ordering checks, not merely fail to link.
PARSER_ORDER_SRC = $(filter-out tests/unit/parser_script_events_test.c,$(PARSER_SCRIPT_SRC)) tests/unit/parser_script_order_test.c
.SECONDEXPANSION:
$(PARSER_SCRIPT_DIR)/order: $$(PARSER_ORDER_SRC) $$(RUNTIME_SCROLL_DEPS)
	@mkdir -p $(PARSER_SCRIPT_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(PARSER_ORDER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(PARSER_SCRIPT_DIR)/order-old: $$(PARSER_ORDER_SRC) $$(RUNTIME_SCROLL_DEPS)
	@mkdir -p $(PARSER_SCRIPT_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_PARSER_SCRIPT_LEGACY_ORDER -o $@ $(PARSER_ORDER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-parser-script-order test-parser-script-order-negctl
test-parser-script-order-negctl: $(PARSER_SCRIPT_DIR)/order-old
	@rc=0; $< > $(PARSER_SCRIPT_DIR)/order-old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -q '^FAIL: blocking parser scripts precede' $(PARSER_SCRIPT_DIR)/order-old.log && \
	 grep -q '^FAIL: async resource callback sees later parser declaration' $(PARSER_SCRIPT_DIR)/order-old.log
test-parser-script-order: test-parser-script-order-negctl $(PARSER_SCRIPT_DIR)/order
	@$(PARSER_SCRIPT_DIR)/order
ci-host: test-parser-script-order
