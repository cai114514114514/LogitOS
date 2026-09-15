# Union the actual CSSOM and painter pipelines; never copy their growing lists.
ELEMENT_SCROLL_SRC = $(sort $(filter-out tests/unit/cssom_test.c tests/unit/inline_hit_test.c,$(CSSOM_TEST_SRC) $(INLINE_HIT_SRC))) tests/unit/element_scroll_test.c
ELEMENT_SCROLL_DEPS = $(ELEMENT_SCROLL_SRC) tests/unit/cssom_test.c c/apps/browser/element_scroll_wiring.inc c/apps/browser/text_paint_wiring.inc $(QJS_SRC) $(BUILD)/libcss_host.a
ELEMENT_SCROLL_FLAGS = -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.PHONY: test-element-scroll test-element-scroll-negctl
$(BUILD)/wiring-next/element_scroll_test: $(ELEMENT_SCROLL_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(ELEMENT_SCROLL_FLAGS) -o $@ $(ELEMENT_SCROLL_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
test-element-scroll-negctl: $(ELEMENT_SCROLL_DEPS)
	@mkdir -p $(BUILD)/wiring-next
	$(CC) $(ELEMENT_SCROLL_FLAGS) -DELEMENT_SCROLL_NO_PROJECTION -o $(BUILD)/wiring-next/element_scroll_negctl $(ELEMENT_SCROLL_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/wiring-next/element_scroll_negctl > $(BUILD)/wiring-next/element_scroll_negctl.log 2>&1; rc=$$?; cat $(BUILD)/wiring-next/element_scroll_negctl.log; test $$rc -eq 1 && grep -q 'FAIL real painter moves descendant' $(BUILD)/wiring-next/element_scroll_negctl.log
test-element-scroll: test-element-scroll-negctl $(BUILD)/wiring-next/element_scroll_test
	$(BUILD)/wiring-next/element_scroll_test
.PHONY: test-element-scroll-asan
test-element-scroll-asan: test-element-scroll
	$(CC) $(ELEMENT_SCROLL_FLAGS) -fsanitize=address -fno-omit-frame-pointer -o $(BUILD)/wiring-next/element_scroll_asan $(ELEMENT_SCROLL_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring-next/element_scroll_asan
