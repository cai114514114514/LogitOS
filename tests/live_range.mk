# Same DOM/page/form installers as the real interface and select-state gates.
# Include dependencies matter: timestamp checks must rebuild when an included
# boundary implementation changes, not report yesterday's binary up-to-date.
.PHONY: test-live-range test-live-range-negctl
LIVE_RANGE_SRC = $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) tests/unit/live_range_test.c c/apps/browser/js_characterdata.c
LIVE_RANGE_INC = c/apps/browser/dom_mutation.inc c/apps/browser/dom_text_mutation.inc c/apps/browser/js_live_range_native.inc c/apps/browser/js_live_range_shim.inc
$(BUILD)/live_range_test: $(LIVE_RANGE_SRC) $(LIVE_RANGE_INC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(LIVE_RANGE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/live_range_negctl: $(LIVE_RANGE_SRC) $(LIVE_RANGE_INC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -DDOM_RANGE_NO_ADJUST -o $@ $(LIVE_RANGE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-live-range-negctl: $(BUILD)/live_range_negctl
	@rc=0; $(BUILD)/live_range_negctl > $(BUILD)/live_range_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/live_range_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL native insert shifts boundary' $(BUILD)/live_range_negctl.log && \
	 grep -q '^FAIL native remove relocates descendant' $(BUILD)/live_range_negctl.log && \
	 grep -q '^FAIL UTF16 replacement shifts boundary' $(BUILD)/live_range_negctl.log || \
	 { echo 'test-live-range-negctl: FAIL -- expected boundary divergence absent'; exit 1; }; \
	 echo 'test-live-range-negctl: PASS -- disabled native updates fail semantic assertions'
test-live-range: test-live-range-negctl $(BUILD)/live_range_test
	@$(BUILD)/live_range_test
ci-host: test-live-range
