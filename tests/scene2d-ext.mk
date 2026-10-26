# tests/scene2d-ext.mk -- ol_scene2d extensions (c/lib/gfx/scene/scene2d.c):
# per-layer 24.8 translate / 8.8 scale, a set of disjoint damage rects, effect
# extent, z-reorder and hit-testing.
#
# The control is -DOL_SCENE_SINGLE_DAMAGE: damage collapses to one union rect, as
# before. Everything still draws correctly, so only the "damage-area" check may
# fail, and the control fails if its log lacks that line or the binary never
# reached the end of the run.

.PHONY: test-scene2d-ext test-scene2d-ext-negctl

SCNX_CF = -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC)

$(BUILD)/scene_ext_test: tests/unit/openlogit_scene_ext_test.c $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(SCNX_CF) -o $@ tests/unit/openlogit_scene_ext_test.c $(GFX_SRC) -lm

test-scene2d-ext-negctl:
	@mkdir -p $(BUILD)
	$(CC) $(SCNX_CF) -DOL_SCENE_SINGLE_DAMAGE -o $(BUILD)/scene_ext_negctl \
	    tests/unit/openlogit_scene_ext_test.c $(GFX_SRC) -lm
	@echo "--- negative control: the SAME assertions with single-rect damage ---"
	@if ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/scene_ext_negctl > $(BUILD)/scene_ext_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: single-rect damage met the area check"; \
	    exit 1; \
	elif ! grep -q '^FAIL damage-area' $(BUILD)/scene_ext_negctl.log \
	     || ! grep -q '^scene2d-ext: [0-9]* checks, ' $(BUILD)/scene_ext_negctl.log; then \
	    cat $(BUILD)/scene_ext_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL damage-area' line"; \
	    exit 1; \
	else \
	    grep '^FAIL' $(BUILD)/scene_ext_negctl.log; \
	    echo "negative control ok: damage-area fails with a single union rect"; \
	fi

test-scene2d-ext: test-scene2d-ext-negctl $(BUILD)/scene_ext_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/scene_ext_test

ci-host: test-scene2d-ext
