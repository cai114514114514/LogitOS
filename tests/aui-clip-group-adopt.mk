# tests/aui-clip-group-adopt.mk -- rounded clips and opacity groups in aui
# (c/apps/gui/aui.c): aui_clip_push_round / aui_clip_pop / aui_fade_begin /
# aui_fade_end on SYS_GUI_CLIP_PUSH/POP and GROUP_BEGIN/END, the faux-fade sites
# (tooltip, popup, dialog sheet, disabled controls) on a real group, and rounded
# scroll containers.
#
# tests/unit/aui_clip_group_test.c #includes the real aui.c over the aui-host
# transport with tests/unit/aui_host/v2_state.c registered, which #includes the REAL
# c/kernel/gui/wm_draw2_state.c on a host wm2_env (clip ops on the harness surface
# clip, alloc = malloc). Checks: a disabled button over a gradient is one group at
# AUI_DISABLED_ALPHA (102, design 4) whose pixels equal the display_group oracle; push/pop balance
# per frame (aui's leak counter and SYS_GUI2_INFO a=1); a rounded scroll container
# clips its corners at 100/125/150%; OL_LIMIT falls back; 8 containers nest; an
# alpha-only group change reaches the flush rect.
#
# THE CONTROL builds the SAME sources and flags plus -DAUI_FADE_LEGACY_MIX (aui_fade_*
# ignores the kernel's GROUP bit and mixes colours). The control build runs only the
# fade-group check, which must report 'FAIL fade-group' -- and nothing else. A crash
# or a link error is not that failure.

.PHONY: test-aui-clip-group-adopt test-aui-clip-group-adopt-negctl

ACG_SRC   := tests/unit/aui_clip_group_test.c tests/unit/aui_host/aui_host.c \
             tests/unit/aui_host/v2_state.c $(AUI_TL_SRC) $(GFX_SRC)
ACG_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=aui_host_sys \
             -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
ACG_DEPS  := $(ACG_SRC) c/apps/gui/aui.c c/apps/gui/aui.h c/apps/gui/aui_paint.inc c/apps/gui/aui_damage.h \
             c/kernel/gui/wm_draw2_state.c tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/aui_clip_group_test: $(ACG_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ACG_FLAGS) $(ACG_SRC) -lm -o $@

$(BUILD)/aui_clip_group_negctl: $(ACG_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ACG_FLAGS) -DAUI_FADE_LEGACY_MIX $(ACG_SRC) -lm -o $@

test-aui-clip-group-adopt-negctl: $(BUILD)/aui_clip_group_negctl
	@rc=0; $(BUILD)/aui_clip_group_negctl > $(BUILD)/aui_clip_group_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/aui_clip_group_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL fade-group' $(BUILD)/aui_clip_group_negctl.log || { cat $(BUILD)/aui_clip_group_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL fade-group' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/aui_clip_group_negctl.log | grep -qv '^FAIL fade-group'; then \
		cat $(BUILD)/aui_clip_group_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: legacy colour mixing fails: $$(grep -m1 '^FAIL fade-group' $(BUILD)/aui_clip_group_negctl.log)"

test-aui-clip-group-adopt: test-aui-clip-group-adopt-negctl $(BUILD)/aui_clip_group_test
	@$(BUILD)/aui_clip_group_test

ci-host: test-aui-clip-group-adopt
