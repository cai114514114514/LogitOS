# The same binding and page script ship to the guest; host results only prove
# bytes/validation. Root's guest workbench proves the shipping path separately.
WEB_CRYPTO_OPS_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) c/apps/browser/js_subtle.c $(BROWSER_DIGEST_SRC) $(BROWSER_CRYPTO_OPS_SRC) tests/unit/web_crypto_ops_test.c
WEB_CRYPTO_OPS_DEPS = $(WEB_CRYPTO_OPS_SRC) tests/unit/dom_iface_test.c c/apps/browser/js_digest.inc c/apps/browser/js_crypto_ops.inc tests/fixtures/engine-expansion/crypto-checks.js $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-web-crypto-ops test-web-crypto-ops-negctl
$(BUILD)/web_crypto_ops_test: $(WEB_CRYPTO_OPS_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -Ic/crypto -Ic/kernel/cpu -o $@ $(WEB_CRYPTO_OPS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/web_crypto_ops_negctl: $(WEB_CRYPTO_OPS_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -Ic/crypto -Ic/kernel/cpu -DJS_CRYPTO_OPS_NEGCTL -DJS_CRYPTO_KDF_NEGCTL -DJS_CRYPTO_GCM_NEGCTL -o $@ $(WEB_CRYPTO_OPS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-web-crypto-ops-negctl: $(BUILD)/web_crypto_ops_negctl
	@rc=0; $< > $(BUILD)/web_crypto_ops_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/web_crypto_ops_negctl.log; test $$rc -eq 1 && grep -q '^FAIL real crypto operations match independent vectors' $(BUILD)/web_crypto_ops_negctl.log
test-web-crypto-ops: test-web-crypto-ops-negctl $(BUILD)/web_crypto_ops_test
	@$(BUILD)/web_crypto_ops_test
