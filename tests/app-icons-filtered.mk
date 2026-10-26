# tests/app-icons-filtered.mk -- app_icon_cache.h (LRU tile cache + BOX tile).
# The control builds the same test with -DAPP_ICON_NEAREST (tiles are point
# sampled); the area-average check must fail with "FAIL box-tile".
AIF_SRC := tests/unit/app_icon_cache_test.c $(GFX_SRC)
AIF_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 -Ic/kernel/gui $(GFX_INC)
AIF_DEPS := $(AIF_SRC) $(GFX_HEADERS) c/kernel/gui/app_icon_cache.h

$(BUILD)/app_icon_cache_test: $(AIF_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(AIF_FLAGS) $(AIF_SRC) -lm -lpthread -o $@

$(BUILD)/app_icon_cache_negctl: $(AIF_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(AIF_FLAGS) -DAPP_ICON_NEAREST $(AIF_SRC) -lm -lpthread -o $@

test-app-icons-filtered-negctl: $(BUILD)/app_icon_cache_negctl
	@rc=0; $(BUILD)/app_icon_cache_negctl > $(BUILD)/app-icons-filtered-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/app-icons-filtered-negctl.log; exit 1; }
	@grep -q '^FAIL box-tile$$' $(BUILD)/app-icons-filtered-negctl.log || { echo "negctl did not fail with 'FAIL box-tile'"; cat $(BUILD)/app-icons-filtered-negctl.log; exit 1; }
	@echo 'app-icons-filtered control: point-sampled tile failed FAIL box-tile.'

test-app-icons-filtered: test-app-icons-filtered-negctl $(BUILD)/app_icon_cache_test
	$(BUILD)/app_icon_cache_test

ci-host: test-app-icons-filtered
.PHONY: test-app-icons-filtered test-app-icons-filtered-negctl
