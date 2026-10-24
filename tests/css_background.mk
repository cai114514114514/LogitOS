# Derive the actual parser/style/layout/paint road from its existing consumer.
# This pixel gate is independent of host performance and will also supply the
# same ordinary HTML page for the final guest scanout comparison.
CSS_BACKGROUND_DIR = $(BUILD)/css-background
CSS_BACKGROUND_SRC = $(filter-out tests/unit/paint_gfx_test.c,$(PAINTGFX_SRC)) tests/unit/css_background_test.c $(GFX_SRC) $(HTML_PARSER_SRC)
CSS_BACKGROUND_DEPS = $(CSS_BACKGROUND_SRC) $(BUILD)/libcss_host.a tests/fixtures/engine-expansion/css-background.html tests/fixtures/engine-expansion/css-background-pixels.csv $(wildcard c/apps/gui/browser/paint/box.* c/apps/gui/browser/paint/background.* c/apps/gui/browser/css/background.* c/apps/gui/browser/paint/*.h c/apps/gui/browser/css/*.h c/apps/gui/browser/layout/*.h) tests/unit/backgroundhost/logit.h tests/css_background.mk
$(CSS_BACKGROUND_DIR)/current: $(CSS_BACKGROUND_DEPS)
	@mkdir -p $(CSS_BACKGROUND_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_BACKGROUND_DIR)/no-base: $(CSS_BACKGROUND_DEPS)
	@mkdir -p $(CSS_BACKGROUND_DIR)
	$(CC) -O1 -g -w -DCSS_BACKGROUND_NO_BASE -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_SRC) $(BUILD)/libcss_host.a -lm
$(CSS_BACKGROUND_DIR)/old-box: $(CSS_BACKGROUND_DEPS)
	@mkdir -p $(CSS_BACKGROUND_DIR)
	$(CC) -O1 -g -w -DCSS_BACKGROUND_OLD_BOX -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-css-background test-css-background-negctl
$(CSS_BACKGROUND_DIR)/no-rclip: $(CSS_BACKGROUND_DEPS)
	@mkdir -p $(CSS_BACKGROUND_DIR)
	$(CC) -O1 -g -w -DPAINT_NEGCTL_NO_RCLIP -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_BACKGROUND_SRC) $(BUILD)/libcss_host.a -lm
test-css-background-negctl: $(CSS_BACKGROUND_DIR)/no-base $(CSS_BACKGROUND_DIR)/old-box $(CSS_BACKGROUND_DIR)/no-rclip
	@rc=0; $(CSS_BACKGROUND_DIR)/no-base > $(CSS_BACKGROUND_DIR)/no-base.log 2>&1 || rc=$$?; \
	cat $(CSS_BACKGROUND_DIR)/no-base.log; \
	test $$rc -eq 1 && grep -qx 'css-background: 26 checks, 3 failures' $(CSS_BACKGROUND_DIR)/no-base.log && \
	grep -qx 'FAIL: transparent gradient retains the box background color' $(CSS_BACKGROUND_DIR)/no-base.log && \
	grep -qx 'FAIL: translucent gradient composites over the box background color' $(CSS_BACKGROUND_DIR)/no-base.log && \
	grep -qx 'FAIL: element opacity applies once to the completed inner background' $(CSS_BACKGROUND_DIR)/no-base.log
	@rc=0; $(CSS_BACKGROUND_DIR)/old-box > $(CSS_BACKGROUND_DIR)/old-box.log 2>&1 || rc=$$?; \
	cat $(CSS_BACKGROUND_DIR)/old-box.log; \
	test $$rc -eq 1 && grep -qx 'css-background: 26 checks, 15 failures' $(CSS_BACKGROUND_DIR)/old-box.log && \
	grep -qx 'FAIL: rounded bottom-only border remains visible' $(CSS_BACKGROUND_DIR)/old-box.log && \
	grep -qx 'FAIL: gradient child honors the ancestor rounded overflow clip' $(CSS_BACKGROUND_DIR)/old-box.log
	@rc=0; $(CSS_BACKGROUND_DIR)/no-rclip > $(CSS_BACKGROUND_DIR)/no-rclip.log 2>&1 || rc=$$?; \
	cat $(CSS_BACKGROUND_DIR)/no-rclip.log; \
	test $$rc -eq 1 && grep -qx 'css-background: 26 checks, 2 failures' $(CSS_BACKGROUND_DIR)/no-rclip.log && \
	grep -qx 'FAIL: rounded overflow clips the ancestor rounded top-right corner' $(CSS_BACKGROUND_DIR)/no-rclip.log && \
	grep -qx 'FAIL: gradient child honors the ancestor rounded overflow clip' $(CSS_BACKGROUND_DIR)/no-rclip.log
test-css-background: test-css-background-negctl $(CSS_BACKGROUND_DIR)/current
	@ASAN_OPTIONS=detect_leaks=$(ASAN_LEAKS) UBSAN_OPTIONS=halt_on_error=1 $(CSS_BACKGROUND_DIR)/current $(CSS_BACKGROUND_DIR)/current.ppm
ci-host: test-css-background
