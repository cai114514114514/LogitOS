# tests/ui-aui-core.mk -- the aui core of the 「澄」 overhaul (ui-aui-core):
# the paint API (aui_material / aui_backdrop / aui_image / aui_elev, design
# P4, 3.2, 3.3), the theme roles (aui_tok, appendix A), aui_scroll_offset and
# the aims (ui_aim.h + widget-level aims, design P13).
#
# Both binaries build the REAL ring-3 aui.c with -DLOGIT_SYS_HOOK=core_sys
# (tests/unit/ui_aui_core_hook.h records every call's struct, the flushes and
# the serial output) and run each case in a forked child. Checks are listed in
# each test's header.
#
#   test-ui-aui-core-api   tests/unit/ui_aui_core_test.c. THE CONTROL builds the
#                          same sources with -DUI_AUI_CORE_NEGCTL (aui asks for
#                          the DIRECTIONAL rim) and must exit 1 with 'FAIL
#                          material-rim' and no other failing check.
#   test-ui-aim            tests/unit/ui_aim_test.c. THE CONTROL is
#                          -DUI_AIM_NEGCTL (the fixed-size table that drops new
#                          names once full) and must exit 1 with 'FAIL aim-lru'
#                          and no other failing check.

.PHONY: test-ui-aui-core test-ui-aui-core-api test-ui-aui-core-api-negctl test-ui-aim test-ui-aim-negctl

UACORE_FLAGS := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=core_sys -Itests/unit -Ic/apps/gui -Ic/apps \
                $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UACORE_API_SRC := tests/unit/ui_aui_core_test.c c/apps/gui/aui.c $(AUI_TL_SRC) $(GFX_SRC)
UACORE_AIM_SRC := tests/unit/ui_aim_test.c c/apps/gui/aui.c $(AUI_TL_SRC) $(GFX_SRC)
UACORE_DEPS := tests/unit/ui_aui_core_hook.h c/apps/gui/aui.h c/apps/logit.h $(AUI_INC_SRC) \
               $(wildcard c/include/ui/*.h) $(GFX_HEADERS)

$(BUILD)/ui_aui_core/api_test: $(UACORE_API_SRC) $(UACORE_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UACORE_FLAGS) $(UACORE_API_SRC) -lm -o $@

$(BUILD)/ui_aui_core/api_negctl: $(UACORE_API_SRC) $(UACORE_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UACORE_FLAGS) -DUI_AUI_CORE_NEGCTL $(UACORE_API_SRC) -lm -o $@

$(BUILD)/ui_aui_core/aim_test: $(UACORE_AIM_SRC) $(UACORE_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UACORE_FLAGS) $(UACORE_AIM_SRC) -lm -o $@

$(BUILD)/ui_aui_core/aim_negctl: $(UACORE_AIM_SRC) $(UACORE_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UACORE_FLAGS) -DUI_AIM_NEGCTL $(UACORE_AIM_SRC) -lm -o $@

test-ui-aui-core-api-negctl: $(BUILD)/ui_aui_core/api_negctl
	@rc=0; $(BUILD)/ui_aui_core/api_negctl > $(BUILD)/ui_aui_core/api_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_core/api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict, not a crash or a pass)"; exit 1; fi; \
	grep -q '^FAIL material-rim' $(BUILD)/ui_aui_core/api_negctl.log || { cat $(BUILD)/ui_aui_core/api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL material-rim' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_core/api_negctl.log | grep -v '^FAIL ui-aui-core-api:' | grep -qv '^FAIL material-rim'; then \
		cat $(BUILD)/ui_aui_core/api_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than material-rim failed"; exit 1; fi; \
	echo "negative control ok: the directional rim fails: $$(grep -m1 '^FAIL material-rim' $(BUILD)/ui_aui_core/api_negctl.log)"

test-ui-aui-core-api: test-ui-aui-core-api-negctl $(BUILD)/ui_aui_core/api_test
	@$(BUILD)/ui_aui_core/api_test

test-ui-aim-negctl: $(BUILD)/ui_aui_core/aim_negctl
	@rc=0; $(BUILD)/ui_aui_core/aim_negctl > $(BUILD)/ui_aui_core/aim_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_core/aim_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict, not a crash or a pass)"; exit 1; fi; \
	grep -q '^FAIL aim-lru' $(BUILD)/ui_aui_core/aim_negctl.log || { cat $(BUILD)/ui_aui_core/aim_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL aim-lru' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_core/aim_negctl.log | grep -v '^FAIL ui-aim:' | grep -qv '^FAIL aim-lru'; then \
		cat $(BUILD)/ui_aui_core/aim_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than aim-lru failed"; exit 1; fi; \
	echo "negative control ok: the fixed-size table drops names: $$(grep -m1 '^FAIL aim-lru' $(BUILD)/ui_aui_core/aim_negctl.log)"

test-ui-aim: test-ui-aim-negctl $(BUILD)/ui_aui_core/aim_test
	@$(BUILD)/ui_aui_core/aim_test

test-ui-aui-core: test-ui-aui-core-api test-ui-aim

ci-host: test-ui-aui-core
