# tests/aui-glass-controls.mk -- design 4.3: buttons are capsules with NO glass (a Secondary
# 80x28 = 0 GLASS2, <= 3 PATH, exactly 2 SHADOW2; Primary paints a gradient; the AI stroke's
# paint turns on hover and stops). The segmented control and the switch make no GLASS2
# either (design 4.5 / 4.6), and a new switch is flushed whole. tests/unit/aui_glass_controls_test.c drives the real ring-3 aui.c over the aui
# host with v2_fx.c and v2_path.c (used unchanged), which #include the REAL handlers.
#
# THE CONTROL builds the SAME sources and flags plus -DAUI_BTN_SECONDARY_GLASS (glass_face is
# back under a Secondary button): only the button checks run and it must report
# 'FAIL secondary-glass2' and nothing else.

.PHONY: test-aui-glass-controls test-aui-glass-controls-negctl

AGC_SRC   := tests/unit/aui_glass_controls_test.c tests/unit/aui_host/aui_host.c tests/unit/aui_host/aui_host_scenes.c \
             c/apps/gui/aui.c tests/unit/aui_host/v2_fx.c tests/unit/aui_host/v2_path.c $(AUI_TL_SRC) $(GFX_SRC)
AGC_FLAGS := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=aui_host_sys -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
AGC_DEPS  := $(AGC_SRC) c/apps/gui/aui_button.inc c/apps/gui/aui_paint.inc c/apps/gui/aui_paint.h c/apps/gui/aui_damage.h \
             c/kernel/gui/wm_draw2_fx.c c/kernel/gui/wm_draw2_path.c tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/aui_glass_controls_test: $(AGC_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(AGC_FLAGS) $(AGC_SRC) -lm -o $@

$(BUILD)/aui_glass_controls_negctl: $(AGC_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(AGC_FLAGS) -DAUI_BTN_SECONDARY_GLASS $(AGC_SRC) -lm -o $@

test-aui-glass-controls-negctl: $(BUILD)/aui_glass_controls_negctl
	@rc=0; $(BUILD)/aui_glass_controls_negctl > $(BUILD)/aui_glass_controls_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/aui_glass_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL secondary-glass2' $(BUILD)/aui_glass_controls_negctl.log || { cat $(BUILD)/aui_glass_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL secondary-glass2' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/aui_glass_controls_negctl.log | grep -v '^FAIL aui-glass-controls' | grep -qv '^FAIL secondary-glass2'; then \
		cat $(BUILD)/aui_glass_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: a glass Secondary button fails: $$(grep -m1 '^FAIL secondary-glass2' $(BUILD)/aui_glass_controls_negctl.log)"

test-aui-glass-controls: test-aui-glass-controls-negctl $(BUILD)/aui_glass_controls_test
	@$(BUILD)/aui_glass_controls_test

ci-host: test-aui-glass-controls
