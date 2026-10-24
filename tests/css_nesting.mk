# Real parser/cascade source road, derived from the established layout consumer.
# These controls only disable the new producer, leaving all native CSS active.
CSS_NEST_DIR = $(BUILD)/css-nesting
CSS_NEST_SRC = $(filter-out tests/unit/layout_box_test.c c/apps/gui/browser/layout/block.c c/apps/gui/browser/layout/text.c,$(LBOX_SRC)) tests/unit/css_nesting_test.c $(HTML_PARSER_SRC)
CSS_NEST_DEPS = $(CSS_NEST_SRC) $(wildcard c/apps/gui/browser/css/*.c c/apps/gui/browser/css/*.h c/apps/gui/browser/css/*.inc) $(BUILD)/libcss_host.a tests/css_nesting.mk
.PHONY: test-css-nesting test-css-nesting-negctl test-css-nesting-san
$(CSS_NEST_DIR)/current: $(CSS_NEST_DEPS)
	@mkdir -p $(CSS_NEST_DIR)
	$(CC) -O1 -g $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_NEST_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_NEST_DIR)/no-nesting: CSS_NEST_FAULT=CSS_NESTING_NO_NORMALIZE
$(CSS_NEST_DIR)/no-ratio: CSS_NEST_FAULT=CSS_SIZING_NO_RATIO
$(CSS_NEST_DIR)/no-nesting $(CSS_NEST_DIR)/no-ratio: $(CSS_NEST_DEPS)
	@mkdir -p $(CSS_NEST_DIR)
	$(CC) -O1 -g -D$(CSS_NEST_FAULT) $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_NEST_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_NEST_DIR)/false-limit.o: third_party/css/libcss/src/select/select.c third_party/css/libcss/src/select/select.h
	@mkdir -p $(CSS_NEST_DIR)
	$(CC) -O1 -g -fcommon -D_ALIGNED= -DCSS_ALT_LIMIT_AS_FALSE $(BTEST_INC) $(CSS_INC) -c $< -o $@
$(CSS_NEST_DIR)/false-limit: $(CSS_NEST_DEPS) $(CSS_NEST_DIR)/false-limit.o
	$(CC) -O1 -g $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_NEST_SRC) $(CSS_NEST_DIR)/false-limit.o $(BUILD)/libcss_host.a -lm
test-css-nesting-negctl: $(CSS_NEST_DIR)/no-nesting $(CSS_NEST_DIR)/no-ratio $(CSS_NEST_DIR)/false-limit
	@rc=0; $(CSS_NEST_DIR)/no-nesting nesting > $(CSS_NEST_DIR)/no-nesting.log 2>&1 || rc=$$?; cat $(CSS_NEST_DIR)/no-nesting.log; test $$rc -eq 1 && grep -qx 'css-nesting: 17 checks, 12 failures' $(CSS_NEST_DIR)/no-nesting.log && grep -qx 'FAIL: nesting: ampersand selects parent compound' $(CSS_NEST_DIR)/no-nesting.log
	@rc=0; $(CSS_NEST_DIR)/no-ratio ratio > $(CSS_NEST_DIR)/no-ratio.log 2>&1 || rc=$$?; cat $(CSS_NEST_DIR)/no-ratio.log; test $$rc -eq 1 && grep -qx 'css-nesting: 12 checks, 8 failures' $(CSS_NEST_DIR)/no-ratio.log && grep -qx 'FAIL: ratio: decimals exponent and leading auto' $(CSS_NEST_DIR)/no-ratio.log
	@rc=0; $(CSS_NEST_DIR)/false-limit resource > $(CSS_NEST_DIR)/false-limit.log 2>&1 || rc=$$?; cat $(CSS_NEST_DIR)/false-limit.log; test $$rc -eq 1 && grep -qx 'css-nesting: 5 checks, 1 failures' $(CSS_NEST_DIR)/false-limit.log && grep -qx 'FAIL: resource: shared alternative work ceiling is NOMEM not negation success' $(CSS_NEST_DIR)/false-limit.log
test-css-nesting: test-css-nesting-negctl $(CSS_NEST_DIR)/current
	@$(CSS_NEST_DIR)/current
$(CSS_NEST_DIR)/san: $(CSS_NEST_DEPS)
	@mkdir -p $(CSS_NEST_DIR)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_NEST_SRC) $(BUILD)/libcss_host.a -lm
test-css-nesting-san: test-css-nesting-negctl $(CSS_NEST_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(CSS_NEST_DIR)/san
# Established LibCSS host archive is not sanitized; this target covers adapter
# ownership/normalizer/value parsing, not every library allocation or teardown.
ci-host: test-css-nesting
