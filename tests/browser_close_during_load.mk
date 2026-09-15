# Browser close must remain actionable after the top-level response and during
# a running page script. The source list is derived from LOADER_SRC so this gate
# cannot silently omit a browser translation unit the established real-loader
# harness already needs.
BROWSER_CLOSE_LOAD_SRC := $(filter-out tests/unit/loader_test.c tests/unit/loader_fakebfetch.c,$(LOADER_SRC)) \
                          tests/unit/browser_close_during_load_test.c
BROWSER_CLOSE_LOAD_DEP := $(BROWSER_CLOSE_LOAD_SRC) tests/unit/loader_test.c \
                          tests/unit/loader_fakebfetch.c tests/unit/loaderhost/logit.h \
                          c/apps/browser/js_page.h
BROWSER_CLOSE_LOAD_CF := -O2 -w $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) \
                         -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
                         -DLOADER_CLOCK_HOOK -DLOADER_POLL_HOOK

$(BUILD)/wiring/browser_close_load: $(BROWSER_CLOSE_LOAD_DEP) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/wiring
	$(CC) $(BROWSER_CLOSE_LOAD_CF) -o $@ $(BROWSER_CLOSE_LOAD_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

$(BUILD)/wiring/browser_close_load_old: $(BROWSER_CLOSE_LOAD_DEP) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/wiring
	$(CC) $(BROWSER_CLOSE_LOAD_CF) -DBROWSER_LOAD_CLOSE_LEGACY -o $@ \
	    $(BROWSER_CLOSE_LOAD_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

.PHONY: test-browser-close-load-negctl test-browser-close-load test-browser-close-load-sanitize
test-browser-close-load-negctl: $(BUILD)/wiring/browser_close_load_old
	@rc=0; $< stage > $(BUILD)/wiring/browser_close_load_old-stage.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: close queued after document fetch stops CPU-side loading' $(BUILD)/wiring/browser_close_load_old-stage.log
	@rc=0; $< script > $(BUILD)/wiring/browser_close_load_old-script.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: close during active page script exits the browser' $(BUILD)/wiring/browser_close_load_old-script.log && \
	 grep -F 'FAIL: close interrupts the active page script before the watchdog' $(BUILD)/wiring/browser_close_load_old-script.log
	@$< resource > $(BUILD)/wiring/browser_close_load_old-resource.log
	@rc=0; $< wheel > $(BUILD)/wiring/browser_close_load_old-wheel.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: wheel queued during loading reaches the page scroll path' $(BUILD)/wiring/browser_close_load_old-wheel.log
	@rc=0; $< wheel-cancel > $(BUILD)/wiring/browser_close_load_old-wheel-cancel.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: deferred loading-time wheel reaches its inline handler' $(BUILD)/wiring/browser_close_load_old-wheel-cancel.log
	@rc=0; $< wheel-script > $(BUILD)/wiring/browser_close_load_old-wheel-script.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: wheel queued during loading reaches the page scroll path' $(BUILD)/wiring/browser_close_load_old-wheel-script.log
	@echo 'browser-close-load control: PASS -- old loader misses CPU/script close and cannot paint a loading-time wheel before CSS/script completes; existing resource close passes'

test-browser-close-load: test-browser-close-load-negctl $(BUILD)/wiring/browser_close_load
	$(BUILD)/wiring/browser_close_load stage
	$(BUILD)/wiring/browser_close_load script
	$(BUILD)/wiring/browser_close_load resource
	$(BUILD)/wiring/browser_close_load wheel
	$(BUILD)/wiring/browser_close_load wheel-cancel
	$(BUILD)/wiring/browser_close_load wheel-script
	$(BUILD)/wiring/browser_close_load module

test-browser-close-load-sanitize: test-browser-close-load
	$(CC) $(BROWSER_CLOSE_LOAD_CF) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -o $(BUILD)/wiring/browser_close_load_sanitize $(BROWSER_CLOSE_LOAD_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/browser_close_load_sanitize stage
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/browser_close_load_sanitize script
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/browser_close_load_sanitize resource
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/browser_close_load_sanitize wheel
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/browser_close_load_sanitize wheel-cancel
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/browser_close_load_sanitize wheel-script
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/browser_close_load_sanitize module

ci-host: test-browser-close-load
