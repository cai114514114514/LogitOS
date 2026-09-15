# SPDX-License-Identifier: MIT
BROWSER_CONTEXT_SRC = tests/unit/browser_context_test.c $(STREAM_TEST_SRC) $(QJS_SRC)
BROWSER_CONTEXT_CF = -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
BROWSER_COOKIE_CONTEXT_SRC = $(filter-out tests/unit/range_test.c,$(RANGE_SRC)) tests/unit/browser_cookie_context_test.c c/net/http/cookies.c
.PHONY: test-browser-context test-browser-context-negctl test-browser-cookie-context test-browser-cookie-context-negctl
test-browser-context-negctl: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/browser-context
	@$(CC) $(BROWSER_CONTEXT_CF) -DWEBAPI_PREFLIGHT_UNPARTITIONED -o $(BUILD)/browser-context/old $(BROWSER_CONTEXT_SRC) $(RUST_LIB_HOST) -lm
	@rc=0; $(BUILD)/browser-context/old > $(BUILD)/browser-context/old.log 2>&1 || rc=$$?; grep '^FAIL:' $(BUILD)/browser-context/old.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(BUILD)/browser-context/old.log)" -eq 2 && grep -q '^FAIL: different creator requires fresh preflight' $(BUILD)/browser-context/old.log && grep -q '^FAIL: different URL requires fresh preflight' $(BUILD)/browser-context/old.log
test-browser-context: test-browser-context-negctl
	@$(CC) $(BROWSER_CONTEXT_CF) -o $(BUILD)/browser-context/current $(BROWSER_CONTEXT_SRC) $(RUST_LIB_HOST) -lm
	@$(BUILD)/browser-context/current
test-browser-cookie-context-negctl:
	@mkdir -p $(BUILD)/browser-context
	@$(CC) -O2 -w $(RANGE_INC) -DBROWSER_COOKIE_BASE_IS_DOCUMENT -o $(BUILD)/browser-context/native-old $(BROWSER_COOKIE_CONTEXT_SRC)
	@rc=0; $(BUILD)/browser-context/native-old > $(BUILD)/browser-context/native-old.log 2>&1 || rc=$$?; grep '^FAIL ' $(BUILD)/browser-context/native-old.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL ' $(BUILD)/browser-context/native-old.log)" -eq 5 && grep -q '^FAIL store_kind==CK_REQ_SAME_SITE' $(BUILD)/browser-context/native-old.log && grep -q '^FAIL store_kind==CK_REQ_CROSS_SITE' $(BUILD)/browser-context/native-old.log
test-browser-cookie-context: test-browser-cookie-context-negctl
	@$(CC) -O2 -w $(RANGE_INC) -o $(BUILD)/browser-context/native $(BROWSER_COOKIE_CONTEXT_SRC)
	@$(BUILD)/browser-context/native
ci-host: test-browser-context test-browser-cookie-context

# The native jar gate above cannot see who called browser.c load(). Drive real
# links/forms/tab restoration through app_main and inspect that seam separately.
.PHONY: test-browser-cookie-navigation
test-browser-cookie-navigation: $(RUST_LIB_HOST) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)/browser-context
	@$(CC) $(RUNTIME_SCROLL_CF) -DNAVIGATION_COOKIE_CONTEXT -o $(BUILD)/browser-context/navigation $(NAVIGATION_BASE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@for mode in link relative-form no-action-form tab-link key-submit key-request-submit key-same-url key-submit-then-location key-location-then-submit; do \
	 rc=0; $(BUILD)/browser-context/navigation $$mode > $(BUILD)/browser-context/navigation-$$mode.log 2>&1 || rc=$$?; \
	 tail -12 $(BUILD)/browser-context/navigation-$$mode.log; test $$rc -eq 0 || exit $$rc; done
test-browser-cookie-context: test-browser-cookie-navigation
