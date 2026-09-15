# The guest specimen is the authority for the workload. Disable ONLY its clock
# reads on the host; never publish host timings as browser performance.
SIMPLE_SELECTOR_SRC = $(filter-out tests/unit/selectors_test.c,$(SELECTORS_SRC)) tests/unit/simple_selector_test.c
SIMPLE_SELECTOR_DEP = $(SIMPLE_SELECTOR_SRC) tests/unit/dom_iface_test.c tests/simple_selector.mk c/apps/browser/js_dom.h $(HTML_PARSER_SRC) $(QJS_SRC) $(wildcard c/apps/browser/js_*.inc) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
SIMPLE_SELECTOR_FIXTURES = $(BUILD)/wiring/simple_selector_fixture.html $(BUILD)/wiring/selector_candidate_fixture.html
$(BUILD)/wiring/simple_selector_fixture.html: tools/perf/browser_load.py
	@mkdir -p $(BUILD)/wiring
	@python3 -c 'import runpy; print(runpy.run_path("tools/perf/browser_load.py")["script_specimen"]().replace("performance.now()", "0"))' > $@
$(BUILD)/wiring/selector_candidate_fixture.html: tests/fixtures/engine-expansion/selector-candidates.html
	@mkdir -p $(BUILD)/wiring
	@python3 -c 'import pathlib; print(pathlib.Path("$<").read_text().replace("performance.now()", "0"))' > $@
$(BUILD)/wiring/simple_selector_test: $(SIMPLE_SELECTOR_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) -O2 -w $(SELECTORS_CF) -DJSDOM_TRAVERSAL_PROFILE -o $@ $(SIMPLE_SELECTOR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring/simple_selector_negctl: $(SIMPLE_SELECTOR_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) -O2 -w $(SELECTORS_CF) -DJSDOM_TRAVERSAL_PROFILE -DSIMPLE_SELECTOR_JS_ONLY -o $@ $(SIMPLE_SELECTOR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring/selector_candidate_negctl: $(SIMPLE_SELECTOR_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) -O2 -w $(SELECTORS_CF) -DJSDOM_TRAVERSAL_PROFILE -DSELECTOR_NO_CANDIDATES -o $@ $(SIMPLE_SELECTOR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-simple-selector test-simple-selector-negctl test-selector-candidate-negctl
test-simple-selector-negctl: $(BUILD)/wiring/simple_selector_negctl $(SIMPLE_SELECTOR_FIXTURES)
	@$(BUILD)/wiring/simple_selector_negctl $(SIMPLE_SELECTOR_FIXTURES) > $(BUILD)/wiring/simple_selector_negctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL simple query avoids per-candidate JS attribute reads' $(BUILD)/wiring/simple_selector_negctl.log
test-selector-candidate-negctl: $(BUILD)/wiring/selector_candidate_negctl $(SIMPLE_SELECTOR_FIXTURES)
	@$(BUILD)/wiring/selector_candidate_negctl $(SIMPLE_SELECTOR_FIXTURES) > $(BUILD)/wiring/selector_candidate_negctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL candidate filter avoids JS attribute reads on impossible matches' $(BUILD)/wiring/selector_candidate_negctl.log && \
	 grep -F 'ok: compound workload retains all five matches in all thirty queries' $(BUILD)/wiring/selector_candidate_negctl.log
test-simple-selector: test-simple-selector-negctl test-selector-candidate-negctl $(BUILD)/wiring/simple_selector_test $(SIMPLE_SELECTOR_FIXTURES)
	$(BUILD)/wiring/simple_selector_test $(SIMPLE_SELECTOR_FIXTURES)

.PHONY: test-simple-selector-asan
test-simple-selector-asan: test-simple-selector-negctl test-selector-candidate-negctl
	@mkdir -p $(BUILD)/wiring
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(SELECTORS_CF) -DJSDOM_TRAVERSAL_PROFILE -o $(BUILD)/wiring/simple_selector_asan $(SIMPLE_SELECTOR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/simple_selector_asan $(SIMPLE_SELECTOR_FIXTURES)
ci-host: test-simple-selector

SELECTOR_TOKENS_SRC = $(filter-out tests/unit/selectors_test.c,$(SELECTORS_SRC)) tests/unit/selector_tokens_test.c
SELECTOR_TOKENS_DEP = $(SELECTOR_TOKENS_SRC) tests/unit/dom_iface_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(wildcard c/apps/browser/js_*.inc) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(BUILD)/wiring/selector_tokens_test: $(SELECTOR_TOKENS_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) -O2 -w $(SELECTORS_CF) -o $@ $(SELECTOR_TOKENS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring/selector_tokens_old: $(SELECTOR_TOKENS_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) -O2 -w $(SELECTORS_CF) -DSELECTOR_NO_TOKEN_CACHE -o $@ $(SELECTOR_TOKENS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-selector-tokens test-selector-tokens-negctl
test-selector-tokens-negctl: $(BUILD)/wiring/selector_tokens_old
	@rc=0; $< > $(BUILD)/wiring/selector_tokens_old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL unchanged class string is tokenized once' $(BUILD)/wiring/selector_tokens_old.log && \
	 grep -F 'selector-tokens: 12 checks, 2 failures' $(BUILD)/wiring/selector_tokens_old.log
test-selector-tokens: test-selector-tokens-negctl $(BUILD)/wiring/selector_tokens_test
	$(BUILD)/wiring/selector_tokens_test
ci-host: test-selector-tokens

SELECTOR_BATCH_SRC = $(filter-out tests/unit/selectors_test.c,$(SELECTORS_SRC)) tests/unit/selector_batch_test.c
SELECTOR_BATCH_DEP = $(SELECTOR_BATCH_SRC) tests/unit/dom_iface_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(wildcard c/apps/browser/js_*.inc) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(BUILD)/wiring/selector_batch_test: $(SELECTOR_BATCH_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) -O2 -w $(SELECTORS_CF) -DJSDOM_TRAVERSAL_PROFILE -o $@ $(SELECTOR_BATCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/wiring/selector_batch_old: $(SELECTOR_BATCH_DEP)
	@mkdir -p $(BUILD)/wiring
	$(CC) -O2 -w $(SELECTORS_CF) -DJSDOM_TRAVERSAL_PROFILE -DSELECTOR_NO_BATCH_CANDIDATES -o $@ $(SELECTOR_BATCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-selector-batch test-selector-batch-negctl
test-selector-batch-negctl: $(BUILD)/wiring/selector_batch_old
	@rc=0; $< > $(BUILD)/wiring/selector_batch_old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL selector union rejects impossible nodes before JS matching' $(BUILD)/wiring/selector_batch_old.log && \
	 grep -F 'selector-batch: 15 checks, 2 failures' $(BUILD)/wiring/selector_batch_old.log
test-selector-batch: test-selector-batch-negctl $(BUILD)/wiring/selector_batch_test
	$(BUILD)/wiring/selector_batch_test
ci-host: test-selector-batch

.PHONY: test-selector-batch-asan
test-selector-batch-asan: test-selector-batch-negctl
	@mkdir -p $(BUILD)/wiring
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(SELECTORS_CF) -DJSDOM_TRAVERSAL_PROFILE -o $(BUILD)/wiring/selector_batch_asan $(SELECTOR_BATCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring/selector_batch_asan
