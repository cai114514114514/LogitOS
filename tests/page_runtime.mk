# The actual js_page consumer supplies page_runtime.c, rather than a second
# hand-copied timer model. The core checks additionally observe stale producer
# rejection which JavaScript cannot request after its old context is released.
.PHONY: test-page-runtime test-page-runtime-negctl
PAGE_RUNTIME_SRC = tests/unit/page_runtime_test.c $(JSDOM_HOST_SRC)
PAGE_RUNTIME_DEPS = $(PAGE_RUNTIME_SRC) c/apps/browser/page_runtime.c c/apps/browser/page_runtime.h c/apps/browser/js_page.h
PAGE_RUNTIME_CF = -O1 -g -w $(JSDOM_HOST_CF)
$(BUILD)/page_runtime_test: $(PAGE_RUNTIME_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_RUNTIME_CF) -o $@ $(PAGE_RUNTIME_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/page_runtime_epoch_negctl: $(PAGE_RUNTIME_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_RUNTIME_CF) -DPAGE_RUNTIME_STALE_EPOCH -o $@ $(PAGE_RUNTIME_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/page_runtime_capacity_negctl: $(PAGE_RUNTIME_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_RUNTIME_CF) -DPAGE_RUNTIME_UNBOUNDED -o $@ $(PAGE_RUNTIME_SRC) $(BUILD)/libcss_host.a -lm
# These controls alter ordinary acceptance rules and exit through assertions.
# Keep them prerequisites: naming them only on ci-host would not protect a
# developer who directly runs the positive gate.
test-page-runtime-negctl: $(BUILD)/page_runtime_epoch_negctl $(BUILD)/page_runtime_capacity_negctl
	@rc=0; $(BUILD)/page_runtime_epoch_negctl > $(BUILD)/page_runtime_epoch_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/page_runtime_epoch_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL old page token rejected after reopen' $(BUILD)/page_runtime_epoch_negctl.log && \
	 grep -q '^FAIL old producer cannot enqueue into next page' $(BUILD)/page_runtime_epoch_negctl.log
	@rc=0; $(BUILD)/page_runtime_capacity_negctl > $(BUILD)/page_runtime_capacity_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/page_runtime_capacity_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL queue capacity enforced' $(BUILD)/page_runtime_capacity_negctl.log
	@echo 'page-runtime-negctl: PASS -- generation and capacity assertion failures observed'
test-page-runtime: test-page-runtime-negctl $(BUILD)/page_runtime_test
	@$(BUILD)/page_runtime_test
