# SPDX-License-Identifier: MIT
# Same WebAPI/Blob/File source group as the browser's existing host tests.
# Native URL is added from BROWSER_JS_SRC so host rejection checks measure the
# shipped native classes rather than the old WebAPI fallback JS constructors.
ifeq ($(strip $(WEBAPI_TEST_SRC)),)
$(error tests/html_clone.mk must be included after WEBAPI_TEST_SRC)
endif
HTML_CLONE_DIR := $(BUILD)/html-clone
HTML_CLONE_SRC := tests/unit/html_clone_test.c \
                  $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC)) \
                  $(filter c/apps/gui/browser/js/net/url.c,$(BROWSER_JS_SRC)) $(QJS_SRC)
HTML_CLONE_CF := -O1 -g -w $(BTEST_INC) $(JS_INC) -Iinclude/abi \
                 -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
HTML_CLONE_DEPS := $(HTML_CLONE_SRC) tests/unit/stream_net.h \
                   $(BROWSER_INTERFACE_DEPS) tests/fixtures/html-priority/clone.js \
                   tests/html_clone.mk $(RUST_LIB_HOST)

$(HTML_CLONE_DIR)/current: $(HTML_CLONE_DEPS)
	@mkdir -p $(HTML_CLONE_DIR)
	@$(CC) $(HTML_CLONE_CF) -o $@ $(HTML_CLONE_SRC) $(RUST_LIB_HOST) -lm

$(HTML_CLONE_DIR)/keep-buffer: $(HTML_CLONE_DEPS)
	@mkdir -p $(HTML_CLONE_DIR)
	@$(CC) $(HTML_CLONE_CF) -DJS_CLONE_KEEP_BUFFER -o $@ $(HTML_CLONE_SRC) $(RUST_LIB_HOST) -lm

.PHONY: test-html-clone test-html-clone-negctl test-html-clone-san
# The negative control is a prerequisite: putting it only on a CI aggregate
# would let every direct positive run skip the boundary it purports to guard.
test-html-clone: test-html-clone-negctl $(HTML_CLONE_DIR)/current
	@$(HTML_CLONE_DIR)/current > $(HTML_CLONE_DIR)/current.log 2>&1; rc=$$?; \
	 grep -E '^(FAIL|HARNESS|html-clone:)' $(HTML_CLONE_DIR)/current.log; exit $$rc

test-html-clone-negctl: $(HTML_CLONE_DIR)/keep-buffer
	@rc=0; $(HTML_CLONE_DIR)/keep-buffer > $(HTML_CLONE_DIR)/keep-buffer.log 2>&1 || rc=$$?; \
	 grep '^FAIL:' $(HTML_CLONE_DIR)/keep-buffer.log | sort > $(HTML_CLONE_DIR)/keep-buffer.fails; \
	 printf '%s\n' \
	  'FAIL: transfer detaches source ArrayBuffer' \
	  'FAIL: transfer invalidates old typed array' \
	  'FAIL: detached ArrayBuffer cannot be cloned or transferred again' \
	  'FAIL: detached transfer validation follows graph getter serialization' \
	  'FAIL: transfer captures bytes after graph getters finish' \
	  'FAIL: commit revalidates all buffers after reentrant detachment' \
	  'FAIL: transfer detaches listed buffer even when absent from the graph' \
	  'FAIL: zero-length transferred ArrayBuffer is still detached' \
	  | sort > $(HTML_CLONE_DIR)/keep-buffer.expect; \
	 if [ $$rc -eq 1 ] && grep -qx 'html-clone: 58 checks, 8 failures' $(HTML_CLONE_DIR)/keep-buffer.log && \
	    diff -u $(HTML_CLONE_DIR)/keep-buffer.expect $(HTML_CLONE_DIR)/keep-buffer.fails; then \
	   echo 'html-clone negative control: all 58 checks ran; exactly 8 detachment assertions failed'; \
	   grep '^FAIL:' $(HTML_CLONE_DIR)/keep-buffer.log; \
	 else cat $(HTML_CLONE_DIR)/keep-buffer.log; echo 'html-clone negative control did not fail at its boundary'; exit 1; fi

$(HTML_CLONE_DIR)/san: $(HTML_CLONE_DEPS)
	@mkdir -p $(HTML_CLONE_DIR)
	@$(CC) $(HTML_CLONE_CF) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ $(HTML_CLONE_SRC) $(RUST_LIB_HOST) -lm

# Leak detection is unavailable on the supported Darwin/arm64 host. This still
# instruments QuickJS, clone, Blob/File, and the WebAPI C graph for ASan/UBSan;
# it deliberately does not claim to instrument the prebuilt Rust staticlib.
test-html-clone-san: test-html-clone $(HTML_CLONE_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	 $(HTML_CLONE_DIR)/san > $(HTML_CLONE_DIR)/san.log 2>&1; rc=$$?; \
	 grep -E '^(FAIL|HARNESS|html-clone:)|ERROR: AddressSanitizer|runtime error' $(HTML_CLONE_DIR)/san.log; exit $$rc
