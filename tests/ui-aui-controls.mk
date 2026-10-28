# tests/ui-aui-controls.mk -- design 4.6 / 4.7 / 4.14 / 4.15 / 4.17: switch (38 x 22,
# snappy knob, no glass), checkbox (16, PATH tick 1.75 pt), radio, slider
# (aui_slider_ex: keys, 44-wide tabular value column), stepper, progress,
# spinner (270 degree LINEAR-paint arc, <= 20 / 12 Hz, own-rect damage),
# tooltip (one dark style) and badge (c/apps/gui/aui_controls.{h,inc}).
#
# tests/unit/ui_aui_controls_test.c #includes the real aui.c over the aui host
# with the REAL SYS_GUI_PATH (ui_aui_controls_path.c, a spy-carrying copy of
# v2_path.c), SHADOW2/GLASS2 (v2_fx.c) and CLIP/GROUP (v2_state.c) handlers.
# The control builds the SAME sources and flags plus -DUI_CONTROLS_NEGCTL (the
# old 44 x 24 switch) and must fail 'switch-size' and nothing else.

.PHONY: test-ui-aui-controls test-ui-aui-controls-negctl

UAC_DIR   := tests/unit/aui_host
UAC_SRC   := tests/unit/ui_aui_controls_test.c tests/unit/ui_aui_controls_path.c $(UAC_DIR)/aui_host.c \
             $(UAC_DIR)/v2_fx.c $(UAC_DIR)/v2_state.c $(UAC_DIR)/v2_text.c $(AUI_TL_SRC) $(GFX_SRC)
UAC_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=aui_host_sys -I$(UAC_DIR) -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UAC_DEPS  := $(UAC_SRC) c/apps/gui/aui.c c/apps/gui/aui_controls.inc c/apps/gui/aui_controls.h \
             c/kernel/gui/wm_draw2_fx.c c/kernel/gui/wm_draw2_path.c $(UAC_DIR)/aui_host.h $(GFX_HEADERS)

$(BUILD)/ui_aui_controls_test: $(UAC_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAC_FLAGS) $(UAC_SRC) -lm -o $@

$(BUILD)/ui_aui_controls_negctl: $(UAC_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAC_FLAGS) -DUI_CONTROLS_NEGCTL $(UAC_SRC) -lm -o $@

test-ui-aui-controls-negctl: $(BUILD)/ui_aui_controls_negctl
	@rc=0; $(BUILD)/ui_aui_controls_negctl > $(BUILD)/ui_aui_controls_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL switch-size' $(BUILD)/ui_aui_controls_negctl.log || { cat $(BUILD)/ui_aui_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL switch-size' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_controls_negctl.log | grep -qv '^FAIL switch-size'; then \
		cat $(BUILD)/ui_aui_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: the 44 x 24 switch fails: $$(grep -m1 '^FAIL switch-size' $(BUILD)/ui_aui_controls_negctl.log)"

test-ui-aui-controls: test-ui-aui-controls-negctl $(BUILD)/ui_aui_controls_test
	@$(BUILD)/ui_aui_controls_test

ci-host: test-ui-aui-controls
