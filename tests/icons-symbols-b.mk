# tests/icons-symbols-b.mk -- icon set B: 18 toolbar/sidebar/menu symbols.
# The control builds the same sources with -DICON_TABLE_NEGCTL, which drops the
# stroke ops of the set-B icons; the ink-area band must then fail with
# "FAIL ink-area".
ISB_SRC := tests/unit/icons_b_test.c c/kernel/gui/icons.c $(GFX_SRC)
ISB_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 -DLOGIT_SYS_HOOK=ic_test_sys \
	-Ic/kernel/gui -Ic/kernel/gui/fb -Ic/kernel/cpu -Ic/apps -Iinclude -Iinclude/abi $(GFX_INC)
ISB_DEPS := $(ISB_SRC) $(GFX_HEADERS) c/kernel/gui/icons.h c/apps/logit.h

$(BUILD)/icons_symbols_b_test: $(ISB_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(ISB_FLAGS) $(ISB_SRC) -lm -o $@

$(BUILD)/icons_symbols_b_negctl: $(ISB_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(ISB_FLAGS) -DICON_TABLE_NEGCTL $(ISB_SRC) -lm -o $@

test-icons-symbols-b-negctl: $(BUILD)/icons_symbols_b_negctl
	@rc=0; $(BUILD)/icons_symbols_b_negctl > $(BUILD)/icons-symbols-b-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/icons-symbols-b-negctl.log; exit 1; }
	@grep -q '^FAIL ink-area$$' $(BUILD)/icons-symbols-b-negctl.log || { echo "negctl did not fail with 'FAIL ink-area'"; cat $(BUILD)/icons-symbols-b-negctl.log; exit 1; }
	@echo 'icons-symbols-b control: stroke ops dropped, failed FAIL ink-area.'

test-icons-symbols-b: test-icons-symbols-b-negctl $(BUILD)/icons_symbols_b_test
	$(BUILD)/icons_symbols_b_test

ci-host: test-icons-symbols-b
.PHONY: test-icons-symbols-b test-icons-symbols-b-negctl
