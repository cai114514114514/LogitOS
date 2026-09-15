# HTML raw-text tokenizer fast path.  The bytewise binary is the exact former
# algorithm and is required to fail the pointer-identity performance invariant;
# semantic equivalence remains covered by both the focused checks and the full
# html5lib token/tree suites.

HTML_RAWTEXT_PERF_DIR := $(BUILD)/html-rawtext-perf
HTML_RAWTEXT_PERF_SRC := tests/unit/html_rawtext_perf_test.c c/apps/browser/html_tokenizer.c
HTML_RAWTEXT_PERF_DEP := $(HTML_RAWTEXT_PERF_SRC) c/apps/browser/html_tokenizer.h \
                         c/apps/browser/html_entities.inc c/apps/browser/html_tags.inc \
                         tests/html_rawtext_perf.mk

$(HTML_RAWTEXT_PERF_DIR)/current: $(HTML_RAWTEXT_PERF_DEP)
	@mkdir -p $(HTML_RAWTEXT_PERF_DIR)
	$(CC) -O2 -w $(BTEST_INC) -o $@ $(HTML_RAWTEXT_PERF_SRC)

$(HTML_RAWTEXT_PERF_DIR)/bytewise: $(HTML_RAWTEXT_PERF_DEP)
	@mkdir -p $(HTML_RAWTEXT_PERF_DIR)
	$(CC) -O2 -w $(BTEST_INC) -DHTML_RAWTEXT_BYTEWISE -o $@ $(HTML_RAWTEXT_PERF_SRC)

.PHONY: test-html-rawtext-fast-negctl test-html-rawtext-fast \
        test-html-rawtext-fast-asan bench-html-rawtext

test-html-rawtext-fast-negctl: $(HTML_RAWTEXT_PERF_DIR)/bytewise
	@rc=0; $< > $(HTML_RAWTEXT_PERF_DIR)/bytewise.log 2>&1 || rc=$$?; \
	 cat $(HTML_RAWTEXT_PERF_DIR)/bytewise.log; \
	 test $$rc -eq 1 && \
	 grep -F 'FAIL: ordinary raw-text runs borrow the input buffer' $(HTML_RAWTEXT_PERF_DIR)/bytewise.log && \
	 grep -F 'html raw-text fast path: 7 checks, 1 failure' $(HTML_RAWTEXT_PERF_DIR)/bytewise.log

test-html-rawtext-fast: test-html-rawtext-fast-negctl $(HTML_RAWTEXT_PERF_DIR)/current
	@$(HTML_RAWTEXT_PERF_DIR)/current

test-html-rawtext-fast-asan: test-html-rawtext-fast-negctl
	@mkdir -p $(HTML_RAWTEXT_PERF_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all \
	 -fno-omit-frame-pointer $(BTEST_INC) -o $(HTML_RAWTEXT_PERF_DIR)/sanitize \
	 $(HTML_RAWTEXT_PERF_SRC)
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	 $(HTML_RAWTEXT_PERF_DIR)/sanitize

BENCH_HTML_RAWTEXT_ITERS ?= 64
bench-html-rawtext: $(HTML_RAWTEXT_PERF_DIR)/current $(HTML_RAWTEXT_PERF_DIR)/bytewise
	@echo 'current (host attribution only)'
	@$(HTML_RAWTEXT_PERF_DIR)/current --bench $(BENCH_HTML_RAWTEXT_ITERS)
	@echo 'bytewise control (host attribution only)'
	@$(HTML_RAWTEXT_PERF_DIR)/bytewise --bench $(BENCH_HTML_RAWTEXT_ITERS)

ci-host: test-html-rawtext-fast
