# Installation-order and feature-query regressions use shipping parsers/layout.
CONTENT_WIRING_SRC = $(filter-out tests/unit/generated_content_test.c,$(GENERATED_SRC)) tests/unit/content_supports_wiring_test.c
CSS_LIVE_SRC = $(sort $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) c/apps/browser/js_cssom.c c/apps/browser/css_extra.c c/apps/browser/layout.c c/apps/browser/layout_text.c) tests/unit/css_live_wiring_test.c
.PHONY: test-content-supports-wiring test-content-supports-wiring-negctl test-css-live-wiring test-css-live-wiring-negctl
$(BUILD)/content_supports_wiring_test: $(CONTENT_WIRING_SRC) tests/unit/generated_content_test.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(CONTENT_WIRING_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/css_live_wiring_test: $(CSS_LIVE_SRC) tests/unit/select_state_test.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(CSS_LIVE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/content_supports_wiring_negctl: $(CONTENT_WIRING_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_CONTENT_SUPPORTS_NEGCTL -o $@ $(CONTENT_WIRING_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-content-supports-wiring-negctl: $(BUILD)/content_supports_wiring_negctl
	@rc=0; $< > $(BUILD)/content_supports_wiring_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/content_supports_wiring_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: @supports generated branch' $(BUILD)/content_supports_wiring_negctl.log
test-content-supports-wiring: test-content-supports-wiring-negctl $(BUILD)/content_supports_wiring_test
	@$(BUILD)/content_supports_wiring_test
test-css-live-wiring-negctl: $(CSS_LIVE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@set -e; for n in MEDIA_OVERWRITE DIRTY_EDGE MEDIA_SCANNER; do \
	 $(CC) -O2 -w $(DOMIFACE_CF) -DCSS_LIVE_NEGCTL_$$n -o $(BUILD)/css_live_negctl_$$n $(CSS_LIVE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm; \
	 rc=0; $(BUILD)/css_live_negctl_$$n > $(BUILD)/css_live_negctl_$$n.log 2>&1 || rc=$$?; cat $(BUILD)/css_live_negctl_$$n.log; test $$rc -eq 1; \
	 case $$n in MEDIA_OVERWRITE) grep -q '^FAIL live media updates after viewport' $(BUILD)/css_live_negctl_$$n.log;; MEDIA_SCANNER) grep -q '^FAIL live media shares cascade evaluator' $(BUILD)/css_live_negctl_$$n.log;; DIRTY_EDGE) grep -q '^FAIL second consecutive dirty geometry flush' $(BUILD)/css_live_negctl_$$n.log;; esac; done
test-css-live-wiring: test-css-live-wiring-negctl $(BUILD)/css_live_wiring_test
	@$(BUILD)/css_live_wiring_test

INLINE_EXT_SRC = $(filter-out tests/unit/generated_content_test.c,$(GENERATED_SRC)) tests/unit/css_inline_extensions_test.c
.PHONY: test-css-inline-extensions test-css-inline-extensions-negctl
$(BUILD)/css_inline_extensions_test: $(INLINE_EXT_SRC) tests/unit/generated_content_test.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(INLINE_EXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/css_inline_extensions_negctl: $(INLINE_EXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_EXTRA_NO_INLINE_FALLBACK -o $@ $(INLINE_EXT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-css-inline-extensions-negctl: $(BUILD)/css_inline_extensions_negctl
	@rc=0; $< > $(BUILD)/css_inline_extensions_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/css_inline_extensions_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: inline grid reaches layout without stylesheet extension rules' $(BUILD)/css_inline_extensions_negctl.log
test-css-inline-extensions: test-css-inline-extensions-negctl $(BUILD)/css_inline_extensions_test
	@$(BUILD)/css_inline_extensions_test

PSEUDO_SKIP_SRC = $(filter-out tests/unit/generated_content_test.c,$(GENERATED_SRC)) tests/unit/css_pseudo_skip_test.c
.PHONY: test-css-pseudo-skip test-css-pseudo-skip-negctl
$(BUILD)/css_pseudo_skip_test: $(PSEUDO_SKIP_SRC) tests/unit/generated_content_test.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(PSEUDO_SKIP_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/css_pseudo_skip_negctl: $(PSEUDO_SKIP_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_GENERATED_NO_EMPTY_SKIP -o $@ $(PSEUDO_SKIP_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-css-pseudo-skip-negctl: $(BUILD)/css_pseudo_skip_negctl
	@rc=0; $< > $(BUILD)/css_pseudo_skip_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/css_pseudo_skip_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: empty pseudo reset avoids composition work' $(BUILD)/css_pseudo_skip_negctl.log
test-css-pseudo-skip: test-css-pseudo-skip-negctl $(BUILD)/css_pseudo_skip_test
	@$(BUILD)/css_pseudo_skip_test

FOCUS_STYLE_SRC = $(filter-out tests/unit/css_live_wiring_test.c,$(CSS_LIVE_SRC)) tests/unit/focus_style_flush_test.c
.PHONY: test-focus-style-flush test-focus-style-flush-negctl
$(BUILD)/focus_style_flush_test: $(FOCUS_STYLE_SRC) tests/unit/select_state_test.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(FOCUS_STYLE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/focus_style_flush_negctl: $(FOCUS_STYLE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	$(CC) -O2 -w $(DOMIFACE_CF) -DFOCUS_NO_STYLE_FLUSH -o $@ $(FOCUS_STYLE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-focus-style-flush-negctl: $(BUILD)/focus_style_flush_negctl
	@rc=0; $< > $(BUILD)/focus_style_flush_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/focus_style_flush_negctl.log; test $$rc -eq 1 && grep -q '^FAIL new hidden ancestor blocks synchronous focus' $(BUILD)/focus_style_flush_negctl.log
test-focus-style-flush: test-focus-style-flush-negctl test-focus-style-generation-negctl $(BUILD)/focus_style_flush_test
	@$(BUILD)/focus_style_flush_test

.PHONY: test-focus-style-generation-negctl
$(BUILD)/focus_style_generation_negctl: $(FOCUS_STYLE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	$(CC) -O2 -w $(DOMIFACE_CF) -DCSS_NEGCTL_CAPACITY_FINGERPRINT -o $@ $(FOCUS_STYLE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-focus-style-generation-negctl: $(BUILD)/focus_style_generation_negctl
	@rc=0; $< > $(BUILD)/focus_style_generation_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/focus_style_generation_negctl.log; test $$rc -eq 1 && grep -q '^FAIL show then focus works without an intervening geometry read' $(BUILD)/focus_style_generation_negctl.log
