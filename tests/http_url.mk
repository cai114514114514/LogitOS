# The bounded HTTP transport parser is deliberately smaller than the JS URL
# implementation. Exercise its real bytes and return codes without a browser
# harness that could hide a clipped URL in a second parser.
HTTP_URL_DIR := $(BUILD)/http-url
HTTP_URL_SRC := tests/unit/http_url_test.c c/net/http/url.c
HTTP_URL_DEPS := $(HTTP_URL_SRC) c/net/http/url.h tests/http_url.mk
HTTP_URL_CF := -O1 -g -Wall -Wextra -Werror -Ic/net/http

$(HTTP_URL_DIR)/current: $(HTTP_URL_DEPS)
	@mkdir -p $(HTTP_URL_DIR)
	$(CC) $(HTTP_URL_CF) -o $@ $(HTTP_URL_SRC)
$(HTTP_URL_DIR)/truncated: $(HTTP_URL_DEPS)
	@mkdir -p $(HTTP_URL_DIR)
	$(CC) $(HTTP_URL_CF) -DHTTP_URL_TRUNCATE -o $@ $(HTTP_URL_SRC)

.PHONY: test-http-url test-http-url-negctl test-http-url-san
test-http-url-negctl: $(HTTP_URL_DIR)/truncated
	@rc=0; $(HTTP_URL_DIR)/truncated > $(HTTP_URL_DIR)/truncated.log 2>&1 || rc=$$?; \
	count=$$(grep -c '^FAIL: capacity:' $(HTTP_URL_DIR)/truncated.log); \
	all=$$(grep -c '^FAIL:' $(HTTP_URL_DIR)/truncated.log); \
	if [ $$rc -eq 1 ] && [ $$count -eq 14 ] && [ $$all -eq $$count ] && \
	grep -qx 'FAIL: capacity: relative query exceeding the wire path is refused' $(HTTP_URL_DIR)/truncated.log && \
	grep -qx 'FAIL: capacity: absolute resolution refuses a short output' $(HTTP_URL_DIR)/truncated.log; then \
	  cat $(HTTP_URL_DIR)/truncated.log; \
	  echo 'http-url negative control: clipped URLs falsely succeeded (14 expected failures)'; \
	else cat $(HTTP_URL_DIR)/truncated.log; echo 'http-url negative control failed'; exit 1; fi

# The control is a prerequisite, so running the positive gate always watches
# the old capacity-success illusion go red instead of trusting its name.
test-http-url: test-http-url-negctl $(HTTP_URL_DIR)/current
	@$(HTTP_URL_DIR)/current > $(HTTP_URL_DIR)/current.log 2>&1; rc=$$?; \
	cat $(HTTP_URL_DIR)/current.log; exit $$rc

$(HTTP_URL_DIR)/san: $(HTTP_URL_DEPS)
	@mkdir -p $(HTTP_URL_DIR)
	$(CC) $(HTTP_URL_CF) -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -fno-omit-frame-pointer -o $@ $(HTTP_URL_SRC)
test-http-url-san: test-http-url $(HTTP_URL_DIR)/san
	@$(HTTP_URL_DIR)/san > $(HTTP_URL_DIR)/san.log 2>&1; rc=$$?; \
	cat $(HTTP_URL_DIR)/san.log; exit $$rc
ci-host: test-http-url-san
