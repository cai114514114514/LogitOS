# tests/aui-motion-adopt.mk -- aui_anim curves via ol_ease256_ex, aui_spring via
# ol_spring256, aui_anim_color via ol_mix_argb_premul (c/apps/gui/aui_motion.*).
# The harness #includes the real aui.c and drives its frame clock directly.
# The control builds the SAME sources and flags plus -DAUI_MOTION_LEGACY3 (the
# old three-curve map) and must fail 'FAIL curve' and nothing else.

.PHONY: test-aui-motion-adopt test-aui-motion-adopt-negctl

AMA_SRC   := tests/unit/aui_motion_test.c $(AUI_TL_SRC) $(GFX_SRC)
AMA_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=aui_mt_sys -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
AMA_DEPS  := $(AMA_SRC) c/apps/gui/aui.c c/apps/gui/aui_motion.inc c/apps/gui/aui_motion.h $(GFX_HEADERS)

$(BUILD)/aui_motion_test: $(AMA_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(AMA_FLAGS) $(AMA_SRC) -lm -o $@

$(BUILD)/aui_motion_negctl: $(AMA_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(AMA_FLAGS) -DAUI_MOTION_LEGACY3 $(AMA_SRC) -lm -o $@

test-aui-motion-adopt-negctl: $(BUILD)/aui_motion_negctl
	@rc=0; $(BUILD)/aui_motion_negctl > $(BUILD)/aui_motion_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/aui_motion_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL curve' $(BUILD)/aui_motion_negctl.log || { cat $(BUILD)/aui_motion_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL curve' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/aui_motion_negctl.log | grep -qv '^FAIL curve'; then \
		cat $(BUILD)/aui_motion_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: legacy 3-curve map fails: $$(grep -m1 '^FAIL curve' $(BUILD)/aui_motion_negctl.log)"

test-aui-motion-adopt: test-aui-motion-adopt-negctl $(BUILD)/aui_motion_test
	@$(BUILD)/aui_motion_test

ci-host: test-aui-motion-adopt
