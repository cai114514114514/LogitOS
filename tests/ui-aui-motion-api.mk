# tests/ui-aui-motion-api.mk -- design 3.8 / P6: aui_spring_ex with the five spring
# presets, aui_press_scale_q8 / _dock_q8 and aui_stagger_ms (c/apps/gui/aui_motion.*,
# tokens from c/include/ui/logit_motion.h). The harness #includes the real aui.c and
# drives its frame clock directly. The control builds the SAME sources and flags plus
# -DUI_MOTION_NEGCTL (bouncy preset critically damped, zeta 1.0) and must fail
# 'FAIL bouncy-overshoot' and nothing else.

.PHONY: test-ui-aui-motion-api test-ui-aui-motion-api-negctl

UAMA_SRC   := tests/unit/ui_aui_motion_api_test.c $(AUI_TL_SRC) $(GFX_SRC)
UAMA_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=uam_sys -Ic/apps/gui -Ic/apps \
              $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UAMA_DEPS  := $(UAMA_SRC) c/apps/gui/aui.c c/apps/gui/aui_motion.inc c/apps/gui/aui_motion.h \
              c/include/ui/logit_motion.h $(GFX_HEADERS)

$(BUILD)/ui_aui_motion_api_test: $(UAMA_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAMA_FLAGS) $(UAMA_SRC) -lm -o $@

$(BUILD)/ui_aui_motion_api_negctl: $(UAMA_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAMA_FLAGS) -DUI_MOTION_NEGCTL $(UAMA_SRC) -lm -o $@

test-ui-aui-motion-api-negctl: $(BUILD)/ui_aui_motion_api_negctl
	@rc=0; $(BUILD)/ui_aui_motion_api_negctl > $(BUILD)/ui_aui_motion_api_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_motion_api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL bouncy-overshoot' $(BUILD)/ui_aui_motion_api_negctl.log || { cat $(BUILD)/ui_aui_motion_api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL bouncy-overshoot' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_motion_api_negctl.log | grep -qv '^FAIL bouncy-overshoot'; then \
		cat $(BUILD)/ui_aui_motion_api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: critically damped bouncy fails: $$(grep -m1 '^FAIL bouncy-overshoot' $(BUILD)/ui_aui_motion_api_negctl.log)"

test-ui-aui-motion-api: test-ui-aui-motion-api-negctl $(BUILD)/ui_aui_motion_api_test
	@$(BUILD)/ui_aui_motion_api_test

ci-host: test-ui-aui-motion-api
