# tests/aui-shadow-adopt.mk -- aui_shadow_ex / aui_shadow / aui_card and the new
# aui_shadow2 / aui_elevation / aui_glow lower to SYS_GUI_SHADOW2 when the kernel
# reports LOGIT_GUI2_F_SHADOW2 (c/apps/gui/aui_paint.inc). tests/unit/aui_shadow_test.c
# drives the real ring-3 aui.c over the aui-host transport with v2_fx.c, which
# #includes the REAL c/kernel/gui/wm_draw2_fx.c: the call shape of an elevation
# (aui_elevation 1..4 = E1/E2/E3/E6 and aui_shadow 1..3 = E1/E2/E3 of design 3.3,
# a key and an ambient shadow in the shadow ink), the fields of a general
# shadow2, an unclamped 20 pt sigma against ol_display_shadow_ex at 100..200%,
# damage coverage (3 sigma + offset) and the INFO=0 fallback.
#
# THE CONTROL builds the SAME sources and flags plus -DAUI_SHADOW_LEGACY (aui
# behaves as if the kernel never reported SHADOW2). An elevation is then drawn by
# the tile path and issues no SHADOW2: the call-shape check must report
# 'FAIL elevation-calls' -- and nothing else (the control build runs only that
# check). A crash or a link error is not that failure.

.PHONY: test-aui-shadow-adopt test-aui-shadow-adopt-negctl

ASH_SRC   := tests/unit/aui_shadow_test.c tests/unit/aui_host/aui_host.c tests/unit/aui_host/aui_host_scenes.c \
             c/apps/gui/aui.c tests/unit/aui_host/v2_fx.c $(AUI_TL_SRC) $(GFX_SRC)
ASH_FLAGS := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=aui_host_sys -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
ASH_DEPS  := $(ASH_SRC) c/apps/gui/aui_paint.inc c/apps/gui/aui_paint.h c/apps/gui/aui_damage.h \
             c/kernel/gui/wm_draw2_fx.c tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/aui_shadow_test: $(ASH_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ASH_FLAGS) $(ASH_SRC) -lm -o $@

$(BUILD)/aui_shadow_negctl: $(ASH_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ASH_FLAGS) -DAUI_SHADOW_LEGACY $(ASH_SRC) -lm -o $@

test-aui-shadow-adopt-negctl: $(BUILD)/aui_shadow_negctl
	@rc=0; $(BUILD)/aui_shadow_negctl > $(BUILD)/aui_shadow_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/aui_shadow_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL elevation-calls' $(BUILD)/aui_shadow_negctl.log || { cat $(BUILD)/aui_shadow_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL elevation-calls' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/aui_shadow_negctl.log | grep -v '^FAIL aui-shadow-adopt' | grep -qv '^FAIL elevation-calls'; then \
		cat $(BUILD)/aui_shadow_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: the tile path fails: $$(grep -m1 '^FAIL elevation-calls' $(BUILD)/aui_shadow_negctl.log)"

test-aui-shadow-adopt: test-aui-shadow-adopt-negctl $(BUILD)/aui_shadow_test
	@$(BUILD)/aui_shadow_test

ci-host: test-aui-shadow-adopt
