LATE_SITE_DIR := $(BUILD)/site-general/late-callback
LATE_SITE_SRC = $(filter-out tests/unit/runtime_scroll_test.c,$(RUNTIME_SCROLL_SRC)) tests/unit/late_callbacks_test.c
LATE_SITE_DEPS = $(LATE_SITE_SRC) $(RUNTIME_SCROLL_DEPS) $(wildcard c/apps/browser/*.inc)
$(LATE_SITE_DIR)/test: $(LATE_SITE_DEPS)
	@mkdir -p $(LATE_SITE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_IDLE_OBSERVER -o $@ $(LATE_SITE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(LATE_SITE_DIR)/negctl: $(LATE_SITE_DEPS)
	@mkdir -p $(LATE_SITE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_IDLE_OBSERVER -DBROWSER_EARLY_CALLBACK_CONSUMERS -o $@ $(LATE_SITE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-late-callbacks test-late-callbacks-negctl
test-late-callbacks-negctl: $(LATE_SITE_DIR)/negctl
	@for mode in 0 1 2; do $(LATE_SITE_DIR)/negctl $$mode > $(LATE_SITE_DIR)/negative-$$mode.log 2>&1; rc=$$?; tail -8 $(LATE_SITE_DIR)/negative-$$mode.log; test $$rc -eq 1 && grep -q 'FAIL: late' $(LATE_SITE_DIR)/negative-$$mode.log || exit 1; done
test-late-callbacks: test-late-callbacks-negctl $(LATE_SITE_DIR)/test
	@for mode in 0 1 2 3; do $(LATE_SITE_DIR)/test $$mode > $(LATE_SITE_DIR)/positive-$$mode.log 2>&1; rc=$$?; tail -8 $(LATE_SITE_DIR)/positive-$$mode.log; test $$rc -eq 0 || exit 1; done
