# tests/ui-aui-overlays.mk -- the 「澄」 overlays of the aui toolkit (ui-aui-overlays):
# the shared menu panel (select popup, menu-bar dropdown, aui_context_menu,
# aui_menu_panel), aui_popover_*, the sheet (aui_dialog_*) and aui_alert
# (design 4.9, 4.10).
#
# The binary builds the REAL ring-3 aui.c with -DLOGIT_SYS_HOOK=ov_sys
# (tests/unit/ui_aui_core_hook.h's recorder plus this test's icon and text-run
# recorders) and runs each case in a forked child. Checks are listed in the
# test's header. THE CONTROL builds the SAME sources and flags plus
# -DUI_OVERLAYS_NEGCTL (the plate re-materialises on every animation frame)
# and must exit 1 with 'FAIL menu-snapshot' and no other failing check.

.PHONY: test-ui-aui-overlays test-ui-aui-overlays-negctl

UAOV_SRC   := tests/unit/ui_aui_overlays_test.c c/apps/gui/aui.c $(AUI_TL_SRC) $(GFX_SRC)
UAOV_FLAGS := -O1 -g -Wall -Wextra -DLOGIT_SYS_HOOK=ov_sys -Itests/unit -Ic/apps/gui -Ic/apps \
              $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UAOV_DEPS  := tests/unit/ui_aui_core_hook.h c/apps/gui/aui.h c/apps/logit.h $(AUI_INC_SRC) \
              $(wildcard c/include/ui/*.h) $(GFX_HEADERS)

$(BUILD)/ui_aui_overlays/test: $(UAOV_SRC) $(UAOV_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAOV_FLAGS) $(UAOV_SRC) -lm -o $@

$(BUILD)/ui_aui_overlays/negctl: $(UAOV_SRC) $(UAOV_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAOV_FLAGS) -DUI_OVERLAYS_NEGCTL $(UAOV_SRC) -lm -o $@

test-ui-aui-overlays-negctl: $(BUILD)/ui_aui_overlays/negctl
	@rc=0; $(BUILD)/ui_aui_overlays/negctl > $(BUILD)/ui_aui_overlays/negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_overlays/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict, not a crash or a pass)"; exit 1; fi; \
	grep -q '^FAIL menu-snapshot' $(BUILD)/ui_aui_overlays/negctl.log || { cat $(BUILD)/ui_aui_overlays/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL menu-snapshot' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_overlays/negctl.log | grep -v '^FAIL ui-aui-overlays:' | grep -qv '^FAIL menu-snapshot'; then \
		cat $(BUILD)/ui_aui_overlays/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than menu-snapshot failed"; exit 1; fi; \
	echo "negative control ok: re-materialising per frame fails: $$(grep -m1 '^FAIL menu-snapshot' $(BUILD)/ui_aui_overlays/negctl.log)"

test-ui-aui-overlays: test-ui-aui-overlays-negctl $(BUILD)/ui_aui_overlays/test
	@$(BUILD)/ui_aui_overlays/test

ci-host: test-ui-aui-overlays
