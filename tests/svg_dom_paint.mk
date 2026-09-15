SVG_DOM_DIR = $(BUILD)/svg-dom-paint
SVG_DOM_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) $(IMG_HOST_SRC) tests/unit/svg_dom_paint_test.c
SVG_DOM_DEPS = $(SVG_DOM_SRC) $(HTML_PARSER_SRC) $(wildcard c/apps/browser/layout*.inc c/apps/browser/svg*.inc c/apps/browser/svg*.h c/lib/image/svg*.inc) c/apps/browser/css.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
SVG_DOM_FLAGS ?= -O2
.PHONY: test-svg-dom-paint test-svg-dom-paint-negctl test-svg-dom-paint-asan
$(SVG_DOM_DIR)/test: $(SVG_DOM_DEPS)
	@mkdir -p $(SVG_DOM_DIR)
	@$(CC) $(SVG_DOM_FLAGS) -w $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm -o $@ $(sort $(SVG_DOM_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(SVG_DOM_DIR)/legacy: $(SVG_DOM_DEPS)
	@mkdir -p $(SVG_DOM_DIR)
	@$(CC) $(SVG_DOM_FLAGS) -w -DLAYOUT_SVG_RAW_SOURCE $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm -o $@ $(sort $(SVG_DOM_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(SVG_DOM_DIR)/asan: $(SVG_DOM_DEPS)
	@mkdir -p $(SVG_DOM_DIR)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -w $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm -o $@ $(sort $(SVG_DOM_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-svg-dom-paint-negctl: $(SVG_DOM_DIR)/legacy
	@rc=0; $< > $(SVG_DOM_DIR)/legacy.log 2>&1 || rc=$$?; cat $(SVG_DOM_DIR)/legacy.log; test $$rc -eq 1 && grep -q 'FAIL: createElementNS subtree renders real pixels' $(SVG_DOM_DIR)/legacy.log && grep -q 'FAIL: DOM mutation changes real pixels' $(SVG_DOM_DIR)/legacy.log
test-svg-dom-paint: test-svg-dom-paint-negctl $(SVG_DOM_DIR)/test
	@$(SVG_DOM_DIR)/test
test-svg-dom-paint-asan: test-svg-dom-paint-negctl $(SVG_DOM_DIR)/asan
	@$(SVG_DOM_DIR)/asan
