# The legacy kernel HTTP decoder was embedded in test-browser, so a Darwin
# header conflict stopped the browser battery before it could exercise HTTP.
# Keep a focused gate for the same production translation unit. The network
# stubs never run: these results cover parsing/decoded bytes, not real traffic.
.PHONY: test-http-dechunk test-http-dechunk-negctl

HTTP_DECHUNK_SRC := tests/unit/http_dechunk_test.c c/net/http/url.c
HTTP_DECHUNK_DEPS := $(HTTP_DECHUNK_SRC) c/net/http/http.c c/net/core/net.h \
                     c/net/http/http.h c/net/http/url.h

$(BUILD)/http_dechunk_test: $(HTTP_DECHUNK_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(HOST_INCDIRS) -o $@ $(HTTP_DECHUNK_SRC)

$(BUILD)/http_dechunk_passthrough: $(HTTP_DECHUNK_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w -DHTTP_DECHUNK_PASSTHROUGH $(HOST_INCDIRS) -o $@ $(HTTP_DECHUNK_SRC)

test-http-dechunk-negctl: $(BUILD)/http_dechunk_passthrough
	@$(BUILD)/http_dechunk_passthrough >$(BUILD)/http_dechunk_negctl.log 2>&1; code=$$?; \
	 if [ $$code -eq 0 ] || ! grep -Fq 'FAIL r == 640 && !memcmp(b, expect, 640)' $(BUILD)/http_dechunk_negctl.log; then \
	   cat $(BUILD)/http_dechunk_negctl.log; \
	   echo "http-dechunk-negctl: FAIL -- wire bytes passed as decoded output"; exit 1; \
	 fi; \
	 echo "http-dechunk-negctl: passthrough red as required"

test-http-dechunk: test-http-dechunk-negctl $(BUILD)/http_dechunk_test
	@$(BUILD)/http_dechunk_test

ci-host: test-http-dechunk
