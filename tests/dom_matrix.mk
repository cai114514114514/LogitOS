# Actual CSSOM installer, cascade, layout and matrix math. Each missing door is
# independently compiled and must visibly fail before the positive is run.
DOM_MATRIX_OUT = $(BUILD)/site-general/runtime
DOM_MATRIX_SRC = $(filter-out tests/unit/cssom_test.c,$(CSSOM_TEST_SRC)) c/apps/browser/css_interp.c tests/unit/dom_matrix_test.c
DOM_MATRIX_DEPS = $(DOM_MATRIX_SRC) c/apps/browser/js_matrix.inc tests/unit/cssom_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a
DOM_MATRIX_CF = -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.PHONY: test-dom-matrix test-dom-matrix-negctl
$(DOM_MATRIX_OUT)/dom_matrix_test: $(DOM_MATRIX_DEPS)
	@mkdir -p $(DOM_MATRIX_OUT)
	$(CC) $(DOM_MATRIX_CF) -o $@ $(DOM_MATRIX_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(DOM_MATRIX_OUT)/dom_matrix_no_install: $(DOM_MATRIX_DEPS)
	@mkdir -p $(DOM_MATRIX_OUT)
	$(CC) $(DOM_MATRIX_CF) -DDOM_MATRIX_NO_INSTALL -o $@ $(DOM_MATRIX_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(DOM_MATRIX_OUT)/dom_matrix_no_computed: $(DOM_MATRIX_DEPS)
	@mkdir -p $(DOM_MATRIX_OUT)
	$(CC) $(DOM_MATRIX_CF) -DDOM_MATRIX_NO_COMPUTED_TRANSFORM -o $@ $(DOM_MATRIX_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
test-dom-matrix-negctl: $(DOM_MATRIX_OUT)/dom_matrix_no_install $(DOM_MATRIX_OUT)/dom_matrix_no_computed
	@rc=0; $(DOM_MATRIX_OUT)/dom_matrix_no_install > $(DOM_MATRIX_OUT)/matrix-no-install.log 2>&1 || rc=$$?; cat $(DOM_MATRIX_OUT)/matrix-no-install.log; test $$rc -eq 1 && grep -q 'FAIL matrix installer is present' $(DOM_MATRIX_OUT)/matrix-no-install.log
	@rc=0; $(DOM_MATRIX_OUT)/dom_matrix_no_computed > $(DOM_MATRIX_OUT)/matrix-no-computed.log 2>&1 || rc=$$?; cat $(DOM_MATRIX_OUT)/matrix-no-computed.log; test $$rc -eq 1 && grep -q 'FAIL computed transform consumer resolves actual box' $(DOM_MATRIX_OUT)/matrix-no-computed.log && grep -q 'ok   matrix installer is present' $(DOM_MATRIX_OUT)/matrix-no-computed.log
test-dom-matrix: test-dom-matrix-negctl $(DOM_MATRIX_OUT)/dom_matrix_test
	@$(DOM_MATRIX_OUT)/dom_matrix_test
