# tests/ui-aui-lists.mk -- design 4.5 / 4.8 / 4.11 (ui-aui-lists): the segmented
# control, aui_tabs as a segmented control, lists and tables (aui_table_ex, sort,
# merged selection, re-sort slide) and document tabs (aui_doctabs) of
# c/apps/gui/aui_lists.{h,inc}. The harness builds the REAL aui.c with
# -DLOGIT_SYS_HOOK=ul_sys and runs each case in a forked child (checks are listed
# in tests/unit/ui_aui_lists_test.c). The control builds the SAME sources and flags
# plus -DUI_LISTS_NEGCTL (the segmented thumb is ALSO drawn as glass) and must fail
# 'FAIL segmented-glass2' and nothing else.

.PHONY: test-ui-aui-lists test-ui-aui-lists-negctl

UAL_SRC   := tests/unit/ui_aui_lists_test.c c/apps/gui/aui.c $(AUI_TL_SRC) $(GFX_SRC)
UAL_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=ul_sys -Ic/apps/gui -Ic/apps \
             $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UAL_DEPS  := $(UAL_SRC) $(AUI_INC_SRC) c/apps/logit.h $(wildcard c/include/ui/*.h) $(GFX_HEADERS)

$(BUILD)/ui_aui_lists/test: $(UAL_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAL_FLAGS) $(UAL_SRC) -lm -o $@

$(BUILD)/ui_aui_lists/negctl: $(UAL_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UAL_FLAGS) -DUI_LISTS_NEGCTL $(UAL_SRC) -lm -o $@

test-ui-aui-lists-negctl: $(BUILD)/ui_aui_lists/negctl
	@rc=0; $(BUILD)/ui_aui_lists/negctl > $(BUILD)/ui_aui_lists/negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_lists/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL segmented-glass2' $(BUILD)/ui_aui_lists/negctl.log || { cat $(BUILD)/ui_aui_lists/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL segmented-glass2' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_lists/negctl.log | grep -v '^FAIL ui-aui-lists:' | grep -qv '^FAIL segmented-glass2'; then \
		cat $(BUILD)/ui_aui_lists/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: a glass thumb fails: $$(grep -m1 '^FAIL segmented-glass2' $(BUILD)/ui_aui_lists/negctl.log)"

test-ui-aui-lists: test-ui-aui-lists-negctl $(BUILD)/ui_aui_lists/test
	@$(BUILD)/ui_aui_lists/test

ci-host: test-ui-aui-lists
