# Only parser/metadata assertions here. Real sampling and rendered composition
# belong to the independent paint gates; acceptance is never equated to paint.
CSS_GRAD_DIR=$(BUILD)/css-gradient-parse
CSS_GRAD_SRC=$(filter-out tests/unit/css_paintdecl_test.c,$(CSSDECL_SRC)) tests/unit/css_gradient_parse_test.c $(HTML_PARSER_SRC)
CSS_GRAD_DEPS=$(CSS_GRAD_SRC) $(BUILD)/libcss_host.a $(wildcard c/apps/gui/browser/css/*.c c/apps/gui/browser/css/*.h c/apps/gui/browser/css/*.inc) tests/css_gradient_parse.mk
.PHONY: test-css-gradient-parse test-css-gradient-parse-negctl test-css-gradient-parse-san
$(CSS_GRAD_DIR)/current: $(CSS_GRAD_DEPS)
	@mkdir -p $(CSS_GRAD_DIR)
	$(CC) -O1 -g $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_GRAD_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_GRAD_DIR)/linear-only: $(CSS_GRAD_DEPS)
	@mkdir -p $(CSS_GRAD_DIR)
	$(CC) -O1 -g -DCSS_GRADIENT_LINEAR_ONLY $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_GRAD_SRC) $(BUILD)/libcss_host.a -lm
test-css-gradient-parse-negctl: $(CSS_GRAD_DIR)/linear-only
	@rc=0; $(CSS_GRAD_DIR)/linear-only types > $(CSS_GRAD_DIR)/linear-only.log 2>&1 || rc=$$?; cat $(CSS_GRAD_DIR)/linear-only.log; test $$rc -eq 1 && grep -qx 'css-gradient: 12 checks, 11 failures' $(CSS_GRAD_DIR)/linear-only.log && grep -qx 'FAIL: types: default radial ellipse center and extent' $(CSS_GRAD_DIR)/linear-only.log
test-css-gradient-parse: test-css-gradient-parse-negctl $(CSS_GRAD_DIR)/current
	@$(CSS_GRAD_DIR)/current
$(CSS_GRAD_DIR)/san: $(CSS_GRAD_DEPS)
	@mkdir -p $(CSS_GRAD_DIR)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_GRAD_SRC) $(BUILD)/libcss_host.a -lm
test-css-gradient-parse-san: test-css-gradient-parse-negctl $(CSS_GRAD_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(CSS_GRAD_DIR)/san
# Adapter/grammar sanitization; the established archive is not fully instrumented.
ci-host: test-css-gradient-parse
