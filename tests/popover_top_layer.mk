# Real runtime API and actual layout/painter are separate consumers. The
# prerequisite control removes native admission while preserving the JS API.
POPOVER_RUNTIME_SRC = $(filter-out tests/unit/modal_runtime_test.c,$(MODAL_RUNTIME_SRC)) tests/unit/popover_runtime_test.c
POPOVER_PAINT_SRC = $(filter-out tests/unit/modal_paint_test.c,$(MODAL_PAINT_SRC)) tests/unit/popover_paint_test.c
POPOVER_DEPS = $(MODAL_DEPS) tests/unit/select_state_test.c tests/unit/modal_paint_test.c tests/unit/painthost/logit.h $(wildcard c/apps/browser/*.inc)
$(BUILD)/wiring-next/popover_runtime_test: $(POPOVER_RUNTIME_SRC) $(POPOVER_DEPS) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(POPOVER_RUNTIME_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring-next/popover_negctl: $(POPOVER_RUNTIME_SRC) $(POPOVER_DEPS) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) -O2 -w $(DOMIFACE_CF) -DPOPOVER_NO_NATIVE_LAYER -o $@ $(POPOVER_RUNTIME_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring-next/popover_paint_test: $(POPOVER_PAINT_SRC) $(POPOVER_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(POPOVER_PAINT_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-popover-top-layer test-popover-top-layer-negctl
test-popover-top-layer-negctl: $(BUILD)/wiring-next/popover_negctl
	@$(BUILD)/wiring-next/popover_negctl > $(BUILD)/wiring-next/popover_negctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL popover enters real nonmodal top layer' $(BUILD)/wiring-next/popover_negctl.log
test-popover-top-layer: test-popover-top-layer-negctl $(BUILD)/wiring-next/popover_runtime_test $(BUILD)/wiring-next/popover_paint_test
	@$(BUILD)/wiring-next/popover_runtime_test
	@$(BUILD)/wiring-next/popover_paint_test

# The same fixture that exposed the guest-only invalidation bug must pass the
# real embedder's restyle gate, not a test callback that always invokes layout.
POPOVER_BROWSER_SRC = $(sort $(filter-out tests/unit/runtime_scroll_test.c,$(RUNTIME_SCROLL_SRC)) $(filter-out tests/unit/popover_runtime_test.c,$(POPOVER_RUNTIME_SRC))) tests/unit/popover_browser_test.c
POPOVER_BROWSER_DEP = $(POPOVER_BROWSER_SRC) $(POPOVER_DEPS) $(RUNTIME_SCROLL_DEPS)
$(BUILD)/wiring-next/popover_browser_test: $(POPOVER_BROWSER_DEP)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(POPOVER_BROWSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring-next/popover_browser_negctl: $(POPOVER_BROWSER_DEP)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) $(RUNTIME_SCROLL_CF) -DPOPOVER_PAINT_INVALIDATION -o $@ $(POPOVER_BROWSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-popover-browser test-popover-browser-negctl
test-popover-browser-negctl: $(BUILD)/wiring-next/popover_browser_negctl
	@$(BUILD)/wiring-next/popover_browser_negctl tests/fixtures/engine-expansion/popover.html > $(BUILD)/wiring-next/popover_browser_negctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: first synchronous popover show produces actual browser geometry' $(BUILD)/wiring-next/popover_browser_negctl.log
test-popover-browser: test-popover-browser-negctl $(BUILD)/wiring-next/popover_browser_test
	@$(BUILD)/wiring-next/popover_browser_test tests/fixtures/engine-expansion/popover.html
test-popover-top-layer: test-popover-browser
$(BUILD)/wiring-next/popover_invoker_negctl: $(POPOVER_BROWSER_DEP)
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) $(RUNTIME_SCROLL_CF) -DSEMANTICS_NO_NATIVE_INVOKER -o $@ $(POPOVER_BROWSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-popover-invoker-negctl
test-popover-invoker-negctl: $(BUILD)/wiring-next/popover_invoker_negctl
	@$(BUILD)/wiring-next/popover_invoker_negctl tests/fixtures/engine-expansion/popover.html > $(BUILD)/wiring-next/popover_invoker_negctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: native popover input and lifecycle complete within bounded polls' $(BUILD)/wiring-next/popover_invoker_negctl.log && \
	 grep -F 'popover native stage=3 toggle count=0 pending=0' $(BUILD)/wiring-next/popover_invoker_negctl.log
test-popover-browser: test-popover-invoker-negctl
