# The current producer/cache/painter, derived from the same shared paint group.
# Fault paint keeps discovery and geometry intact but removes bitmap samples;
# the companion fault disables resource loading. Neither can pass on a count.
CSS_BACKGROUND_IMAGE_DIR := $(BUILD)/css-background-image
CSS_BACKGROUND_IMAGE_SRC = $(filter-out tests/unit/paint_gfx_test.c,$(PAINTGFX_SRC)) tests/unit/css_background_image_test.c $(GFX_SRC) $(HTML_PARSER_SRC)
CSS_BACKGROUND_IMAGE_DEPS = $(CSS_BACKGROUND_IMAGE_SRC) $(BROWSER_STYLE_MODULES) $(BROWSER_HEADERS) tests/unit/css_background_test.c tests/unit/backgroundhost/logit.h c/apps/gui/browser/app/background.c tests/css_background_image.mk
.SECONDEXPANSION:
.PHONY: test-css-background-image test-css-background-image-negctl test-css-background-image-grammar-negctl test-css-background-image-san
$(CSS_BACKGROUND_IMAGE_DIR)/current: $$(CSS_BACKGROUND_IMAGE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CSS_BACKGROUND_IMAGE_DIR)
	$(CC) -O2 -w -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_IMAGE_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_BACKGROUND_IMAGE_DIR)/no-pixels: $$(CSS_BACKGROUND_IMAGE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CSS_BACKGROUND_IMAGE_DIR)
	$(CC) -O2 -w -DCSS_BACKGROUND_NO_BITMAP -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_IMAGE_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_BACKGROUND_IMAGE_DIR)/no-load: $$(CSS_BACKGROUND_IMAGE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CSS_BACKGROUND_IMAGE_DIR)
	$(CC) -O2 -w -DCSS_BACKGROUND_NO_LOAD -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_IMAGE_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_BACKGROUND_IMAGE_DIR)/center-order: $$(CSS_BACKGROUND_IMAGE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CSS_BACKGROUND_IMAGE_DIR)
	$(CC) -O2 -w -DCSS_BACKGROUND_CENTER_ORDER -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_IMAGE_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit: $$(CSS_BACKGROUND_IMAGE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CSS_BACKGROUND_IMAGE_DIR)
	$(CC) -O2 -w -DCSS_BACKGROUND_RAW_INHERIT -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_IMAGE_SRC) $(BUILD)/libcss_host.a -lm

# The controls retain the real parser/cascade/layout/painter and change only
# the two producer edges. A crash or an unrelated failing assertion is not red
# evidence: require the normal exit and the exact causal public observations.
test-css-background-image-grammar-negctl: $(CSS_BACKGROUND_IMAGE_DIR)/center-order $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit
	@rc=0; $(CSS_BACKGROUND_IMAGE_DIR)/center-order grammar > $(CSS_BACKGROUND_IMAGE_DIR)/center-order.log 2>&1 || rc=$$?; \
	cat $(CSS_BACKGROUND_IMAGE_DIR)/center-order.log; \
	test $$rc -eq 1 && test $$(grep -c '^FAIL:' $(CSS_BACKGROUND_IMAGE_DIR)/center-order.log) -eq 4 && \
	grep -qx 'css-background-grammar: 22 checks, 4 failures' $(CSS_BACKGROUND_IMAGE_DIR)/center-order.log && \
	grep -qx 'FAIL: center-first horizontal edge position is supported' $(CSS_BACKGROUND_IMAGE_DIR)/center-order.log && \
	grep -qx 'FAIL: center-first right offset keeps the vertical center' $(CSS_BACKGROUND_IMAGE_DIR)/center-order.log && \
	grep -qx 'FAIL: center-first right position paints its horizontal edge offset' $(CSS_BACKGROUND_IMAGE_DIR)/center-order.log && \
	grep -qx 'FAIL: center-first right position paints the centered vertical band' $(CSS_BACKGROUND_IMAGE_DIR)/center-order.log
	@rc=0; $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit grammar > $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log 2>&1 || rc=$$?; \
	cat $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log; \
	test $$rc -eq 1 && test $$(grep -c '^FAIL:' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log) -eq 7 && \
	grep -qx 'css-background-grammar: 22 checks, 7 failures' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log && \
	grep -qx 'FAIL: longhand inherit spelling publishes one stable canonical keyword' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log && \
	grep -qx 'FAIL: shorthand inherit spelling publishes canonical keywords for every slot' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log && \
	grep -qx 'FAIL: commented longhand inherit retains the parent computed size' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log && \
	grep -qx 'FAIL: commented longhand inherit retains the parent computed position' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log && \
	grep -qx 'FAIL: commented shorthand inherit retains the parent geometry and font basis' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log && \
	grep -qx 'FAIL: commented longhand inherit paints the parent image over its own color' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log && \
	grep -qx 'FAIL: commented shorthand inherit paints the parent image in the child box' $(CSS_BACKGROUND_IMAGE_DIR)/raw-inherit.log
test-css-background-image-negctl: test-css-background-image-grammar-negctl $(CSS_BACKGROUND_IMAGE_DIR)/no-pixels $(CSS_BACKGROUND_IMAGE_DIR)/no-load
	@python3 tests/unit/css_background_image_control.py $(CSS_BACKGROUND_IMAGE_DIR)/no-pixels $(CSS_BACKGROUND_IMAGE_DIR)/no-load
test-css-background-image: test-css-background-image-negctl $(CSS_BACKGROUND_IMAGE_DIR)/current
	@$(CSS_BACKGROUND_IMAGE_DIR)/current
$(CSS_BACKGROUND_IMAGE_DIR)/san: $$(CSS_BACKGROUND_IMAGE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CSS_BACKGROUND_IMAGE_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_IMAGE_SRC) $(BUILD)/libcss_host.a -lm
test-css-background-image-san: test-css-background-image-negctl $(CSS_BACKGROUND_IMAGE_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(CSS_BACKGROUND_IMAGE_DIR)/san
ci-host: test-css-background-image
