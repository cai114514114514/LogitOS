# Timer semantics use a controlled monotonic clock, never host elapsed time.
# Each harness textually includes the production owner it observes; this lets
# ID wrap and allocation-failure checks inspect actual ownership rather than a
# copied queue model. The shared public JS runs in both Window and Worker.
HTML_TIMERS_DIR = $(BUILD)/html-timers
HTML_TIMERS_CORE = $(filter-out tests/unit/page_runtime_test.c c/apps/gui/browser/js/runtime/page.c,$(PAGE_RUNTIME_SRC))
HTML_TIMERS_PAGE = tests/unit/html_timers_test.c $(HTML_TIMERS_CORE)
HTML_TIMERS_WORKER = $(HTML_TIMERS_PAGE) c/apps/gui/browser/js/runtime/page.c c/net/http/url.c tests/unit/loader_fakebfetch.c
HTML_TIMERS_DEPS = tests/unit/html_timers_test.c tests/fixtures/html-priority/timers.js c/apps/gui/browser/js/runtime/page.c c/apps/gui/browser/js/workers/worker.c c/apps/gui/browser/js/runtime/timers.h c/apps/gui/browser/app/page.c c/apps/gui/browser/app/page.h $(HTML_TIMERS_WORKER) tests/html_timers.mk $(BROWSER_INTERFACE_DEPS)
HTML_TIMERS_CF = -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(JSDOM_HOST_CF) -Itests/unit
HTML_TIMERS_SAN = ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=86 UBSAN_OPTIONS=halt_on_error=1

$(HTML_TIMERS_DIR)/page: $(HTML_TIMERS_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(HTML_TIMERS_DIR)
	$(CC) $(HTML_TIMERS_CF) -DHTML_TIMERS_PAGE -o $@ $(HTML_TIMERS_PAGE) $(BUILD)/libcss_host.a -lm
$(HTML_TIMERS_DIR)/worker: $(HTML_TIMERS_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(HTML_TIMERS_DIR)
	$(CC) $(HTML_TIMERS_CF) -DHTML_TIMERS_WORKER -o $@ $(HTML_TIMERS_WORKER) $(BUILD)/libcss_host.a -lm
$(HTML_TIMERS_DIR)/page-no-clamp: $(HTML_TIMERS_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(HTML_TIMERS_DIR)
	$(CC) $(HTML_TIMERS_CF) -DHTML_TIMERS_PAGE -DJS_TIMER_NO_CLAMP -o $@ $(HTML_TIMERS_PAGE) $(BUILD)/libcss_host.a -lm
$(HTML_TIMERS_DIR)/worker-no-clamp: $(HTML_TIMERS_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(HTML_TIMERS_DIR)
	$(CC) $(HTML_TIMERS_CF) -DHTML_TIMERS_WORKER -DJS_TIMER_NO_CLAMP -o $@ $(HTML_TIMERS_WORKER) $(BUILD)/libcss_host.a -lm

.PHONY: test-html-timers test-html-timers-negctl
test-html-timers-negctl: $(HTML_TIMERS_DIR)/page-no-clamp $(HTML_TIMERS_DIR)/worker-no-clamp
	@rc=0; $(HTML_TIMERS_SAN) $(HTML_TIMERS_DIR)/page-no-clamp > $(HTML_TIMERS_DIR)/page-no-clamp.log 2>&1 || rc=$$?; cat $(HTML_TIMERS_DIR)/page-no-clamp.log; test $$rc -eq 1 && grep -q '^FAIL shared nesting 6 clamps to 4 ms' $(HTML_TIMERS_DIR)/page-no-clamp.log && grep -q '^FAIL page nested timeouts apply' $(HTML_TIMERS_DIR)/page-no-clamp.log
	@rc=0; $(HTML_TIMERS_SAN) $(HTML_TIMERS_DIR)/worker-no-clamp > $(HTML_TIMERS_DIR)/worker-no-clamp.log 2>&1 || rc=$$?; cat $(HTML_TIMERS_DIR)/worker-no-clamp.log; test $$rc -eq 1 && grep -q '^FAIL shared nesting 6 clamps to 4 ms' $(HTML_TIMERS_DIR)/worker-no-clamp.log && grep -q '^FAIL worker nested timeouts apply' $(HTML_TIMERS_DIR)/worker-no-clamp.log
	@echo 'html-timers-negctl: PASS -- missing clamp fails shared rules and both real task loops'
test-html-timers: test-html-timers-negctl $(HTML_TIMERS_DIR)/page $(HTML_TIMERS_DIR)/worker
	@$(HTML_TIMERS_SAN) $(HTML_TIMERS_DIR)/page
	@$(HTML_TIMERS_SAN) $(HTML_TIMERS_DIR)/worker
