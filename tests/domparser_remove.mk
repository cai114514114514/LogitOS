# The parsed-document wrapper is independent of the live-page Element wrapper.
# Removing the method must fail real summary cleanup and detached-node tests.
.PHONY: test-domparser-remove test-domparser-remove-negctl

$(BUILD)/domparser_remove_negctl: $(DOMPARSER_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -DDOMPARSER_NO_REMOVE -o $@ $(DOMPARSER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm

test-domparser-remove-negctl: $(BUILD)/domparser_remove_negctl
	@rc=0; $(BUILD)/domparser_remove_negctl > $(BUILD)/domparser_remove_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/domparser_remove_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL DOMParser remove cleans queried elements' $(BUILD)/domparser_remove_negctl.log || \
	 { echo 'test-domparser-remove-negctl: FAIL -- missing method did not fail cleanup'; exit 1; }; \
	 echo 'test-domparser-remove-negctl: PASS -- real summary removal fails without the method'

# The existing gate also executes this control; merely listing it in CI would
# leave users of test-domparser with an unwatched mutation path.
test-domparser: test-domparser-remove-negctl

test-domparser-remove: test-domparser-remove-negctl $(BUILD)/domparser_test
	@$(BUILD)/domparser_test
