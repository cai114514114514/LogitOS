WEB_DIGEST_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) c/apps/browser/js_subtle.c $(BROWSER_DIGEST_SRC) tests/unit/web_digest_test.c
WEB_DIGEST_DEPS = $(WEB_DIGEST_SRC) tests/unit/dom_iface_test.c c/apps/browser/js_digest.inc tests/fixtures/engine-expansion/digest-vectors.js $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-web-digest test-web-digest-negctl
$(BUILD)/web_digest_test: $(WEB_DIGEST_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -Ic/crypto -o $@ $(WEB_DIGEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/web_digest_negctl: $(WEB_DIGEST_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -Ic/crypto -DJS_DIGEST_NEGCTL -o $@ $(WEB_DIGEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-web-digest-negctl: $(BUILD)/web_digest_negctl
	@rc=0; $< > $(BUILD)/web_digest_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/web_digest_negctl.log; test $$rc -eq 1 && grep -q '^FAIL real SHA digest bytes match independent vectors' $(BUILD)/web_digest_negctl.log
test-web-digest: test-web-digest-negctl $(BUILD)/web_digest_test
	@$(BUILD)/web_digest_test
