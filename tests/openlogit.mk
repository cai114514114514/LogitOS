# Native graphics API above the existing rasterizer. A private control removes
# only frame commit; the same ordinary drawing assertion must visibly fail.
include tests/openlogit_scroll.mk
include tests/openlogit_canvas.mk
$(BUILD)/openlogit_test: tests/unit/openlogit_test.c $(GFX_SRC) c/lib/gfx/include/openlogit.h c/apps/gui/clock/geometry.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) -pthread $< $(GFX_SRC) -o $@
$(BUILD)/openlogit_neg: tests/unit/openlogit_test.c $(GFX_SRC) c/lib/gfx/include/openlogit.h c/apps/gui/clock/geometry.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DOPENLOGIT_NO_COMMIT $(GFX_INC) -pthread $< $(GFX_SRC) -o $@
test-openlogit-neg: $(BUILD)/openlogit_neg
	@$(BUILD)/openlogit_neg > $(BUILD)/openlogit-neg.log 2>&1; rc=$$?; test $$rc -ne 0 && rg '^FAIL ordinary frame commits' $(BUILD)/openlogit-neg.log
test-openlogit: test-openlogit-neg $(BUILD)/openlogit_test
	$(BUILD)/openlogit_test
ci-host: test-openlogit
.PHONY: test-openlogit test-openlogit-neg

$(BUILD)/clock.elf: c/apps/gui/openlogit_window.h c/lib/gfx/include/openlogit.h c/apps/gui/clock/geometry.h
$(GFX_OBJ): c/lib/gfx/include/openlogit.h
test-openlogit-os: test-openlogit $(ISO) $(DISK)
	python3 tests/boot/run-openlogit.py --build $(BUILD)
ci-boot: test-openlogit-os
.PHONY: test-openlogit-os

$(BUILD)/openlogit_clock_neg: tests/unit/openlogit_test.c $(GFX_SRC) c/lib/gfx/include/openlogit.h c/apps/gui/clock/geometry.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DCLOCK_PATH_CAP=512 $(GFX_INC) -pthread $< $(GFX_SRC) -o $@
test-openlogit-clock-neg: $(BUILD)/openlogit_clock_neg
	@$(BUILD)/openlogit_clock_neg > $(BUILD)/openlogit-clock-neg.log 2>&1; rc=$$?; tail -4 $(BUILD)/openlogit-clock-neg.log; test $$rc -ne 0 && rg -q '^FAIL Clock device geometry fits' $(BUILD)/openlogit-clock-neg.log
test-openlogit: test-openlogit-clock-neg
.PHONY: test-openlogit-clock-neg

$(BUILD)/openlogit_anim_test: tests/unit/openlogit_anim_test.c c/lib/gfx/animation/openlogit_anim.c c/lib/gfx/include/openlogit_anim.h c/lib/gfx/geometry/gfx_math.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< c/lib/gfx/animation/openlogit_anim.c c/lib/gfx/geometry/gfx_math.c -lm -o $@
$(BUILD)/openlogit_anim_neg: tests/unit/openlogit_anim_test.c c/lib/gfx/animation/openlogit_anim.c c/lib/gfx/include/openlogit_anim.h c/lib/gfx/geometry/gfx_math.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DOPENLOGIT_ANIM_FROZEN $(GFX_INC) $< c/lib/gfx/animation/openlogit_anim.c c/lib/gfx/geometry/gfx_math.c -lm -o $@
test-openlogit-animation-neg: $(BUILD)/openlogit_anim_neg
	@$(BUILD)/openlogit_anim_neg > $(BUILD)/openlogit-animation-neg.log 2>&1; rc=$$?; test $$rc -ne 0 && rg '^FAIL animation has a real intermediate value' $(BUILD)/openlogit-animation-neg.log
test-openlogit-animation: test-openlogit-animation-neg $(BUILD)/openlogit_anim_test
	$(BUILD)/openlogit_anim_test
test-openlogit: test-openlogit-animation
.PHONY: test-openlogit-animation test-openlogit-animation-neg

