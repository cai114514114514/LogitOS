# Derive the full native browser harness; the minimal loader deliberately lacks
# the URL Standard parser, and cannot demonstrate this user action.
FRAME_OPEN_SRC = $(sort $(filter-out tests/unit/navigation_base_test.c,$(NAVIGATION_BASE_SRC)) c/apps/browser/js_url.c) tests/unit/frame_open_test.c
FRAME_OPEN_DIR = $(BUILD)/frame-open
FRAME_OPEN_DEPS = $(FRAME_OPEN_SRC) $(RUNTIME_SCROLL_DEPS)
$(FRAME_OPEN_DIR)/current: $(FRAME_OPEN_DEPS)
	@mkdir -p $(FRAME_OPEN_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(FRAME_OPEN_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(FRAME_OPEN_DIR)/old: $(FRAME_OPEN_DEPS)
	@mkdir -p $(FRAME_OPEN_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_FRAME_OPEN_NO_ACTION -o $@ $(FRAME_OPEN_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-frame-open test-frame-open-negctl
test-frame-open-negctl: $(FRAME_OPEN_DIR)/old
	@rc=0; $(FRAME_OPEN_DIR)/old open > $(FRAME_OPEN_DIR)/old.log 2>&1 || rc=$$?; tail -15 $(FRAME_OPEN_DIR)/old.log; test $$rc -eq 1 && grep -q '^FAIL: native frame action reaches the exact src destination' $(FRAME_OPEN_DIR)/old.log && test "$$(grep -c '^FAIL:' $(FRAME_OPEN_DIR)/old.log)" -eq 3
test-frame-open: test-frame-open-negctl $(FRAME_OPEN_DIR)/current
	@for mode in open relative cancel remove sandbox-late sandbox srcdoc data overflow path-limit credentials; do \
	 rc=0; $(FRAME_OPEN_DIR)/current $$mode > $(FRAME_OPEN_DIR)/$$mode.log 2>&1 || rc=$$?; tail -15 $(FRAME_OPEN_DIR)/$$mode.log; test $$rc -eq 0 || exit $$rc; done
ci-host: test-frame-open

.PHONY: test-frame-open-sanitize
test-frame-open-sanitize: test-frame-open
	@$(CC) $(filter-out -O2,$(RUNTIME_SCROLL_CF)) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $(FRAME_OPEN_DIR)/sanitize $(FRAME_OPEN_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@for mode in open remove sandbox-late; do \
	 rc=0; ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(FRAME_OPEN_DIR)/sanitize $$mode > $(FRAME_OPEN_DIR)/sanitize-$$mode.log 2>&1 || rc=$$?; tail -15 $(FRAME_OPEN_DIR)/sanitize-$$mode.log; test $$rc -eq 0 || exit $$rc; done
