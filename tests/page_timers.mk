# Same core runtime as test-js-dom-asan; no copied QuickJS/HTML source lists.
.PHONY: test-page-timers test-page-timers-negctl
PAGE_TIMERS_CF = -O1 -g -w -fsanitize=address -fno-omit-frame-pointer $(JSDOM_HOST_CF)
PAGE_TIMERS_DEPS = tests/unit/page_timers_test.c $(JSDOM_HOST_SRC) c/apps/browser/js_page.h c/apps/browser/dom.h c/apps/browser/css.h
PAGE_TIMERS_ASAN = ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=86

$(BUILD)/page_timers_test: $(PAGE_TIMERS_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_TIMERS_CF) -o $@ tests/unit/page_timers_test.c $(JSDOM_HOST_SRC) $(BUILD)/libcss_host.a -lm

# The mutated sources live under BUILD. Do not toggle production source files
# while another agent or a guest image build might be reading them.
$(BUILD)/page_timers_%_negctl.c: c/apps/browser/js_page.c tests/unit/page_timers_negctl.py
	@mkdir -p $(BUILD)
	python3 tests/unit/page_timers_negctl.py $* $< $@

$(BUILD)/page_timers_%_negctl: $(BUILD)/page_timers_%_negctl.c $(PAGE_TIMERS_DEPS) $(BUILD)/libcss_host.a
	$(CC) $(PAGE_TIMERS_CF) -o $@ tests/unit/page_timers_test.c $(filter-out c/apps/browser/js_page.c,$(JSDOM_HOST_SRC)) $< $(BUILD)/libcss_host.a -lm

test-page-timers-negctl: $(BUILD)/page_timers_this_negctl \
                         $(BUILD)/page_timers_lifetime_negctl \
                         $(BUILD)/page_timers_starvation_negctl
	@rc=0; $(PAGE_TIMERS_ASAN) $(BUILD)/page_timers_this_negctl --interval > $(BUILD)/page_timers_this_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/page_timers_this_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL strict interval receives window' $(BUILD)/page_timers_this_negctl.log || \
	 { echo 'page-timers-negctl: FAIL -- missing receiver assertion'; exit 1; }
	@rc=0; $(PAGE_TIMERS_ASAN) $(BUILD)/page_timers_lifetime_negctl > $(BUILD)/page_timers_lifetime_negctl.log 2>&1 || rc=$$?; \
	 test "$$rc" -eq 86 && grep -q '^page-timers: shipping queue entered' $(BUILD)/page_timers_lifetime_negctl.log && \
	 grep -q 'ERROR: AddressSanitizer: heap-use-after-free' $(BUILD)/page_timers_lifetime_negctl.log || \
	 { cat $(BUILD)/page_timers_lifetime_negctl.log; echo 'page-timers-negctl: FAIL -- missing lifetime diagnosis'; exit 1; }; \
	 echo 'page-timers-negctl: PASS -- restored freed timer read detected by AddressSanitizer'
	@rc=0; $(PAGE_TIMERS_ASAN) $(BUILD)/page_timers_starvation_negctl > $(BUILD)/page_timers_starvation_negctl.log 2>&1 || rc=$$?; \
	 test "$$rc" -eq 1 && grep -q '^FAIL resumed turn checkpoints pending fetches' $(BUILD)/page_timers_starvation_negctl.log || \
	 { cat $(BUILD)/page_timers_starvation_negctl.log; echo 'page-timers-negctl: FAIL -- missing fetch-starvation diagnosis'; exit 1; }; \
	 echo 'page-timers-negctl: PASS -- removing the resumed-turn checkpoint starves the synthetic fetch'

test-page-timers: test-page-timers-negctl $(BUILD)/page_timers_test
	$(PAGE_TIMERS_ASAN) $(BUILD)/page_timers_test
