# SPDX-License-Identifier: MIT
# Shipping WebAPI source subtraction; this gate only runs the new Streams
# boundary fixture, not the old Readable/Fetch regression or clone fixture.
ifeq ($(strip $(WEBAPI_TEST_SRC)),)
$(error tests/html_streams.mk must be included after WEBAPI_TEST_SRC)
endif
HTML_STREAMS_DIR := $(BUILD)/html-streams
HTML_STREAMS_CHECKS := 79
HTML_STREAMS_SRC := tests/unit/html_streams_test.c \
                    $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC)) $(QJS_SRC)
HTML_STREAMS_CF := -O1 -g -w $(BTEST_INC) $(JS_INC) -Iinclude/abi \
                   -DCONFIG_VERSION='"host"' -DWEBAPI_HOST \
                   -DHTML_STREAMS_CHECKS=$(HTML_STREAMS_CHECKS)
HTML_STREAMS_DEPS := $(HTML_STREAMS_SRC) tests/unit/stream_net.h \
                     $(BROWSER_INTERFACE_DEPS) tests/fixtures/html-priority/streams.js \
                     tests/html_streams.mk $(RUST_LIB_HOST)

$(HTML_STREAMS_DIR)/current: $(HTML_STREAMS_DEPS)
	@mkdir -p $(HTML_STREAMS_DIR)
	@$(CC) $(HTML_STREAMS_CF) -o $@ $(HTML_STREAMS_SRC) $(RUST_LIB_HOST) -lm

.PHONY: test-html-streams test-html-streams-negctl test-html-streams-san
# Direct positive runs must watch the ready producer fail first. A CI-only
# reference would leave this control stranded whenever developers run the gate.
test-html-streams: test-html-streams-negctl $(HTML_STREAMS_DIR)/current
	@$(HTML_STREAMS_DIR)/current > $(HTML_STREAMS_DIR)/current.log 2>&1; rc=$$?; \
	 grep -E '^(FAIL|HARNESS|html-streams:)' $(HTML_STREAMS_DIR)/current.log; exit $$rc

$(HTML_STREAMS_DIR)/ready-early: $(HTML_STREAMS_DEPS)
	@mkdir -p $(HTML_STREAMS_DIR)
	@$(CC) $(HTML_STREAMS_CF) -DJS_STREAMS_READY_EARLY -o $@ $(HTML_STREAMS_SRC) $(RUST_LIB_HOST) -lm

test-html-streams-negctl: $(HTML_STREAMS_DIR)/ready-early
	@rc=0; $(HTML_STREAMS_DIR)/ready-early > $(HTML_STREAMS_DIR)/ready-early.log 2>&1 || rc=$$?; \
	 count=$$(grep -c '^FAIL:' $(HTML_STREAMS_DIR)/ready-early.log); \
	 if [ $$rc -eq 1 ] && [ $$count -eq 1 ] && \
	    grep -qx 'html-streams: $(HTML_STREAMS_CHECKS) checks, 1 failures' $(HTML_STREAMS_DIR)/ready-early.log && \
	    grep -qx 'FAIL: Writable ready remains pending while queue reaches highWaterMark' $(HTML_STREAMS_DIR)/ready-early.log; then \
	   echo 'html-streams negative control: all $(HTML_STREAMS_CHECKS) checks ran; ready fulfilled early under real queue pressure'; \
	   grep '^FAIL:' $(HTML_STREAMS_DIR)/ready-early.log; \
	 else cat $(HTML_STREAMS_DIR)/ready-early.log; echo 'html-streams ready control failed at an unexpected boundary'; exit 1; fi

$(HTML_STREAMS_DIR)/san: $(HTML_STREAMS_DEPS)
	@mkdir -p $(HTML_STREAMS_DIR)
	@$(CC) $(HTML_STREAMS_CF) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ $(HTML_STREAMS_SRC) $(RUST_LIB_HOST) -lm

test-html-streams-san: test-html-streams $(HTML_STREAMS_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	 $(HTML_STREAMS_DIR)/san > $(HTML_STREAMS_DIR)/san.log 2>&1; rc=$$?; \
	 grep -E '^(FAIL|HARNESS|html-streams:)|ERROR: AddressSanitizer|runtime error' $(HTML_STREAMS_DIR)/san.log; exit $$rc
