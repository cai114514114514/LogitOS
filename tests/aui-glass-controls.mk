# tests/aui-glass-controls.mk -- buttons, the segmented pill and the toggle track
# are ONE SYS_GUI_GLASS2 (SYMMETRIC rim, THIN blur, tint = the face colour) when
# the kernel reports GLASS2 and ui.reduce_transparency is off; otherwise the
# gradient face (c/apps/gui/aui.c glass_face). tests/unit/aui_glass_controls_test.c
# drives the real ring-3 aui.c over the aui host with v2_fx.c (used unchanged),
# which #includes the REAL wm_draw2_fx.c, so the rim is the real material.
#
# THE CONTROL builds the SAME sources and flags plus -DAUI_GLASS_DIRECTIONAL (aui
# asks for the directional rim, whose two grey crescents break the rim): only the
# rim check runs there and it must report 'FAIL rim-uniform' and nothing else.

.PHONY: test-aui-glass-controls test-aui-glass-controls-negctl

AGC_SRC   := tests/unit/aui_glass_controls_test.c tests/unit/aui_host/aui_host.c tests/unit/aui_host/aui_host_scenes.c \
             c/apps/gui/aui.c tests/unit/aui_host/v2_fx.c $(AUI_TL_SRC) $(GFX_SRC)
AGC_FLAGS := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=aui_host_sys -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
AGC_DEPS  := $(AGC_SRC) c/apps/gui/aui_paint.inc c/apps/gui/aui_paint.h c/apps/gui/aui_damage.h \
             c/kernel/gui/wm_draw2_fx.c tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/aui_glass_controls_test: $(AGC_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(AGC_FLAGS) $(AGC_SRC) -lm -o $@

$(BUILD)/aui_glass_controls_negctl: $(AGC_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(AGC_FLAGS) -DAUI_GLASS_DIRECTIONAL $(AGC_SRC) -lm -o $@

test-aui-glass-controls-negctl: $(BUILD)/aui_glass_controls_negctl
	@rc=0; $(BUILD)/aui_glass_controls_negctl > $(BUILD)/aui_glass_controls_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/aui_glass_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL rim-uniform' $(BUILD)/aui_glass_controls_negctl.log || { cat $(BUILD)/aui_glass_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL rim-uniform' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/aui_glass_controls_negctl.log | grep -v '^FAIL aui-glass-controls' | grep -qv '^FAIL rim-uniform'; then \
		cat $(BUILD)/aui_glass_controls_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: the directional rim fails: $$(grep -m1 '^FAIL rim-uniform' $(BUILD)/aui_glass_controls_negctl.log)"

test-aui-glass-controls: test-aui-glass-controls-negctl $(BUILD)/aui_glass_controls_test
	@$(BUILD)/aui_glass_controls_test

ci-host: test-aui-glass-controls
