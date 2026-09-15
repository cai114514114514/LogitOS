# Operational keys use the exact browser binding and crypto source list. The
# host adapter supplies only the OS entropy and base64 Web API boundaries.
WEB_KEYS_SRC = c/apps/browser/js_subtle.c $(BROWSER_DIGEST_SRC) $(BROWSER_CRYPTO_OPS_SRC) $(QJS_SRC) tests/unit/web_crypto_keys_test.c
WEB_KEYS_DEPS = $(WEB_KEYS_SRC) c/apps/browser/js_crypto_keys.inc c/apps/browser/js_crypto_keys_script.inc c/apps/browser/js_crypto_ops.inc c/apps/browser/js_digest.inc c/apps/browser/web_entropy.h
WEB_KEYS_CF = -O1 -g -w -DWEBAPI_HOST -DCONFIG_VERSION='"host"' -Ithird_party/quickjs -Iinclude -Ic/apps -Ic/crypto -Ic/kernel/cpu
$(BUILD)/web_crypto_keys: $(WEB_KEYS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(WEB_KEYS_CF) $(WEB_KEYS_SRC) -lm -o $@
$(BUILD)/web_crypto_keys_neg: $(WEB_KEYS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(WEB_KEYS_CF) -DJS_CRYPTO_KEYS_NEGCTL $(WEB_KEYS_SRC) -lm -o $@
$(BUILD)/web_crypto_entropy_neg: $(WEB_KEYS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(WEB_KEYS_CF) -DWEB_ENTROPY_NEGCTL $(WEB_KEYS_SRC) -lm -o $@
.PHONY: test-web-crypto-keys test-web-crypto-keys-negctl
test-web-crypto-keys-negctl: $(BUILD)/web_crypto_keys_neg $(BUILD)/web_crypto_entropy_neg
	@rc=0; $(BUILD)/web_crypto_keys_neg > $(BUILD)/web_crypto_keys_neg.log 2>&1 || rc=$$?; cat $(BUILD)/web_crypto_keys_neg.log; test $$rc -eq 1 && grep -q 'KEY_RESULTS.*unexpected DataError: invalid key material' $(BUILD)/web_crypto_keys_neg.log
	@$(BUILD)/web_crypto_entropy_neg
test-web-crypto-keys: test-web-crypto-keys-negctl $(BUILD)/web_crypto_keys
	@$(BUILD)/web_crypto_keys
ci-host: test-web-crypto-keys
