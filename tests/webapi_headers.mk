# Same actual fetch consumer and transport fixture as test-webapi. The wrong
# raw-list view is watched failing; merely renaming a stale assertion would
# leave the existing standard behavior without a regression boundary.
# Observed on the actual WEBAPI_TEST_SRC consumer: normal 227/0; raw 227/6.
# Four Headers checks and the two existing XHR serialization consumers fail;
# get() still preserves every value and network Set-Cookie remains hidden.
WEBAPI_HEADERS_DIR = $(BUILD)/site-general/continue/headers
WEBAPI_HEADERS_DEPS = $(WEBAPI_TEST_SRC) $(QJS_SRC) $(RUST_LIB_HOST) $(wildcard c/apps/browser/*.h c/apps/browser/*.inc)
$(WEBAPI_HEADERS_DIR)/current: $(WEBAPI_HEADERS_DEPS)
	@mkdir -p $(WEBAPI_HEADERS_DIR)
	$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -o $@ $(WEBAPI_TEST_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
$(WEBAPI_HEADERS_DIR)/raw: $(WEBAPI_HEADERS_DEPS)
	@mkdir -p $(WEBAPI_HEADERS_DIR)
	$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -DWEBAPI_HEADERS_RAW_ITERATION -o $@ $(WEBAPI_TEST_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-webapi-headers test-webapi-headers-negctl
test-webapi-headers-negctl: $(WEBAPI_HEADERS_DIR)/raw
	@rc=0; $(WEBAPI_HEADERS_DIR)/raw > $(WEBAPI_HEADERS_DIR)/raw.log 2>&1 || rc=$$?; \
	 tail -8 $(WEBAPI_HEADERS_DIR)/raw.log; \
	 test $$rc -eq 1 && \
	 test "$$(grep -c '^FAIL:' $(WEBAPI_HEADERS_DIR)/raw.log)" -eq 6 && \
	 grep -q '^227 checks, 6 failures$$' $(WEBAPI_HEADERS_DIR)/raw.log && \
	 grep -q '^FAIL: repeated response headers iterate once with both wire values' $(WEBAPI_HEADERS_DIR)/raw.log && \
	 grep -q '^FAIL: response forEach sorts names and combines repeated values in wire order' $(WEBAPI_HEADERS_DIR)/raw.log && \
	 grep -q '^FAIL: response entries keys and values share the combined iteration view' $(WEBAPI_HEADERS_DIR)/raw.log && \
	 grep -q '^FAIL: Set-Cookie alone retains repeated iteration entries' $(WEBAPI_HEADERS_DIR)/raw.log && \
	 grep -q '^FAIL: getAllResponseHeaders$$' $(WEBAPI_HEADERS_DIR)/raw.log && \
	 grep -q '^FAIL: setRequestHeader reached the wire$$' $(WEBAPI_HEADERS_DIR)/raw.log && \
	 grep -q '^ok  : all mixed-case repeated wire values survive get' $(WEBAPI_HEADERS_DIR)/raw.log && \
	 grep -q '^ok  : network Set-Cookie remains hidden from every Headers read surface' $(WEBAPI_HEADERS_DIR)/raw.log
test-webapi-headers: test-webapi-headers-negctl $(WEBAPI_HEADERS_DIR)/current
	@$(WEBAPI_HEADERS_DIR)/current > $(WEBAPI_HEADERS_DIR)/current.log 2>&1; rc=$$?; tail -8 $(WEBAPI_HEADERS_DIR)/current.log; exit $$rc
# A control must run when the ordinary existing gate runs as well.
test-webapi: test-webapi-headers-negctl
