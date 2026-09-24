# SPDX-License-Identifier: MIT
# tests/fetch_body.mk -- what goes on the wire for every request body type.
#
#   make test-fetch-body
#
# Fetch "extract a body" and XHR send(): string, ArrayBuffer, every
# ArrayBufferView (with a non-zero byteOffset into a larger buffer -- the
# classic bug), Blob/File, FormData (multipart/form-data, byte-exact against
# the HTML encoding algorithm written out by hand in the test), URLSearchParams,
# null/undefined, an explicit Content-Type, GET/HEAD with a body, a
# ReadableStream body, fetch(Request) and XHR's charset rewrite. Host-only:
# the REAL js_webapi.c prelude and __fetchStart over tests/unit/stream_net.h's
# in-memory server, which captures the request byte for byte.
#
# Two negative controls, each watched failing for its own reason and required
# to fail EXACTLY the checks it names (not "some"):
#   -DWEBAPI_FETCH_BODY_AB_ONLY        the ArrayBuffer-only handling the native
#       primitive had: every view, Blob and FormData is stringified. The view,
#       Blob and FormData checks must fail; strings, ArrayBuffer,
#       URLSearchParams, GET/HEAD and the stream refusal must still pass.
#   -DWEBAPI_FETCH_BODY_VIEW_WHOLE_BUFFER   the classic view bug (send the
#       whole underlying buffer): exactly the checks whose view does NOT span
#       its buffer must fail -- whole-buffer views are the ones that hide it.
FETCH_BODY_DIR := $(BUILD)/fetch-body
# A SUBTRACTION from test-webapi's list, not a copy of it. This fragment is
# read right after tests/jsprobe.mk, which is before the Makefile defines
# STREAM_TEST_SRC -- a `:=` over that variable expanded to nothing and the
# first link of this gate died on js_webapi_install. WEBAPI_TEST_SRC is
# defined above the include and is the same transport stack.
ifeq ($(strip $(WEBAPI_TEST_SRC)),)
$(error tests/fetch_body.mk: WEBAPI_TEST_SRC is empty -- include this fragment after the Makefile defines it)
endif
FETCH_BODY_SRC := tests/unit/fetch_body_test.c $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC)) $(QJS_SRC)
FETCH_BODY_DEP := $(FETCH_BODY_SRC) tests/unit/stream_net.h $(BROWSER_JS_PRELUDES) \
                  c/apps/gui/browser/storage/cookies.c tests/fetch_body.mk $(RUST_LIB_HOST)
FETCH_BODY_CF  := -O1 -g -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(FETCH_BODY_DIR)/current: $(FETCH_BODY_DEP)
	@mkdir -p $(FETCH_BODY_DIR)
	@$(CC) $(FETCH_BODY_CF) -o $@ $(FETCH_BODY_SRC) $(RUST_LIB_HOST) -lm
$(FETCH_BODY_DIR)/ab-only: $(FETCH_BODY_DEP)
	@mkdir -p $(FETCH_BODY_DIR)
	@$(CC) $(FETCH_BODY_CF) -DWEBAPI_FETCH_BODY_AB_ONLY -o $@ $(FETCH_BODY_SRC) $(RUST_LIB_HOST) -lm
$(FETCH_BODY_DIR)/whole-buffer: $(FETCH_BODY_DEP)
	@mkdir -p $(FETCH_BODY_DIR)
	@$(CC) $(FETCH_BODY_CF) -DWEBAPI_FETCH_BODY_VIEW_WHOLE_BUFFER -o $@ $(FETCH_BODY_SRC) $(RUST_LIB_HOST) -lm

# The exact FAIL lines each control must produce live in
# tests/unit/fetch_body_{ab_only,whole_buffer}.expect, one per line; the rule
# diffs them against the control's output, so a control that fails MORE (a
# broken harness) or FEWER (a check that stopped measuring) is itself red.
.PHONY: test-fetch-body test-fetch-body-negctl test-fetch-body-negctl-ab-only test-fetch-body-negctl-whole-buffer
test-fetch-body-negctl-ab-only: $(FETCH_BODY_DIR)/ab-only
	@rc=0; $(FETCH_BODY_DIR)/ab-only > $(FETCH_BODY_DIR)/ab-only.log 2>&1 || rc=$$?; \
	 grep '^FAIL' $(FETCH_BODY_DIR)/ab-only.log | sort > $(FETCH_BODY_DIR)/ab-only.fails; \
	 sort tests/unit/fetch_body_ab_only.expect > $(FETCH_BODY_DIR)/ab-only.expect; \
	 if [ $$rc -eq 1 ] && diff -u $(FETCH_BODY_DIR)/ab-only.expect $(FETCH_BODY_DIR)/ab-only.fails; then \
	   echo "fetch-body negative control (ArrayBuffer-only): exactly the $$(wc -l < $(FETCH_BODY_DIR)/ab-only.fails | tr -d ' ') view/Blob/FormData checks fail"; \
	 else cat $(FETCH_BODY_DIR)/ab-only.log; echo "fetch-body AB-only control did not fail exactly the expected checks (rc=$$rc)"; exit 1; fi
test-fetch-body-negctl-whole-buffer: $(FETCH_BODY_DIR)/whole-buffer
	@rc=0; $(FETCH_BODY_DIR)/whole-buffer > $(FETCH_BODY_DIR)/whole-buffer.log 2>&1 || rc=$$?; \
	 grep '^FAIL' $(FETCH_BODY_DIR)/whole-buffer.log | sort > $(FETCH_BODY_DIR)/whole-buffer.fails; \
	 sort tests/unit/fetch_body_whole_buffer.expect > $(FETCH_BODY_DIR)/whole-buffer.expect; \
	 if [ $$rc -eq 1 ] && diff -u $(FETCH_BODY_DIR)/whole-buffer.expect $(FETCH_BODY_DIR)/whole-buffer.fails; then \
	   echo "fetch-body negative control (whole buffer): exactly the $$(wc -l < $(FETCH_BODY_DIR)/whole-buffer.fails | tr -d ' ') offset-view checks fail"; \
	 else cat $(FETCH_BODY_DIR)/whole-buffer.log; echo "fetch-body whole-buffer control did not fail exactly the expected checks (rc=$$rc)"; exit 1; fi
test-fetch-body-negctl: test-fetch-body-negctl-ab-only test-fetch-body-negctl-whole-buffer
test-fetch-body: test-fetch-body-negctl $(FETCH_BODY_DIR)/current
	@$(FETCH_BODY_DIR)/current > $(FETCH_BODY_DIR)/current.log 2>&1; rc=$$?; \
	 grep -E '^(FAIL|fetch-body:)' $(FETCH_BODY_DIR)/current.log; exit $$rc
ci-host: test-fetch-body

# ASan/UBSan over the same checks: js_utf8_encode rewrites bytes in place and
# __fetchStart copies a page-sized buffer. Leak detection is off because
# darwin/arm64 ASan has none (see CLAUDE.md); memory errors still abort.
$(FETCH_BODY_DIR)/san: $(FETCH_BODY_DEP)
	@mkdir -p $(FETCH_BODY_DIR)
	@$(CC) $(FETCH_BODY_CF) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(FETCH_BODY_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-fetch-body-san
test-fetch-body-san: test-fetch-body $(FETCH_BODY_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 $(FETCH_BODY_DIR)/san > $(FETCH_BODY_DIR)/san.log 2>&1; rc=$$?; \
	 grep -E '^(FAIL|fetch-body:)|ERROR: AddressSanitizer|runtime error' $(FETCH_BODY_DIR)/san.log; exit $$rc
ci-host: test-fetch-body-san
