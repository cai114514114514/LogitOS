# tests/gfx-gradient-conic.mk -- conic gradient, repeat/reflect spread, 16 stops and
# premultiplied stop interpolation (c/lib/gfx/raster/gfx_gradient.c), and SYS_GUI_PATH
# accepting them (c/kernel/gui/wm_draw2_path.c).
#
# The control is -DGFX_STOPS_STRAIGHT_LERP: stops are interpolated in straight
# alpha again, so red fading to transparent black goes dark and the "fade-hue"
# check must fail -- and ONLY that check: the control also fails if the log lacks
# the 'FAIL fade-hue' line, if any other check fails, or if the binary dies
# before reaching the summary.

.PHONY: test-gfx-gradient-conic test-gfx-gradient-conic-negctl

GGC_SRC := tests/unit/gfx_conic_test.c c/kernel/gui/wm_draw2_path.c $(GFX_SRC)
GGC_CF  := -O1 -g -Wall -Wextra $(GFX_INC) $(HOST_INCDIRS)
GGC_DEPS := $(GGC_SRC) $(GFX_HEADERS) c/kernel/gui/wm_draw2.h c/kernel/gui/wm2_scale.h include/abi/logit_abi.h

$(BUILD)/gfx_conic_test: $(GGC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GGC_CF) -o $@ $(GGC_SRC) -lm

$(BUILD)/gfx_conic_negctl: $(GGC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GGC_CF) -DGFX_STOPS_STRAIGHT_LERP -o $@ $(GGC_SRC) -lm

test-gfx-gradient-conic-negctl: $(BUILD)/gfx_conic_negctl
	@echo "--- negative control: the SAME assertions with straight-alpha stop interpolation ---"
	@if $(BUILD)/gfx_conic_negctl > $(BUILD)/gfx_conic_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: straight-alpha stops passed the fade-hue check"; \
	    exit 1; \
	elif ! grep -q '^FAIL fade-hue' $(BUILD)/gfx_conic_negctl.log \
	     || ! grep -q '^[0-9]* checks, ' $(BUILD)/gfx_conic_negctl.log; then \
	    cat $(BUILD)/gfx_conic_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL fade-hue' line"; \
	    exit 1; \
	elif grep '^FAIL' $(BUILD)/gfx_conic_negctl.log | grep -qv '^FAIL fade-hue'; then \
	    cat $(BUILD)/gfx_conic_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than fade-hue failed"; \
	    exit 1; \
	else \
	    grep -m1 '^FAIL fade-hue' $(BUILD)/gfx_conic_negctl.log; \
	    echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/gfx_conic_negctl.log) fade-hue assertion(s) fail with straight lerp"; \
	fi

test-gfx-gradient-conic: test-gfx-gradient-conic-negctl $(BUILD)/gfx_conic_test
	$(BUILD)/gfx_conic_test

ci-host: test-gfx-gradient-conic
