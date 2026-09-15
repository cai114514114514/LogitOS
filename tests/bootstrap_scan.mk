# Same production JS install set as semantics; only swap the test main.
BOOTSTRAP_SCAN_SRC = tests/unit/bootstrap_scan_test.c $(filter-out tests/unit/semantics_test.c,$(SEM_SRC))
.PHONY: test-bootstrap-scan test-bootstrap-scan-negctl
test-bootstrap-scan: test-bootstrap-scan-negctl $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w $(SEM_CF) -o $(BUILD)/bootstrap_scan $(BOOTSTRAP_SCAN_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/bootstrap_scan
test-bootstrap-scan-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w -DJS_BOOTSTRAP_JS_SCAN $(SEM_CF) -o $(BUILD)/bootstrap_scan_negctl $(BOOTSTRAP_SCAN_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@rc=0; $(BUILD)/bootstrap_scan_negctl >$(BUILD)/bootstrap_scan_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/bootstrap_scan_negctl.log; \
	 test $$rc -eq 1 && grep -q '^FAIL bootstrap-scan: unrelated tree was wrapped' $(BUILD)/bootstrap_scan_negctl.log && \
	 test "$$(grep -c '^FAIL' $(BUILD)/bootstrap_scan_negctl.log)" -eq 1
