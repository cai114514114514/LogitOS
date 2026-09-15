DOM_WRAPPER_SRC = $(sort $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) c/apps/browser/js_domparser.c c/apps/browser/js_frame.c) tests/unit/dom_wrapper_lifetime_test.c
DOM_WRAPPER_DEPS = $(DOM_WRAPPER_SRC) tests/unit/select_state_test.c c/apps/browser/js_dom.h $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-dom-wrapper-lifetime test-dom-wrapper-lifetime-negctl
$(BUILD)/wiring-next/dom_wrapper_lifetime: $(DOM_WRAPPER_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(DOM_WRAPPER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-dom-wrapper-lifetime-negctl: $(DOM_WRAPPER_DEPS)
	@mkdir -p $(BUILD)/wiring-next
	@set -e; for ctl in JSDOM_WEAK_WRAPPERS JSDOM_NO_WRAPPER_GC_MARK; do \
	 $(CC) -O2 -w $(DOMIFACE_CF) -D$$ctl -o $(BUILD)/wiring-next/wrapper_$$ctl $(DOM_WRAPPER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm; \
	 set +e; $(BUILD)/wiring-next/wrapper_$$ctl > $(BUILD)/wiring-next/wrapper_$$ctl.log 2>&1; rc=$$?; set -e; cat $(BUILD)/wiring-next/wrapper_$$ctl.log; test $$rc -eq 1; \
	 case $$ctl in JSDOM_WEAK_WRAPPERS) grep -q 'FAIL connected Symbol expando survives wrapper release' $(BUILD)/wiring-next/wrapper_$$ctl.log;; JSDOM_NO_WRAPPER_GC_MARK) grep -q 'FAIL detached wrapper cycles are collected' $(BUILD)/wiring-next/wrapper_$$ctl.log;; esac; done

test-dom-wrapper-lifetime: test-dom-wrapper-lifetime-negctl $(BUILD)/wiring-next/dom_wrapper_lifetime
	@$(BUILD)/wiring-next/dom_wrapper_lifetime
.PHONY: test-dom-wrapper-lifetime-asan
test-dom-wrapper-lifetime-asan: test-dom-wrapper-lifetime
	@$(CC) -O1 -g -w -fsanitize=address -fno-omit-frame-pointer $(DOMIFACE_CF) -o $(BUILD)/wiring-next/dom_wrapper_asan $(DOM_WRAPPER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring-next/dom_wrapper_asan
