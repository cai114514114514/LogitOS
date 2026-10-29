# Same core runtime as test-js-dom-asan; no copied QuickJS/HTML source lists.
PAGE_TIMERS_CF = -O1 -g -w -fsanitize=address -fno-omit-frame-pointer $(JSDOM_HOST_CF)
PAGE_TIMERS_DEPS = tests/unit/page_timers_test.c $(JSDOM_HOST_SRC) c/apps/gui/browser/js/runtime/page.h c/apps/gui/browser/js/runtime/timers.h c/apps/gui/browser/app/page.c c/apps/gui/browser/app/page.h c/apps/gui/browser/dom/tree.h c/apps/gui/browser/css/style.h
PAGE_TIMERS_ASAN = ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=86

$(BUILD)/page_timers_test: $(PAGE_TIMERS_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_TIMERS_CF) -o $@ tests/unit/page_timers_test.c $(JSDOM_HOST_SRC) $(BUILD)/libcss_host.a -lm

# The mutated sources live under BUILD. Do not toggle production source files
# while another agent or a guest image build might be reading them.
$(BUILD)/page_timers_%_negctl.c: c/apps/gui/browser/js/runtime/page.c tests/unit/page_timers_negctl.py
	@mkdir -p $(BUILD)
	python3 tests/unit/page_timers_negctl.py $* $< $@

$(BUILD)/page_timers_%_negctl: $(BUILD)/page_timers_%_negctl.c $(PAGE_TIMERS_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_TIMERS_CF) -o $@ tests/unit/page_timers_test.c $(filter-out c/apps/gui/browser/js/runtime/page.c,$(JSDOM_HOST_SRC)) $< $(BUILD)/libcss_host.a -lm


