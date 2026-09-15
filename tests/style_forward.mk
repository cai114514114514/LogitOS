STYLE_FORWARD_DIR = $(BUILD)/style-forward
STYLE_FORWARD_SRC = tests/unit/style_forward_test.c $(filter-out tests/unit/cssom_test.c,$(CSSOM_TEST_SRC))
STYLE_FORWARD_DEPS = $(STYLE_FORWARD_SRC) tests/unit/cssom_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(wildcard c/apps/browser/js_dom*.inc) $(BUILD)/libcss_host.a
STYLE_FORWARD_CF = -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
$(STYLE_FORWARD_DIR)/current: $(STYLE_FORWARD_DEPS)
	@mkdir -p $(STYLE_FORWARD_DIR)
	$(CC) $(STYLE_FORWARD_CF) -o $@ $(STYLE_FORWARD_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(STYLE_FORWARD_DIR)/old: $(STYLE_FORWARD_DEPS)
	@mkdir -p $(STYLE_FORWARD_DIR)
	$(CC) $(STYLE_FORWARD_CF) -DCSSD_FORWARD_LEGACY -DCSSD_INTERFACE_LEGACY -o $@ $(STYLE_FORWARD_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(STYLE_FORWARD_DIR)/san: $(STYLE_FORWARD_DEPS)
	@mkdir -p $(STYLE_FORWARD_DIR)
	$(CC) $(STYLE_FORWARD_CF) -O1 -g -fsanitize=address,undefined -o $@ $(STYLE_FORWARD_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-style-forward test-style-forward-negctl test-style-forward-san
test-style-forward-negctl: $(STYLE_FORWARD_DIR)/old
	@rc=0; $< > $(STYLE_FORWARD_DIR)/old.log 2>&1 || rc=$$?; cat $(STYLE_FORWARD_DIR)/old.log; test $$rc -eq 1 && grep -q 'FAIL STYLE interface exists' $(STYLE_FORWARD_DIR)/old.log && grep -q 'FAIL STYLE assignment updates measured geometry' $(STYLE_FORWARD_DIR)/old.log
test-style-forward: test-style-forward-negctl $(STYLE_FORWARD_DIR)/current
	@$(STYLE_FORWARD_DIR)/current
test-style-forward-san: test-style-forward $(STYLE_FORWARD_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(STYLE_FORWARD_DIR)/san
ci-host: test-style-forward
