# tests/aui_damage.mk -- aui flushes a SET of damage rectangles per frame, not
# one bounding box (c/apps/gui/aui_damage.h). Coverage is checked as a
# randomised property; tightness by the cases that motivated it. The control
# restores the single box, keeps coverage, and must fail tightness.

.PHONY: test-aui-damage test-aui-damage-negctl test-aui-damage-halo test-aui-damage-halo-negctl

test-aui-damage-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAUI_FLUSH_SINGLE_RECT -Ic/apps/gui \
		-o $(BUILD)/aui_damage_negctl tests/unit/aui_damage_test.c
	@if $(BUILD)/aui_damage_negctl > $(BUILD)/aui_damage_negctl.log 2>&1; then \
		cat $(BUILD)/aui_damage_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: the suite passes with one bounding box"; exit 1; \
	elif grep -q '^FAIL coverage' $(BUILD)/aui_damage_negctl.log; then \
		cat $(BUILD)/aui_damage_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: coverage broke"; exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/aui_damage_negctl.log) tightness checks fail on one box"; \
	fi

test-aui-damage: test-aui-damage-negctl test-aui-damage-halo
	@$(CC) -O2 -Wall -Wextra -Ic/apps/gui -o $(BUILD)/aui_damage_test tests/unit/aui_damage_test.c
	@$(BUILD)/aui_damage_test

ci-host: test-aui-damage

# Halo coverage (ad_add_halo, ad_halo_shadow/stroke/glass): the REAL primitives
# (ol_display_shadow_ex, the gfx stroker, ol_display_liquid_glass_ex at every
# tier) draw into a host display and every pixel they write -- or, for glass,
# every backdrop pixel they sample -- must lie inside the reported set. Built
# with -DGFX_NCTX=2 and no fb.c. The control makes every halo 0 and must fail
# 'FAIL coverage halo'; it uses the same sources and flags as the positive.
ADH_SRC := tests/unit/aui_damage_test.c $(GFX_SRC)
ADH_FLAGS := -O2 -Wall -Wextra -DGFX_NCTX=2 -DAD_HALO_TEST -Ic/apps/gui $(GFX_INC)
ADH_DEPS := $(ADH_SRC) c/apps/gui/aui_damage.h $(GFX_HEADERS)

$(BUILD)/aui_damage_halo_test: $(ADH_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ADH_FLAGS) $(ADH_SRC) -lm -o $@

$(BUILD)/aui_damage_halo_negctl: $(ADH_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ADH_FLAGS) -DAD_HALO_NEGCTL_ZERO $(ADH_SRC) -lm -o $@

test-aui-damage-halo-negctl: $(BUILD)/aui_damage_halo_negctl
	@rc=0; $(BUILD)/aui_damage_halo_negctl > $(BUILD)/aui_damage_halo_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/aui_damage_halo_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	grep -q '^FAIL coverage halo' $(BUILD)/aui_damage_halo_negctl.log || { cat $(BUILD)/aui_damage_halo_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: no 'FAIL coverage halo' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/aui_damage_halo_negctl.log | grep -qv '^FAIL coverage halo'; then \
		cat $(BUILD)/aui_damage_halo_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON"; exit 1; fi; \
	echo "negative control ok: zero halos fail: $$(grep -m1 '^FAIL coverage halo' $(BUILD)/aui_damage_halo_negctl.log)"

test-aui-damage-halo: test-aui-damage-halo-negctl $(BUILD)/aui_damage_halo_test
	@$(BUILD)/aui_damage_halo_test