OL3D_SRC := $(sort $(shell find c/lib/gfx3d -name '*.c'))
OL3D_HEADERS := $(sort $(shell find c/lib/gfx3d -name '*.h'))
OL3D_INC := $(addprefix -I,$(sort $(dir $(OL3D_HEADERS))))
include tests/openlogit_scene.mk
$(BUILD)/openlogit_3d_test: tests/unit/openlogit_3d_test.c $(OL3D_SRC) $(OL3D_HEADERS) $(GFX_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_3d_depth_neg: tests/unit/openlogit_3d_test.c $(OL3D_SRC) $(OL3D_HEADERS) $(GFX_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O2 -DOPENLOGIT_DEPTH_DISABLED $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_3d_shader_neg: tests/unit/openlogit_3d_test.c $(OL3D_SRC) $(OL3D_HEADERS) $(GFX_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O2 -DOPENLOGIT_SHADER_DISABLED $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
test-openlogit-3d-neg: $(BUILD)/openlogit_3d_depth_neg $(BUILD)/openlogit_3d_shader_neg
	@$(BUILD)/openlogit_3d_depth_neg > $(BUILD)/openlogit-depth-neg.log 2>&1; rc=$$?; test $$rc -ne 0 && rg '^FAIL depth keeps' $(BUILD)/openlogit-depth-neg.log
	@$(BUILD)/openlogit_3d_shader_neg > $(BUILD)/openlogit-shader-neg.log 2>&1; rc=$$?; test $$rc -ne 0 && rg '^FAIL shared edge covers' $(BUILD)/openlogit-shader-neg.log
test-openlogit-3d: test-openlogit-3d-neg $(BUILD)/openlogit_3d_test
	$(BUILD)/openlogit_3d_test
ci-host: test-openlogit-3d
.PHONY: test-openlogit-3d test-openlogit-3d-neg

$(BUILD)/openlogit_v11_test: tests/unit/openlogit_v11_test.c $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< $(GFX_SRC) -o $@
test-openlogit-v11: test-openlogit-neg $(BUILD)/openlogit_v11_test
	$(BUILD)/openlogit_v11_test
test-openlogit: test-openlogit-v11
.PHONY: test-openlogit-v11

$(BUILD)/openlogit_3d_sanitize: tests/unit/openlogit_3d_test.c $(OL3D_SRC) $(OL3D_HEADERS) $(GFX_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_v11_sanitize: tests/unit/openlogit_v11_test.c $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $< $(GFX_SRC) -lm -o $@
test-openlogit-sanitize: test-openlogit-3d-neg test-openlogit-neg $(BUILD)/openlogit_3d_sanitize $(BUILD)/openlogit_v11_sanitize
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/openlogit_3d_sanitize
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/openlogit_v11_sanitize
ci-host: test-openlogit-sanitize
.PHONY: test-openlogit-sanitize

test-openlogit-consumers-neg:
	@mkdir -p $(BUILD)
	@rc=0; python3 tools/check_openlogit_consumers.py --inject-legacy > $(BUILD)/openlogit-consumer-neg.log 2>&1 || rc=$$?; test $$rc -ne 0 && rg '^FAIL .*legacy raster bypass' $(BUILD)/openlogit-consumer-neg.log
test-openlogit-consumers: test-openlogit-consumers-neg
	python3 tools/check_openlogit_consumers.py --output $(BUILD)/openlogit-consumers.json
test-openlogit: test-openlogit-consumers
.PHONY: test-openlogit-consumers test-openlogit-consumers-neg

MOTION_CACHE_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/openlogit_motion_cache_test.c
MOTION_CACHE_DEPS = $(MOTION_CACHE_SRC) tests/unit/passive_layout_context_test.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
$(BUILD)/motion_cache_test: $(MOTION_CACHE_DEPS)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(MOTION_CACHE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/motion_cache_neg: $(MOTION_CACHE_DEPS)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DOPENLOGIT_MOTION_INVALIDATION_DISABLED -o $@ $(MOTION_CACHE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-openlogit-motion-cache-neg: $(BUILD)/motion_cache_neg
	@rc=0; $< > $(BUILD)/motion-cache-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL: motion change invalidates' $(BUILD)/motion-cache-neg.log && rg '^FAIL: motion change reaches' $(BUILD)/motion-cache-neg.log
test-openlogit-motion-cache: test-openlogit-motion-cache-neg $(BUILD)/motion_cache_test
	$(BUILD)/motion_cache_test
ci-host: test-openlogit-motion-cache
.PHONY: test-openlogit-motion-cache test-openlogit-motion-cache-neg

$(BUILD)/openlogit_effects_test: tests/unit/openlogit_effects_test.c $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< $(GFX_SRC) -o $@
$(BUILD)/openlogit_effects_neg: tests/unit/openlogit_effects_test.c $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -DOPENLOGIT_EFFECT_BLUR_DISABLED $(GFX_INC) $< $(GFX_SRC) -o $@
$(BUILD)/openlogit_effects_sanitize: tests/unit/openlogit_effects_test.c $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $< $(GFX_SRC) -o $@
test-openlogit-effects-neg: $(BUILD)/openlogit_effects_neg
	@rc=0; $< > $(BUILD)/openlogit-effects-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL premultiplied blur matches' $(BUILD)/openlogit-effects-neg.log
test-openlogit-effects: test-openlogit-effects-neg $(BUILD)/openlogit_effects_test $(BUILD)/openlogit_effects_sanitize
	$(BUILD)/openlogit_effects_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/openlogit_effects_sanitize
test-openlogit: test-openlogit-effects
.PHONY: test-openlogit-effects test-openlogit-effects-neg

include tests/openlogit_material.mk
include tests/openlogit_lsl.mk
