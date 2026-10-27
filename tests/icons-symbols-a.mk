# tests/icons-symbols-a.mk -- icon set A: stroke ops, 64-entry LRU, 12 UI symbols.
# The control builds the same sources with -DICON_CACHE_SINGLE (a one-entry
# cache, i.e. the old thrash); alternating 16/32 px draws then miss every time
# and the hit-rate check must fail with "FAIL hitrate".
ISA_SRC := tests/unit/icons_test.c c/kernel/gui/icons.c $(GFX_SRC)
ISA_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 -DLOGIT_SYS_HOOK=ic_test_sys \
	-Ic/kernel/gui -Ic/kernel/gui/fb -Ic/kernel/cpu -Ic/apps -Iinclude -Iinclude/abi $(GFX_INC)
ISA_DEPS := $(ISA_SRC) $(GFX_HEADERS) c/kernel/gui/icons.h c/apps/logit.h

$(BUILD)/icons_symbols_a_test: $(ISA_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(ISA_FLAGS) $(ISA_SRC) -lm -o $@

$(BUILD)/icons_symbols_a_negctl: $(ISA_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(ISA_FLAGS) -DICON_CACHE_SINGLE $(ISA_SRC) -lm -o $@

test-icons-symbols-a-negctl: $(BUILD)/icons_symbols_a_negctl
	@rc=0; $(BUILD)/icons_symbols_a_negctl > $(BUILD)/icons-symbols-a-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/icons-symbols-a-negctl.log; exit 1; }
	@grep -q '^FAIL hitrate$$' $(BUILD)/icons-symbols-a-negctl.log || { echo "negctl did not fail with 'FAIL hitrate'"; cat $(BUILD)/icons-symbols-a-negctl.log; exit 1; }
	@echo 'icons-symbols-a control: one-entry cache failed FAIL hitrate.'

test-icons-symbols-a: test-icons-symbols-a-negctl $(BUILD)/icons_symbols_a_test
	$(BUILD)/icons_symbols_a_test

ci-host: test-icons-symbols-a
.PHONY: test-icons-symbols-a test-icons-symbols-a-negctl
