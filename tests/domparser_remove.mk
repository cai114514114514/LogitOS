# The parsed-document wrapper is independent of the live-page Element wrapper.
# Removing the method must fail real summary cleanup and detached-node tests.

$(BUILD)/domparser_remove_negctl: $(DOMPARSER_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -DDOMPARSER_NO_REMOVE -o $@ $(DOMPARSER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm


# The existing gate also executes this control; merely listing it in CI would
# leave users of test-domparser with an unwatched mutation path.
test-domparser: test-domparser-remove-negctl

