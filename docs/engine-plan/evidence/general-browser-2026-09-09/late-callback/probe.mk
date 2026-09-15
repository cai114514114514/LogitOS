LATE_DIR := build/wiring-next/late-callback
LATE_SRC = $(filter-out c/apps/browser/browser.c tests/unit/runtime_scroll_test.c,$(RUNTIME_SCROLL_SRC)) $(LATE_DIR)/probe.c
LATE_CF = $(RUNTIME_SCROLL_CF) -Ic/apps/browser -Iinclude
.PHONY: late-callback-negative late-callback-positive
$(LATE_DIR)/original: $(LATE_DIR)/browser.original.probe.c $(LATE_SRC)
	$(CC) $(LATE_CF) -o $@ $(LATE_DIR)/browser.original.probe.c $(LATE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(LATE_DIR)/patched: $(LATE_DIR)/browser.patched.probe.c $(LATE_SRC)
	$(CC) $(LATE_CF) -o $@ $(LATE_DIR)/browser.patched.probe.c $(LATE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
late-callback-negative: $(LATE_DIR)/original
	@for mode in 0 1 2; do $(LATE_DIR)/original $$mode > $(LATE_DIR)/original-$$mode.log 2>&1; rc=$$?; tail -8 $(LATE_DIR)/original-$$mode.log; test $$rc -eq 1 || exit 1; grep -q 'FAIL: late' $(LATE_DIR)/original-$$mode.log || exit 1; done
late-callback-positive: late-callback-negative $(LATE_DIR)/patched
	@for mode in 0 1 2 3; do $(LATE_DIR)/patched $$mode > $(LATE_DIR)/patched-$$mode.log 2>&1; rc=$$?; tail -8 $(LATE_DIR)/patched-$$mode.log; test $$rc -eq 0 || exit 1; done
