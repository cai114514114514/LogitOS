# SYS_GUI_SHADOW2 / SYS_GUI_GLASS2 handlers (c/kernel/gui/wm_draw2_fx.c) against
# the direct ol_display_shadow_ex / ol_display_liquid_glass_ex calls, inside a
# wm2 group, with bad arguments, at 125/150/200% and with reduce transparency.
#
# THE CONTROL builds the same sources with -DWM_DRAW2_FX_NO_SCALE (sigma not
# scaled by S(pct)): 'FAIL sigma-scale' must fire, and nothing else may fail.
WDFX_SRC := tests/unit/wm_draw2_fx_test.c c/kernel/gui/wm_draw2_fx.c c/kernel/gui/wm_draw2_state.c $(GFX_SRC)
WDFX_FLAGS := -O1 -g -Wall -Wextra $(GFX_INC) $(HOST_INCDIRS)
WDFX_DEPS := $(WDFX_SRC) $(GFX_HEADERS) c/kernel/gui/wm_draw2.h c/kernel/gui/wm2_scale.h include/abi/logit_abi.h

$(BUILD)/wm_draw2_fx_test: $(WDFX_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(WDFX_FLAGS) $(WDFX_SRC) -lm -o $@

$(BUILD)/wm_draw2_fx_negctl: $(WDFX_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(WDFX_FLAGS) -DWM_DRAW2_FX_NO_SCALE $(WDFX_SRC) -lm -o $@

test-wm-draw2-effects-negctl: $(BUILD)/wm_draw2_fx_negctl
	@rc=0; $(BUILD)/wm_draw2_fx_negctl > $(BUILD)/wm-draw2-effects-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/wm-draw2-effects-negctl.log; exit 1; }
	@grep -q '^FAIL sigma-scale' $(BUILD)/wm-draw2-effects-negctl.log || { echo "negctl did not fail with 'FAIL sigma-scale'"; cat $(BUILD)/wm-draw2-effects-negctl.log; exit 1; }
	@! grep '^FAIL' $(BUILD)/wm-draw2-effects-negctl.log | grep -qv '^FAIL sigma-scale' || { echo "negctl failed a check other than sigma-scale"; cat $(BUILD)/wm-draw2-effects-negctl.log; exit 1; }
	@echo "wm-draw2-effects control: unscaled sigma failed: $$(grep -m1 '^FAIL sigma-scale' $(BUILD)/wm-draw2-effects-negctl.log)"

test-wm-draw2-effects: test-wm-draw2-effects-negctl $(BUILD)/wm_draw2_fx_test
	$(BUILD)/wm_draw2_fx_test

ci-host: test-wm-draw2-effects

.PHONY: test-wm-draw2-effects test-wm-draw2-effects-negctl
