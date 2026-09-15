# Finite ordinary images only; no budget/fault/stress fixture prerequisites.
SVG_STYLESHEET_DIR = $(BUILD)/svg-stylesheet
SVG_STYLESHEET_SRC = $(filter-out tests/unit/svg_scene_test.c,$(SVG_SCENE_SRC)) tests/unit/svg_stylesheet_test.c
SVG_STYLESHEET_DEP = $(SVG_STYLESHEET_SRC) $(wildcard c/lib/image/svg*.inc c/lib/gfx/*.h) c/lib/image/img.h tests/svg_stylesheet.mk
$(SVG_STYLESHEET_DIR)/current: $(SVG_STYLESHEET_DEP)
	@mkdir -p $(SVG_STYLESHEET_DIR)
	$(CC) -O1 -g -Wall -Wextra $(IMG_HOST_INC) $(SVG_STYLESHEET_SRC) -o $@ -lm
$(SVG_STYLESHEET_DIR)/legacy: $(SVG_STYLESHEET_DEP)
	@mkdir -p $(SVG_STYLESHEET_DIR)
	$(CC) -O1 -g -DSVG_SCENE_NO_STYLESHEET $(IMG_HOST_INC) $(SVG_STYLESHEET_SRC) -o $@ -lm
$(SVG_STYLESHEET_DIR)/san: $(SVG_STYLESHEET_DEP)
	@mkdir -p $(SVG_STYLESHEET_DIR)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(IMG_HOST_INC) $(SVG_STYLESHEET_SRC) -o $@ -lm
.PHONY: test-svg-stylesheet test-svg-stylesheet-negctl test-svg-stylesheet-asan
test-svg-stylesheet-negctl: $(SVG_STYLESHEET_DIR)/legacy
	@python3 tests/unit/svg_stylesheet_check.py $< $(SVG_STYLESHEET_DIR)/legacy.log legacy
test-svg-stylesheet: test-svg-stylesheet-negctl $(SVG_STYLESHEET_DIR)/current
	@python3 tests/unit/svg_stylesheet_check.py $(SVG_STYLESHEET_DIR)/current $(SVG_STYLESHEET_DIR)/current.log current
test-svg-stylesheet-asan: test-svg-stylesheet-negctl $(SVG_STYLESHEET_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 python3 tests/unit/svg_stylesheet_check.py $(SVG_STYLESHEET_DIR)/san $(SVG_STYLESHEET_DIR)/san.log current
ci-host: test-svg-stylesheet
