# ol_ui interaction core: hit testing, focus order, capture, slider drag,
# disabled-slot feedback and argument checks.
OL_UI_FIXES_DEPS := tests/unit/openlogit_ui_test.c c/lib/gfx/ui/interaction.c $(GFX_SRC) $(GFX_HEADERS)
$(BUILD)/ol_ui_fixes_test: $(OL_UI_FIXES_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/ol_ui_fixes_neg: $(OL_UI_FIXES_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -DOL_UI_HOVER_IGNORES_ENABLED $(GFX_INC) $< $(GFX_SRC) -lm -o $@
test-ol-ui-fixes-negctl: $(BUILD)/ol_ui_fixes_neg
	@rc=0; $< > $(BUILD)/ol-ui-fixes-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL disabled-hover' $(BUILD)/ol-ui-fixes-neg.log
test-ol-ui-fixes: test-ol-ui-fixes-negctl $(BUILD)/ol_ui_fixes_test
	$(BUILD)/ol_ui_fixes_test
ci-host: test-ol-ui-fixes
.PHONY: test-ol-ui-fixes test-ol-ui-fixes-negctl
