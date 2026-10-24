# One pure numeric oracle plus real GUI pixels. The pure oracle links the same
# grammar/color authority; the image oracle derives the shipping road from
# PAINTGFX_SRC rather than inventing another source list. Both use existing
# in-tree math/color code; nothing is downloaded.
CSS_GRADIENT_DIR = $(BUILD)/css-gradients
CSS_GRADIENT_MODULES = c/apps/gui/browser/paint/gradient.c c/apps/gui/browser/paint/gradient.h c/apps/gui/browser/css/gradient.c c/apps/gui/browser/css/gradient.h
CSS_GRADIENT_UNIT_SRC = tests/unit/css_gradient_test.c c/apps/gui/browser/paint/gradient.c c/apps/gui/browser/css/gradient.c c/lib/image/svg.c $(GFX_SRC)
CSS_GRADIENT_PIXEL_SRC = $(filter-out tests/unit/paint_gfx_test.c,$(PAINTGFX_SRC)) tests/unit/css_gradient_pixels_test.c $(GFX_SRC) $(HTML_PARSER_SRC)
CSS_GRADIENT_SANITIZERS = -fsanitize=address,undefined -fno-sanitize-recover=all
CSS_GRADIENT_PIXEL_DEPS = $(CSS_GRADIENT_PIXEL_SRC) $(CSS_GRADIENT_MODULES) $(BUILD)/libcss_host.a tests/unit/css_background_test.c tests/unit/backgroundhost/logit.h tests/unit/css_gradient_control.py tests/fixtures/engine-expansion/css-gradients.html tests/fixtures/engine-expansion/css-gradients-pixels.csv $(wildcard c/apps/gui/browser/paint/*.c c/apps/gui/browser/paint/*.h c/apps/gui/browser/css/*.h c/apps/gui/browser/layout/*.h) tests/css_gradients.mk
$(CSS_GRADIENT_DIR)/numeric: $(CSS_GRADIENT_UNIT_SRC) $(CSS_GRADIENT_MODULES) tests/css_gradients.mk
	@mkdir -p $(CSS_GRADIENT_DIR)
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all $(BTEST_INC) -Iinclude -o $@ $(CSS_GRADIENT_UNIT_SRC) -lm
define CSS_GRADIENT_PIXEL_BUILD
	@mkdir -p $(CSS_GRADIENT_DIR)
	$(CC) -O1 -g -w $(1) -Itests/unit/backgroundhost $(BTEST_INC) $(CSS_INC) -o $@ $(CSS_GRADIENT_PIXEL_SRC) $(BUILD)/libcss_host.a -lm
endef
$(CSS_GRADIENT_DIR)/current: $(CSS_GRADIENT_PIXEL_DEPS)
	$(call CSS_GRADIENT_PIXEL_BUILD,$(CSS_GRADIENT_SANITIZERS))
$(CSS_GRADIENT_DIR)/no-premult: $(CSS_GRADIENT_PIXEL_DEPS)
	$(call CSS_GRADIENT_PIXEL_BUILD,-DCSS_GRADIENT_NO_PREMULT)
$(CSS_GRADIENT_DIR)/bad-fixup: $(CSS_GRADIENT_PIXEL_DEPS)
	$(call CSS_GRADIENT_PIXEL_BUILD,-DCSS_GRADIENT_BAD_FIXUP)
$(CSS_GRADIENT_DIR)/no-repeat: $(CSS_GRADIENT_PIXEL_DEPS)
	$(call CSS_GRADIENT_PIXEL_BUILD,-DCSS_GRADIENT_NO_REPEAT)
.PHONY: test-css-gradients test-css-gradients-negctl
test-css-gradients-negctl: $(CSS_GRADIENT_DIR)/no-premult $(CSS_GRADIENT_DIR)/bad-fixup $(CSS_GRADIENT_DIR)/no-repeat
	python3 tests/unit/css_gradient_control.py $(CSS_GRADIENT_DIR)/no-premult no-premult $(CSS_GRADIENT_DIR)/no-premult.log
	python3 tests/unit/css_gradient_control.py $(CSS_GRADIENT_DIR)/bad-fixup bad-fixup $(CSS_GRADIENT_DIR)/bad-fixup.log
	python3 tests/unit/css_gradient_control.py $(CSS_GRADIENT_DIR)/no-repeat no-repeat $(CSS_GRADIENT_DIR)/no-repeat.log
test-css-gradients: test-css-gradients-negctl $(CSS_GRADIENT_DIR)/numeric $(CSS_GRADIENT_DIR)/current
	@ASAN_OPTIONS=detect_leaks=$(ASAN_LEAKS) UBSAN_OPTIONS=halt_on_error=1 $(CSS_GRADIENT_DIR)/numeric
	@ASAN_OPTIONS=detect_leaks=$(ASAN_LEAKS) UBSAN_OPTIONS=halt_on_error=1 $(CSS_GRADIENT_DIR)/current $(CSS_GRADIENT_DIR)/current.ppm
ci-host: test-css-gradients
