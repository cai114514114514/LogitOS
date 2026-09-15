# Extend the already source-derived real app_main fixture, not the bare
# __frameAdopt unit test that cannot detect installer-order omissions.
FRAME_BOOTSTRAP_SRC = $(sort $(filter-out tests/unit/popover_browser_test.c,$(POPOVER_BROWSER_SRC)) c/apps/browser/js_frame.c c/apps/browser/js_domparser.c) tests/unit/frame_bootstrap_wiring_test.c
FRAME_BOOTSTRAP_DEP = $(FRAME_BOOTSTRAP_SRC) $(POPOVER_BROWSER_DEP) $(wildcard c/apps/browser/*.inc)
$(BUILD)/wiring-next/frame_bootstrap_test: $(FRAME_BOOTSTRAP_DEP)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(FRAME_BOOTSTRAP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring-next/frame_bootstrap_negctl: $(FRAME_BOOTSTRAP_DEP)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) $(RUNTIME_SCROLL_CF) -DFRAME_BOOTSTRAP_LATE_INSTALL -o $@ $(FRAME_BOOTSTRAP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-frame-bootstrap-wiring test-frame-bootstrap-wiring-negctl
test-frame-bootstrap-wiring-negctl: $(BUILD)/wiring-next/frame_bootstrap_negctl
	@$(BUILD)/wiring-next/frame_bootstrap_negctl tests/fixtures/engine-expansion/frame-bootstrap.html > $(BUILD)/wiring-next/frame_bootstrap_negctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: parser and dynamic srcdoc both execute the same child script exactly once' $(BUILD)/wiring-next/frame_bootstrap_negctl.log
test-frame-bootstrap-wiring: test-frame-bootstrap-wiring-negctl $(BUILD)/wiring-next/frame_bootstrap_test
	@$(BUILD)/wiring-next/frame_bootstrap_test tests/fixtures/engine-expansion/frame-bootstrap.html
$(BUILD)/wiring-next/frame_sandbox_negctl: $(FRAME_BOOTSTRAP_DEP)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) $(RUNTIME_SCROLL_CF) -DFRAME_SANDBOX_BLANK_BYPASS -o $@ $(FRAME_BOOTSTRAP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-frame-sandbox-wiring-negctl
test-frame-sandbox-wiring-negctl: $(BUILD)/wiring-next/frame_sandbox_negctl
	@$(BUILD)/wiring-next/frame_sandbox_negctl tests/fixtures/engine-expansion/frame-bootstrap.html > $(BUILD)/wiring-next/frame_sandbox_negctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: sandboxed blank frames refuse access through all source forms' $(BUILD)/wiring-next/frame_sandbox_negctl.log && \
	 test "$$(grep -c '^\[frame\] FRAME-SANDBOX-BYPASS-RAN' $(BUILD)/wiring-next/frame_sandbox_negctl.log)" -eq 3
test-frame-bootstrap-wiring: test-frame-sandbox-wiring-negctl

$(BUILD)/wiring-next/frame_sandbox_url_negctl: $(FRAME_BOOTSTRAP_DEP)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) $(RUNTIME_SCROLL_CF) -DFRAME_SANDBOX_URL_BYPASS -o $@ $(FRAME_BOOTSTRAP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-frame-sandbox-url-negctl
test-frame-sandbox-url-negctl: $(BUILD)/wiring-next/frame_sandbox_url_negctl
	@$(BUILD)/wiring-next/frame_sandbox_url_negctl tests/fixtures/engine-expansion/frame-bootstrap.html > $(BUILD)/wiring-next/frame_sandbox_url_negctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: sandboxed blank frames refuse access through all source forms' $(BUILD)/wiring-next/frame_sandbox_url_negctl.log && \
	 test "$$(grep -c '^\[frame\] FRAME-SANDBOX-BYPASS-RAN' $(BUILD)/wiring-next/frame_sandbox_url_negctl.log)" -eq 1
test-frame-bootstrap-wiring: test-frame-sandbox-url-negctl
