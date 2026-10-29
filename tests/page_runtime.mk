# The actual js_page consumer supplies page_runtime.c, rather than a second
# hand-copied timer model. The core checks additionally observe stale producer
# rejection which JavaScript cannot request after its old context is released.
PAGE_RUNTIME_SRC = tests/unit/page_runtime_test.c $(JSDOM_HOST_SRC)
PAGE_RUNTIME_DEPS = $(PAGE_RUNTIME_SRC) c/apps/gui/browser/app/page.c c/apps/gui/browser/app/page.h c/apps/gui/browser/js/runtime/page.h
PAGE_RUNTIME_CF = -O1 -g -w $(JSDOM_HOST_CF)
$(BUILD)/page_runtime_test: $(PAGE_RUNTIME_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_RUNTIME_CF) -o $@ $(PAGE_RUNTIME_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/page_runtime_epoch_negctl: $(PAGE_RUNTIME_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_RUNTIME_CF) -DPAGE_RUNTIME_STALE_EPOCH -o $@ $(PAGE_RUNTIME_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/page_runtime_capacity_negctl: $(PAGE_RUNTIME_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_RUNTIME_CF) -DPAGE_RUNTIME_UNBOUNDED -o $@ $(PAGE_RUNTIME_SRC) $(BUILD)/libcss_host.a -lm
